#!/bin/bash
# The register edit path (ADR 0014): POST /api/split/<split-guid>.
#
# Usage: register-edit.sh <build-dir> <source-dir>
#
# Generates the 190-row demo book with web/test/make-demo-book.py and checks:
#
#   * read-only (the default): every editable field is refused with a named
#     403 that says how to start the server writable, the method check is 405,
#     a tokenless request is 404, and the book is byte-for-byte untouched;
#   * writable: GET /api/register says editable:true; a refused edit does not
#     dirty the book (a save after the bad-value battery answers changed:false);
#   * a good edit answers with the updated row, the register reflects it, and
#     the running balances and amounts are unchanged (the text fields cannot
#     move money); an empty string clears a field;
#   * bad values are named 400s and change nothing: bad-type, too-long,
#     bad-value, bad-json, no-fields, unknown-field, unsupported-field (amount),
#     unknown-split, and a body with one good and one bad field applies neither;
#   * a body over 1 MiB is a named 413;
#   * a save after an edit names a backup that is byte-for-byte the pre-edit
#     book, and a fresh read-only server re-opens the book and sees the edit;
#   * an edit that sets the value it already had leaves the book clean
#     (a save then answers changed:false);
#   * a locked book refuses the edit with 409 naming the .LCK, and does not
#     change the book.
#
# Exit status: 0 when every check passes, 1 otherwise.
set -u

BUILD=${1:?usage: register-edit.sh <build-dir> <source-dir>}
SRC=${2:?usage: register-edit.sh <build-dir> <source-dir>}

SCHEMAS="$BUILD/share/glib-2.0/schemas"
CCACHE=$(ls -d "$BUILD"/lib/*/guile/3.0/site-ccache 2>/dev/null | head -1)

fail() { echo "FAIL: $*" >&2; exit 1; }

[ -x "$BUILD/bin/gnc-webd" ] || fail "gnc-webd was not built"
[ -f "$SRC/web/test/make-demo-book.py" ] || fail "make-demo-book.py not found"
command -v curl >/dev/null || fail "curl is required"
command -v python3 >/dev/null || fail "python3 is required"
if [ ! -f "$SCHEMAS/gschemas.compiled" ]; then
    SCHEMAS=""
fi

TMP=$(mktemp -d)
PIDS=()
cleanup() {
    for pid in "${PIDS[@]}"; do kill "$pid" 2>/dev/null; done
    rm -rf "$TMP"
}
trap cleanup EXIT

BOOK="$TMP/book.gnucash"
python3 "$SRC/web/test/make-demo-book.py" "$BOOK" >/dev/null \
    || fail "could not generate the demo book"
cp "$BOOK" "$TMP/pre-edit.gnucash"

# start <log> [extra gnc-webd args...] -- sets URL and SERVER_PID.
start() {
    local log=$1
    shift
    local args=()
    [ -f "$BUILD/lib/gnucash/libgncmod-backend-xml.so" ] \
        && args+=(--backend "$BUILD/lib/gnucash/libgncmod-backend-xml.so")
    env GNC_UNINSTALLED=YES GNC_BUILDDIR="$BUILD" \
        ${SCHEMAS:+GSETTINGS_SCHEMA_DIR="$SCHEMAS"} \
        LD_LIBRARY_PATH="$BUILD/lib:$BUILD/lib/gnucash" \
        GNC_MODULE_PATH="$BUILD/lib/gnucash" \
        GUILE_LOAD_PATH="$BUILD/share/guile/site/3.0" \
        GUILE_LOAD_COMPILED_PATH="${CCACHE:-}" \
        GNC_WEBUI_DIR="$SRC/web/ui" \
        GNC_WEBUI_REPORTS="$TMP/reports.json" \
        "$BUILD/bin/gnc-webd" "${args[@]}" "$@" "$BOOK" > "$log" 2>&1 &
    SERVER_PID=$!
    PIDS+=("$SERVER_PID")

    URL=""
    for _ in $(seq 1 300); do
        URL=$(grep -oE 'http://127\.0\.0\.1:[0-9]+/[0-9a-f]+/' "$log" | head -1)
        [ -n "$URL" ] && break
        kill -0 "$SERVER_PID" 2>/dev/null || break
        sleep 0.2
    done
    [ -n "$URL" ] || { cat "$log" >&2; fail "server did not start"; }
}

# stop <pid> -- ordinary SIGTERM; the server is expected to release its lock.
stop() {
    local pid=${1:-$SERVER_PID}
    kill "$pid" 2>/dev/null
    for _ in $(seq 1 100); do
        kill -0 "$pid" 2>/dev/null || break
        sleep 0.1
    done
    wait "$pid" 2>/dev/null || true
}

# request <method> <path> [body-file] -- sets CODE and BODY (body in $TMP/body).
request() {
    local method=$1 path=$2 body=${3:-}
    if [ -n "$body" ]; then
        CODE=$(curl -sS -o "$TMP/body" -w '%{http_code}' -X "$method" \
            -H 'Content-Type: application/json' \
            --data-binary "@$body" "${URL}${path}") \
            || fail "curl $method $path failed"
    else
        CODE=$(curl -sS -o "$TMP/body" -w '%{http_code}' -X "$method" \
            "${URL}${path}") || fail "curl $method $path failed"
    fi
    BODY=$(cat "$TMP/body")
}

# split_field <json-file> <split-guid> <field> -- the row's field, unquoted.
split_field() {
    python3 -c '
import json, sys
data = json.load(open(sys.argv[1]))
row = next((r for r in data["splits"] if r["guid"] == sys.argv[2]), None)
if row is None:
    sys.exit("no row for split " + sys.argv[2])
print(row[sys.argv[3]])
' "$1" "$2" "$3"
}

# register_field <json-file> <field> -- a top-level register field.
register_field() {
    python3 -c '
import json, sys
data = json.load(open(sys.argv[1]))
print(data[sys.argv[2]])
' "$1" "$2"
}

# balances <json-file> -- every amount and running balance, one per line.
balances() {
    python3 -c '
import json, sys
data = json.load(open(sys.argv[1]))
for row in data["splits"]:
    print(row["amount"], "|", row["running_balance"])
' "$1"
}

write_body() { # write_body <file> <json>
    printf '%s' "$2" > "$1"
}

echo "== pick an account and a split from the demo book"
start "$TMP/ro.log"
RO_URL=$URL
request GET "api/accounts"
cp "$TMP/body" "$TMP/accounts.json"
GUID=$(python3 -c '
import json, sys
root = json.load(open(sys.argv[1]))
stack = list(root["children"])
while stack:
    node = stack.pop()
    if not node["children"]:
        print(node["guid"])
        break
    stack.extend(node["children"])
' "$TMP/accounts.json")
[ -n "$GUID" ] || fail "the demo book has no leaf account"

request GET "api/register/$GUID"
cp "$TMP/body" "$TMP/reg-before.json"
SPLIT=$(python3 -c '
import json, sys
data = json.load(open(sys.argv[1]))
print(data["splits"][0]["guid"])
' "$TMP/reg-before.json")
[ -n "$SPLIT" ] || fail "the account has no rows to edit"
balances "$TMP/reg-before.json" > "$TMP/balances-before.txt"
echo "account $GUID, split $SPLIT, $(wc -l < "$TMP/balances-before.txt") rows"

echo "== read-only (the default) refuses every editable field by name"
[ "$(register_field "$TMP/reg-before.json" editable)" = "False" ] \
    || fail "the read-only register says it is editable"
REASON=$(register_field "$TMP/reg-before.json" edit_reason)
echo "$REASON" | grep -q 'read-only' \
    || fail "the register does not say the session is read-only: $REASON"
echo "$REASON" | grep -q -- '--writable' \
    || fail "the register does not say how to enable editing: $REASON"
echo "read-only register: editable:false, reason names --writable"

for field in memo description num; do
    write_body "$TMP/edit.json" "{\"$field\":\"read-only probe\"}"
    request POST "api/split/$SPLIT" "$TMP/edit.json"
    [ "$CODE" = "403" ] \
        || fail "read-only edit of $field answered $CODE (expected 403): $BODY"
    echo "$BODY" | grep -q '"error":"read-only"' \
        || fail "the read-only refusal of $field is not named 'read-only': $BODY"
    echo "$BODY" | grep -q -- '--writable' \
        || fail "the read-only refusal of $field does not say how to enable it: $BODY"
done
echo "read-only: 403 named for memo, description and num"

write_body "$TMP/edit.json" '{"amount":"1.00"}'
request POST "api/split/$SPLIT" "$TMP/edit.json"
[ "$CODE" = "403" ] || fail "read-only edit of amount answered $CODE (expected 403)"

request GET "api/split/$SPLIT"
[ "$CODE" = "405" ] || fail "GET api/split answered $CODE (expected 405)"
echo "$BODY" | grep -q '"error":"method"' \
    || fail "the method refusal is not named: $BODY"

PORT=$(echo "$RO_URL" | sed -E 's|.*:([0-9]+)/.*|\1|')
write_body "$TMP/edit.json" '{"memo":"tokenless"}'
CODE=$(curl -sS -o "$TMP/tokenless" -w '%{http_code}' -X POST \
    --data-binary "@$TMP/edit.json" "http://127.0.0.1:$PORT/api/split/$SPLIT")
[ "$CODE" = "404" ] || fail "a tokenless edit answered $CODE (expected 404)"

stop
cmp -s "$BOOK" "$TMP/pre-edit.gnucash" \
    || fail "the read-only server changed the book on disk"
[ ! -e "$BOOK.LCK" ] || fail "the read-only server created $BOOK.LCK"
echo "read-only: 405, 404, book untouched, no lock"

echo "== writable: the register says it is editable"
start "$TMP/rw.log" --writable
grep -q 'webd mode: writable' "$TMP/rw.log" \
    || fail "the writable server did not say so: $(cat "$TMP/rw.log")"
request GET "api/register/$GUID"
cp "$TMP/body" "$TMP/reg-editable.json"
[ "$(register_field "$TMP/reg-editable.json" editable)" = "True" ] \
    || fail "the writable register does not say it is editable"
[ "$(register_field "$TMP/reg-editable.json" edit_reason)" = "" ] \
    || fail "the writable register still carries a read-only reason"
echo "writable register: editable:true"

echo "== bad values are named 400s and change nothing"
bad_case() { # bad_case <name> <slug> <json> [field]
    local name=$1 slug=$2 json=$3 field=${4:-}
    write_body "$TMP/edit.json" "$json"
    request POST "api/split/$SPLIT" "$TMP/edit.json"
    [ "$CODE" = "400" ] || fail "$name answered $CODE (expected 400): $BODY"
    echo "$BODY" | grep -q "\"error\":\"$slug\"" \
        || fail "$name is not named '$slug': $BODY"
    if [ -n "$field" ]; then
        echo "$BODY" | grep -q "\"field\":\"$field\"" \
            || fail "$name does not name the field '$field': $BODY"
    fi
    echo "$BODY" | grep -q '"message"' || fail "$name has no message: $BODY"
}
bad_case "a numeric memo" bad-type '{"memo":5}' memo
bad_case "a boolean description" bad-type '{"description":true}' description
bad_case "a null num" bad-type '{"num":null}' num
bad_case "a control character" bad-value '{"memo":"a\u0001b"}' memo
bad_case "an unknown field" unknown-field '{"date":"2020-01-01"}' date
bad_case "the amount" unsupported-field '{"amount":"1.00"}' amount
bad_case "an empty object" no-fields '{}'
bad_case "a bare string" bad-body '"memo"'
bad_case "broken JSON" bad-json '{"memo":'
# One good field and one bad field: the body is checked before anything is
# written, so the good field must not stick either.
write_body "$TMP/edit.json" "{\"memo\":\"must not stick\",\"amount\":\"1.00\"}"
request POST "api/split/$SPLIT" "$TMP/edit.json"
[ "$CODE" = "400" ] || fail "a mixed good/bad body answered $CODE (expected 400)"
request GET "api/register/$GUID"
[ "$(split_field "$TMP/body" "$SPLIT" memo)" != "must not stick" ] \
    || fail "a refused edit applied its good field anyway"

# 2 KiB is the per-field limit; a body over 1 MiB is refused before it is read.
python3 -c 'print("{\"memo\":\"" + "a" * 2049 + "\"}")' > "$TMP/edit.json"
request POST "api/split/$SPLIT" "$TMP/edit.json"
[ "$CODE" = "400" ] || fail "an over-long memo answered $CODE (expected 400)"
echo "$BODY" | grep -q '"error":"too-long"' \
    || fail "an over-long memo is not named 'too-long': $BODY"

python3 -c 'print("{\"memo\":\"" + "a" * 1048577 + "\"}")' > "$TMP/edit.json"
request POST "api/split/$SPLIT" "$TMP/edit.json"
[ "$CODE" = "413" ] || fail "a body over 1 MiB answered $CODE (expected 413)"
echo "$BODY" | grep -q '"error":"body-too-large"' \
    || fail "the oversized body is not named 'body-too-large': $BODY"

# An unknown split is a named 400, like the register's unknown account.
write_body "$TMP/edit.json" '{"memo":"x"}'
request POST "api/split/0123456789abcdef0123456789abcdef" "$TMP/edit.json"
[ "$CODE" = "400" ] || fail "an unknown split answered $CODE (expected 400)"
echo "$BODY" | grep -q '"error":"unknown-split"' \
    || fail "an unknown split is not named 'unknown-split': $BODY"

# None of the refusals touched the book: the engine was never asked to edit.
request POST "api/book/save"
[ "$CODE" = "200" ] || fail "the save after the refusals answered $CODE: $BODY"
echo "$BODY" | grep -q '"changed":false' \
    || fail "a refused edit dirtied the book: $BODY"
echo "bad values: 400/413 named, book still clean"

echo "== a good edit answers with the updated row"
write_body "$TMP/edit.json" '{"memo":"first memo"}'
request POST "api/split/$SPLIT" "$TMP/edit.json"
[ "$CODE" = "200" ] || fail "a memo edit answered $CODE: $BODY"
echo "$BODY" | grep -q '"memo":"first memo"' \
    || fail "the edit response does not carry the new memo: $BODY"
echo "$BODY" | grep -q '"guid":"'"$SPLIT"'"' \
    || fail "the edit response is not the edited split's row: $BODY"
echo "edit response: the updated row, memo=\"first memo\""

# An empty string clears the field rather than being ignored or coerced.
write_body "$TMP/edit.json" '{"memo":""}'
request POST "api/split/$SPLIT" "$TMP/edit.json"
[ "$CODE" = "200" ] || fail "clearing the memo answered $CODE: $BODY"
echo "$BODY" | grep -q '"memo":""' \
    || fail "an empty memo was not stored as empty: $BODY"

write_body "$TMP/edit.json" '{"memo":"register edit memo"}'
request POST "api/split/$SPLIT" "$TMP/edit.json"
[ "$CODE" = "200" ] || fail "restoring the memo answered $CODE: $BODY"

write_body "$TMP/edit.json" \
    '{"description":"register edit description","num":"EDIT-1"}'
request POST "api/split/$SPLIT" "$TMP/edit.json"
[ "$CODE" = "200" ] || fail "a description/num edit answered $CODE: $BODY"

request GET "api/register/$GUID"
cp "$TMP/body" "$TMP/reg-after.json"
[ "$(split_field "$TMP/reg-after.json" "$SPLIT" memo)" = "register edit memo" ] \
    || fail "the register does not show the edited memo"
[ "$(split_field "$TMP/reg-after.json" "$SPLIT" description)" \
    = "register edit description" ] \
    || fail "the register does not show the edited description"
[ "$(split_field "$TMP/reg-after.json" "$SPLIT" num)" = "EDIT-1" ] \
    || fail "the register does not show the edited number"
balances "$TMP/reg-after.json" > "$TMP/balances-after.txt"
cmp -s "$TMP/balances-before.txt" "$TMP/balances-after.txt" \
    || fail "a text edit changed an amount or a running balance"
echo "register: the three text fields edited, every amount and balance unchanged"

echo "== a save writes the edits, with a backup of the pre-edit book"
request POST "api/book/save"
[ "$CODE" = "200" ] || fail "the save answered $CODE: $BODY"
echo "$BODY" | grep -q '"changed":true' \
    || fail "the save after an edit reported no change: $BODY"
BACKUP=$(echo "$BODY" | sed -n 's/.*"backup":"\([^"]*\)".*/\1/p')
[ -n "$BACKUP" ] || fail "the save named no backup: $BODY"
[ -f "$BACKUP" ] || fail "the named backup does not exist: $BACKUP"
cmp -s "$BACKUP" "$TMP/pre-edit.gnucash" \
    || fail "the backup is not byte-for-byte the pre-edit book"
cmp -s "$BOOK" "$TMP/pre-edit.gnucash" \
    && fail "the book on disk did not change"
BYTES=$(echo "$BODY" | sed -n 's/.*"bytes":\([0-9]*\).*/\1/p')
[ "$BYTES" = "$(stat -c%s "$BOOK")" ] \
    || fail "the reported size ($BYTES) is not the file's ($(stat -c%s "$BOOK"))"
echo "save: $(stat -c%s "$BOOK") bytes, backup $BACKUP"

echo "== an edit that changes nothing leaves the book clean"
write_body "$TMP/edit.json" '{"memo":"register edit memo"}'
request POST "api/split/$SPLIT" "$TMP/edit.json"
[ "$CODE" = "200" ] || fail "a same-value edit answered $CODE: $BODY"
request POST "api/book/save"
[ "$CODE" = "200" ] || fail "the save after a same-value edit answered $CODE: $BODY"
echo "$BODY" | grep -q '"changed":false' \
    || fail "a same-value edit dirtied the book: $BODY"
echo "same value: 200, and the book is still clean"

stop
[ ! -e "$BOOK.LCK" ] || fail "the writable server left $BOOK.LCK behind"

echo "== a fresh server re-opens the book and sees the edit"
start "$TMP/reopen.log"
request GET "api/register/$GUID"
[ "$CODE" = "200" ] || fail "the re-opened register answered $CODE: $BODY"
[ "$(split_field "$TMP/body" "$SPLIT" memo)" = "register edit memo" ] \
    || fail "the saved memo is not in the re-opened book"
[ "$(split_field "$TMP/body" "$SPLIT" description)" \
    = "register edit description" ] \
    || fail "the saved description is not in the re-opened book"
[ "$(split_field "$TMP/body" "$SPLIT" num)" = "EDIT-1" ] \
    || fail "the saved number is not in the re-opened book"
stop
echo "re-open: the edits are on disk"

echo "== two dirty saves back-to-back both succeed, each with its own backup"
# The user's case (ADR 0014 §10): save, edit, save again immediately.  If both
# saves fall inside one second the engine's per-second backup name collides
# and the server waits for the next second and retries once; if the clock has
# already turned there is no collision to retry.  Either way both saves must
# succeed, both backups must exist and be distinct, and the final book must
# carry both edits.
start "$TMP/collision.log" --writable
write_body "$TMP/edit.json" '{"memo":"back-to-back first"}'
request POST "api/split/$SPLIT" "$TMP/edit.json"
[ "$CODE" = "200" ] || fail "the first back-to-back edit answered $CODE: $BODY"
request POST "api/book/save"
[ "$CODE" = "200" ] || fail "the first back-to-back save answered $CODE: $BODY"
BACKUP_FIRST=$(echo "$BODY" | sed -n 's/.*"backup":"\([^"]*\)".*/\1/p')
[ -n "$BACKUP_FIRST" ] \
    || fail "the first back-to-back save named no backup: $BODY"
[ -f "$BACKUP_FIRST" ] \
    || fail "the first back-to-back backup is missing: $BACKUP_FIRST"

# No sleep here on purpose: this is the save that can collide.
write_body "$TMP/edit.json" '{"num":"BACK-TO-BACK-2"}'
request POST "api/split/$SPLIT" "$TMP/edit.json"
[ "$CODE" = "200" ] || fail "the second back-to-back edit answered $CODE: $BODY"
request POST "api/book/save"
[ "$CODE" = "200" ] || fail "the second back-to-back save answered $CODE: $BODY"
BACKUP_SECOND=$(echo "$BODY" | sed -n 's/.*"backup":"\([^"]*\)".*/\1/p')
[ -n "$BACKUP_SECOND" ] \
    || fail "the second back-to-back save named no backup: $BODY"
[ -f "$BACKUP_SECOND" ] \
    || fail "the second back-to-back backup is missing: $BACKUP_SECOND"
[ "$BACKUP_FIRST" != "$BACKUP_SECOND" ] \
    || fail "the two saves named the same backup file: $BACKUP_FIRST"
echo "back-to-back: 200/200, backups $(basename "$BACKUP_FIRST") and $(basename "$BACKUP_SECOND")"

echo "== a colliding same-second backup name is waited out and retried once"
# Force the exact precondition: plant a file at the backup name this second
# would use, then save inside that second.  The engine refuses the backup, the
# server sees this second's name is taken, waits for the next second and
# retries once.  The back-to-back case above only reaches this by timing; this
# one reaches it by construction.
RETRY_MARK='retrying the save once'
RETRIES_BEFORE=$(grep -cF "$RETRY_MARK" "$TMP/collision.log" || true)
COLLISION_OK=0
PLANTED=""
for attempt in 1 2 3; do
    # Start just after a second boundary so the plant, the edit and the save
    # all fit inside one second.
    python3 -c 'import time; time.sleep(1.0 - (time.time() % 1.0) + 0.01)'
    STAMP=$(date +%Y%m%d%H%M%S)
    PLANTED="$BOOK.$STAMP.gnucash"
    if [ -e "$PLANTED" ]; then
        # A real backup already uses this second's name; try the next second.
        PLANTED=""
        continue
    fi
    : > "$PLANTED"
    write_body "$TMP/edit.json" \
        "{\"description\":\"retried collision $attempt\"}"
    request POST "api/split/$SPLIT" "$TMP/edit.json"
    [ "$CODE" = "200" ] \
        || { rm -f "$PLANTED"; fail "the edit before the colliding save answered $CODE: $BODY"; }
    request POST "api/book/save"
    if [ "$CODE" != "200" ]; then
        rm -f "$PLANTED"
        fail "a save whose backup name was taken answered $CODE instead of retrying: $BODY"
    fi
    RETRIES_AFTER=$(grep -cF "$RETRY_MARK" "$TMP/collision.log" || true)
    if [ "$RETRIES_AFTER" -gt "$RETRIES_BEFORE" ]; then
        COLLISION_OK=1
        break
    fi
    # The clock turned between the plant and the save, so there was nothing to
    # collide with.  Remove the plant and try again.
    rm -f "$PLANTED"
    PLANTED=""
done
[ "$COLLISION_OK" = "1" ] \
    || fail "no save collided on the planted backup name in three attempts"
[ -f "$PLANTED" ] \
    || fail "the retry removed the file that caused the collision: $PLANTED"
[ ! -s "$PLANTED" ] \
    || fail "the retry overwrote the file that caused the collision: $PLANTED"
RETRY_BACKUP=$(echo "$BODY" | sed -n 's/.*"backup":"\([^"]*\)".*/\1/p')
[ -n "$RETRY_BACKUP" ] || fail "the retried save named no backup: $BODY"
[ "$RETRY_BACKUP" != "$PLANTED" ] \
    || fail "the retried save reused the colliding name: $BODY"
[ -f "$RETRY_BACKUP" ] \
    || fail "the retried save's backup is missing: $RETRY_BACKUP"
echo "$BODY" | grep -q '"retried":true' \
    || fail "the retried save does not report that it retried: $BODY"
echo "collision: waited for the next second, retried once, backup $(basename "$RETRY_BACKUP")"

echo "== the saved book carries both back-to-back changes"
stop
[ ! -e "$BOOK.LCK" ] || fail "the collision server left $BOOK.LCK behind"
start "$TMP/collision-reopen.log"
request GET "api/register/$GUID"
[ "$CODE" = "200" ] \
    || fail "the reopened register answered $CODE: $BODY"
[ "$(split_field "$TMP/body" "$SPLIT" memo)" = "back-to-back first" ] \
    || fail "the first back-to-back change is not in the saved book"
[ "$(split_field "$TMP/body" "$SPLIT" num)" = "BACK-TO-BACK-2" ] \
    || fail "the second back-to-back change is not in the saved book"
echo "$BODY" | grep -q 'retried collision' \
    || fail "the retried save's change is not in the saved book"
echo "reopen: both back-to-back changes and the retried edit are on disk"
stop

echo "== a genuinely failing backup still fails with the named 500, and is not retried"
start "$TMP/failing-backup.log" --writable
write_body "$TMP/edit.json" '{"memo":"backup must fail"}'
request POST "api/split/$SPLIT" "$TMP/edit.json"
[ "$CODE" = "200" ] \
    || fail "the edit before the failing save answered $CODE: $BODY"
cp "$BOOK" "$TMP/before-failing-backup.gnucash"
RETRIES_BEFORE=$(grep -cF "$RETRY_MARK" "$TMP/failing-backup.log" || true)
# The engine makes the backup in the book's directory; taking write permission
# away there makes backup_file() fail without any name having been taken.
chmod u-w "$TMP"
request POST "api/book/save"
chmod u+w "$TMP"
[ "$CODE" = "500" ] \
    || fail "a save with no writable directory answered $CODE (expected 500): $BODY"
echo "$BODY" | grep -q '"error":"backup-failed"' \
    || fail "the failed-backup refusal is not named 'backup-failed': $BODY"
echo "$BODY" | grep -q '"path_exists":true' \
    || fail "the failed save does not say the book is still there: $BODY"
FAILED_BACKUP=$(echo "$BODY" | sed -n 's/.*"backup":"\([^"]*\)".*/\1/p')
[ -n "$FAILED_BACKUP" ] \
    || fail "the failed-backup body names no backup path: $BODY"
echo "$FAILED_BACKUP" | grep -qE '\.[0-9]{14}\.gnucash$' \
    || fail "the failed-backup body does not name a timestamped backup: $FAILED_BACKUP"
echo "$BODY" | grep -qF "$FAILED_BACKUP" \
    || fail "the failed-backup message does not name the backup path: $BODY"
RETRIES_AFTER=$(grep -cF "$RETRY_MARK" "$TMP/failing-backup.log" || true)
[ "$RETRIES_AFTER" = "$RETRIES_BEFORE" ] \
    || fail "a genuinely failing backup was retried"
cmp -s "$BOOK" "$TMP/before-failing-backup.gnucash" \
    || fail "the book changed even though the backup failed"
echo "failing backup: 500 named, backup path named, no retry, book untouched"
stop

echo "== a locked book refuses the edit and names the lock"
start "$TMP/lockA.log" --writable
A_PID=$SERVER_PID
[ -e "$BOOK.LCK" ] || fail "the first writable server did not take the lock"
LOCKED_COPY=$TMP/locked-copy.gnucash
cp "$BOOK" "$LOCKED_COPY"

start "$TMP/lockB.log" --writable
B_PID=$SERVER_PID
grep -q 'webd mode: read-only (locked:' "$TMP/lockB.log" \
    || fail "the second writable server did not report the lock: $(cat "$TMP/lockB.log")"

write_body "$TMP/edit.json" '{"memo":"locked edit"}'
request POST "api/split/$SPLIT" "$TMP/edit.json"
[ "$CODE" = "409" ] || fail "the locked book answered $CODE (expected 409): $BODY"
echo "$BODY" | grep -q '"error":"locked"' \
    || fail "the locked refusal is not named 'locked': $BODY"
echo "$BODY" | grep -qF "$BOOK.LCK" \
    || fail "the locked refusal does not name the lock file: $BODY"

stop "$B_PID"
stop "$A_PID"
[ ! -e "$BOOK.LCK" ] || fail "the writable server left $BOOK.LCK after a clean stop"
cmp -s "$BOOK" "$LOCKED_COPY" || fail "a refused edit modified the book anyway"
echo "locked: 409 named, the .LCK path, book unchanged"

echo
echo "register edit test passed"
