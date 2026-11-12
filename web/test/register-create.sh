#!/bin/bash
# Creating a transaction (ADR 0015): POST /api/transaction and
# POST /api/transaction/validate, plus GET /api/accounts/options.
#
# Usage: register-create.sh <build-dir> <source-dir>
#
# Generates the 190-row demo book with web/test/make-demo-book.py and checks:
#
#   * read-only (the default): a create is refused with a named 403 naming
#     --writable, the method check is 405, a tokenless request is 404, and the
#     book is byte-for-byte untouched (no lock either);
#   * writable: a balanced two-line transaction is created; the response is the
#     transaction shape GET /api/transaction returns, plus the register rows it
#     produced and book_dirty:true; the transaction is in the engine but the
#     book is still dirty in memory;
#   * the register shows the new row, the engine's own /api/transaction shows
#     both splits, and Save writes the book with a backup of the pre-create
#     file;
#   * a fresh server re-opens the saved book and the transaction is there with
#     the right splits, accounts, amounts and memos;
#   * named 400s create nothing: unbalanced, unknown account, missing account,
#     zero lines, one line, bad date, bad amount (non-numeric and too precise),
#     too-long text, too many lines, unknown/unknown-split fields, bad type,
#     bad JSON; a body over 1 MiB is a named 413; a save after the battery
#     answers changed:false, so none of them touched the engine;
#   * a locked book refuses creation with 409 naming the .LCK and changes
#     nothing.
#
# Exit status: 0 when every check passes, 1 otherwise.
set -u

BUILD=${1:?usage: register-create.sh <build-dir> <source-dir>}
SRC=${2:?usage: register-create.sh <build-dir> <source-dir>}

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
cp "$BOOK" "$TMP/pre-create.gnucash"

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
        URL=$(grep -oE 'http://127\.0\.0\.1:[0-9]+/[0-9a-f]+/' "$log" 2>/dev/null | head -1)
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

# json_field <json-file> <python-expression on d> -- prints the expression.
json_value() {
    python3 -c '
import json, sys
d = json.load(open(sys.argv[1]))
value = eval(sys.argv[2], {"d": d})
print(value)
' "$1" "$2"
}

# register_field <json-file> <field> -- a top-level register field.
register_field() {
    json_value "$1" "d['$2']"
}

write_body() { # write_body <file> <json>
    printf '%s' "$2" > "$1"
}

echo "== read-only (the default) refuses creation by name"
start "$TMP/ro.log"
RO_URL=$URL
request GET "api/accounts/options"
[ "$CODE" = "200" ] || fail "GET api/accounts/options answered $CODE: $BODY"
cp "$TMP/body" "$TMP/options.json"
GUID_A=$(json_value "$TMP/options.json" "d['accounts'][0]['guid']")
GUID_B=$(python3 -c '
import json, sys
accounts = json.load(open(sys.argv[1]))["accounts"]
first = accounts[0]
other = next(a for a in accounts
             if a["commodity"] == first["commodity"] and a["guid"] != first["guid"])
print(other["guid"])
' "$TMP/options.json")
[ -n "$GUID_A" ] && [ -n "$GUID_B" ] || fail "the demo book has no two usable accounts"
TOTAL=$(json_value "$TMP/options.json" "d['accounts'][0]['total']")
echo "$TOTAL" | grep -q '[0-9]' \
    || fail "an account option carries no subtree total: $TOTAL"
echo "options: $(json_value "$TMP/options.json" "len(d['accounts'])") accounts, " \
     "first total $TOTAL"

create_body() { # create_body <file> <date> <num> <description> <amount-a> <amount-b>
    python3 -c '
import json, sys
out, date, num, description, a, b = sys.argv[1:7]
body = {
    "date": date,
    "number": num,
    "description": description,
    "splits": [
        {"account": sys.argv[7], "amount": a, "memo": "first memo"},
        {"account": sys.argv[8], "amount": b, "memo": "second memo"},
    ],
}
open(out, "w").write(json.dumps(body))
' "$1" "$2" "$3" "$4" "$5" "$6" "$GUID_A" "$GUID_B"
}

create_body "$TMP/create.json" "2026-01-15" "CREATE-1" \
    "created by register-create.sh" "12.34" "-12.34"

request POST "api/transaction" "$TMP/create.json"
[ "$CODE" = "403" ] || fail "a read-only create answered $CODE (expected 403): $BODY"
echo "$BODY" | grep -q '"error":"read-only"' \
    || fail "the read-only create refusal is not named 'read-only': $BODY"
echo "$BODY" | grep -q -- '--writable' \
    || fail "the read-only create refusal does not name --writable: $BODY"
echo "read-only: 403 named, names --writable"

request POST "api/transaction/validate" "$TMP/create.json"
[ "$CODE" = "403" ] || fail "a read-only validate answered $CODE (expected 403): $BODY"
echo "$BODY" | grep -q '"error":"read-only"' \
    || fail "the read-only validate refusal is not named: $BODY"

request GET "api/transaction"
[ "$CODE" = "405" ] || fail "GET api/transaction answered $CODE (expected 405)"
echo "$BODY" | grep -q '"error":"method"' \
    || fail "the create method refusal is not named: $BODY"

request GET "api/transaction/validate"
[ "$CODE" = "405" ] || fail "GET api/transaction/validate answered $CODE (expected 405)"

PORT=$(echo "$RO_URL" | sed -E 's|.*:([0-9]+)/.*|\1|')
CODE=$(curl -sS -o "$TMP/tokenless" -w '%{http_code}' -X POST \
    --data-binary "@$TMP/create.json" "http://127.0.0.1:$PORT/api/transaction")
[ "$CODE" = "404" ] || fail "a tokenless create answered $CODE (expected 404)"

stop
cmp -s "$BOOK" "$TMP/pre-create.gnucash" \
    || fail "the read-only server changed the book on disk"
[ ! -e "$BOOK.LCK" ] || fail "the read-only server created $BOOK.LCK"
echo "read-only: 405, 404, book untouched, no lock"

echo "== writable: the validate route reports the balance before anything is written"
start "$TMP/rw.log" --writable
request GET "api/register/$GUID_A"
[ "$CODE" = "200" ] || fail "the register answered $CODE: $BODY"
cp "$TMP/body" "$TMP/reg-before.json"
ROWS_BEFORE=$(register_field "$TMP/reg-before.json" row_count)

request POST "api/transaction/validate" "$TMP/create.json"
[ "$CODE" = "200" ] || fail "validate answered $CODE: $BODY"
echo "$BODY" | grep -q '"balanced":true' \
    || fail "a balanced body is not reported balanced: $BODY"
echo "$BODY" | grep -q '"split_count":2' \
    || fail "the verdict does not count the two lines: $BODY"

# Unbalanced is a well-formed body: the verdict is 200 with balanced:false.
create_body "$TMP/unbalanced.json" "2026-01-15" "" "" "12.34" "-5.00"
request POST "api/transaction/validate" "$TMP/unbalanced.json"
[ "$CODE" = "200" ] || fail "validate of an unbalanced body answered $CODE: $BODY"
echo "$BODY" | grep -q '"balanced":false' \
    || fail "an unbalanced body is not reported unbalanced: $BODY"
echo "$BODY" | grep -q '"imbalance"' \
    || fail "the unbalanced verdict carries no imbalance: $BODY"
echo "validate: balanced true/false with a running sum, nothing written"

echo "== bad bodies are named 400s and create nothing"
bad_case() { # bad_case <name> <slug> <json> [field]
    local name=$1 slug=$2 json=$3 field=${4:-}
    write_body "$TMP/bad.json" "$json"
    request POST "api/transaction" "$TMP/bad.json"
    [ "$CODE" = "400" ] || fail "$name answered $CODE (expected 400): $BODY"
    echo "$BODY" | grep -q "\"error\":\"$slug\"" \
        || fail "$name is not named '$slug': $BODY"
    if [ -n "$field" ]; then
        echo "$BODY" | grep -qF "\"field\":\"$field\"" \
            || fail "$name does not name the field '$field': $BODY"
    fi
    echo "$BODY" | grep -q '"message"' || fail "$name has no message: $BODY"
}

# An unbalanced body: the named refusal carries the imbalance and creates
# nothing.
write_body "$TMP/bad.json" \
    "{\"date\":\"2026-01-15\",\"splits\":[
       {\"account\":\"$GUID_A\",\"amount\":\"12.34\"},
       {\"account\":\"$GUID_B\",\"amount\":\"-5.00\"}]}"
request POST "api/transaction" "$TMP/bad.json"
[ "$CODE" = "400" ] || fail "an unbalanced create answered $CODE: $BODY"
echo "$BODY" | grep -q '"error":"unbalanced"' \
    || fail "the unbalanced refusal is not named 'unbalanced': $BODY"
echo "$BODY" | grep -q '"imbalance"' \
    || fail "the unbalanced refusal names no imbalance: $BODY"
echo "$BODY" | grep -q 'do not balance' \
    || fail "the unbalanced refusal does not explain itself: $BODY"

write_body "$TMP/bad.json" \
    "{\"date\":\"2026-01-15\",\"splits\":[
       {\"account\":\"0123456789abcdef0123456789abcdef\",\"amount\":\"1.00\"},
       {\"account\":\"$GUID_B\",\"amount\":\"-1.00\"}]}"
request POST "api/transaction" "$TMP/bad.json"
[ "$CODE" = "400" ] || fail "an unknown account answered $CODE: $BODY"
echo "$BODY" | grep -q '"error":"unknown-account"' \
    || fail "an unknown account is not named 'unknown-account': $BODY"

write_body "$TMP/bad.json" \
    "{\"date\":\"2026-01-15\",\"splits\":[
       {\"amount\":\"1.00\"},
       {\"account\":\"$GUID_B\",\"amount\":\"-1.00\"}]}"
request POST "api/transaction" "$TMP/bad.json"
[ "$CODE" = "400" ] || fail "a missing account answered $CODE: $BODY"
echo "$BODY" | grep -q '"error":"missing-field"' \
    || fail "a missing account is not named 'missing-field': $BODY"
echo "$BODY" | grep -qF '"field":"splits[0].account"' \
    || fail "a missing account does not name the field: $BODY"

bad_case "an empty splits array" too-few-splits \
    "{\"date\":\"2026-01-15\",\"splits\":[]}"
bad_case "one split" too-few-splits \
    "{\"date\":\"2026-01-15\",\"splits\":[{\"account\":\"$GUID_A\",\"amount\":\"1.00\"}]}"
bad_case "a date that is not a day" bad-date \
    "{\"date\":\"2026-02-30\",\"splits\":[{\"account\":\"$GUID_A\",\"amount\":\"1.00\"},{\"account\":\"$GUID_B\",\"amount\":\"-1.00\"}]}" \
    date
bad_case "a malformed date" bad-date \
    "{\"date\":\"15/01/2026\",\"splits\":[{\"account\":\"$GUID_A\",\"amount\":\"1.00\"},{\"account\":\"$GUID_B\",\"amount\":\"-1.00\"}]}" \
    date
bad_case "a non-numeric amount" bad-amount \
    "{\"date\":\"2026-01-15\",\"splits\":[{\"account\":\"$GUID_A\",\"amount\":\"abc\"},{\"account\":\"$GUID_B\",\"amount\":\"-1.00\"}]}" \
    "splits[0].amount"
bad_case "more decimal places than the commodity allows" bad-amount \
    "{\"date\":\"2026-01-15\",\"splits\":[{\"account\":\"$GUID_A\",\"amount\":\"1.234\"},{\"account\":\"$GUID_B\",\"amount\":\"-1.23\"}]}" \
    "splits[0].amount"
bad_case "an amount that is not a string" bad-type \
    "{\"date\":\"2026-01-15\",\"splits\":[{\"account\":\"$GUID_A\",\"amount\":5},{\"account\":\"$GUID_B\",\"amount\":\"-5\"}]}" \
    "splits[0].amount"
bad_case "an unknown top-level field" unknown-field \
    "{\"date\":\"2026-01-15\",\"payee\":\"x\",\"splits\":[{\"account\":\"$GUID_A\",\"amount\":\"1.00\"},{\"account\":\"$GUID_B\",\"amount\":\"-1.00\"}]}" \
    payee
bad_case "an unknown split field" unknown-split-field \
    "{\"date\":\"2026-01-15\",\"splits\":[{\"account\":\"$GUID_A\",\"amount\":\"1.00\",\"price\":\"2\"},{\"account\":\"$GUID_B\",\"amount\":\"-1.00\"}]}" \
    "splits[0].price"
bad_case "broken JSON" bad-json '{"date":'

python3 -c '
import json, sys
body = {"date": "2026-01-15", "description": "a" * 2049,
        "splits": [{"account": sys.argv[1], "amount": "1.00"},
                   {"account": sys.argv[2], "amount": "-1.00"}]}
open(sys.argv[3], "w").write(json.dumps(body))
' "$GUID_A" "$GUID_B" "$TMP/long.json"
request POST "api/transaction" "$TMP/long.json"
[ "$CODE" = "400" ] || fail "an over-long description answered $CODE: $BODY"
echo "$BODY" | grep -q '"error":"too-long"' \
    || fail "an over-long description is not named 'too-long': $BODY"
echo "$BODY" | grep -q '"field":"description"' \
    || fail "an over-long description does not name the field: $BODY"

python3 -c '
import json, sys
body = {"date": "2026-01-15", "splits": [{} for _ in range(51)]}
open(sys.argv[1], "w").write(json.dumps(body))
' "$TMP/many.json"
request POST "api/transaction" "$TMP/many.json"
[ "$CODE" = "400" ] || fail "51 splits answered $CODE: $BODY"
echo "$BODY" | grep -q '"error":"too-many-splits"' \
    || fail "51 splits are not named 'too-many-splits': $BODY"

python3 -c 'print("{\"date\":\"2026-01-15\",\"description\":\"" + "a" * 1048577 + "\",\"splits\":[]}")' \
    > "$TMP/huge.json"
request POST "api/transaction" "$TMP/huge.json"
[ "$CODE" = "413" ] || fail "a body over 1 MiB answered $CODE (expected 413)"
echo "$BODY" | grep -q '"error":"body-too-large"' \
    || fail "the oversized body is not named 'body-too-large': $BODY"

# None of the refusals touched the engine: the engine is still clean.
request POST "api/book/save"
[ "$CODE" = "200" ] || fail "the save after the refusals answered $CODE: $BODY"
echo "$BODY" | grep -q '"changed":false' \
    || fail "a refused create dirtied the book: $BODY"
echo "bad bodies: 400/413 named, book still clean"

echo "== a balanced transaction is created, but not saved"
request POST "api/transaction" "$TMP/create.json"
[ "$CODE" = "200" ] || fail "a good create answered $CODE: $BODY"
cp "$TMP/body" "$TMP/created.json"
CREATED=$(json_value "$TMP/created.json" "d['guid']")
[ -n "$CREATED" ] || fail "the create response names no transaction: $BODY"
[ "$(json_value "$TMP/created.json" "d['balanced']")" = "True" ] \
    || fail "the created transaction is not balanced: $BODY"
[ "$(json_value "$TMP/created.json" "d['book_dirty']")" = "True" ] \
    || fail "the create response does not say the book is dirty: $BODY"
[ "$(json_value "$TMP/created.json" "len(d['splits'])")" = "2" ] \
    || fail "the created transaction does not have two splits: $BODY"
[ "$(json_value "$TMP/created.json" "len(d['rows'])")" = "2" ] \
    || fail "the create response does not carry the register rows: $BODY"
[ "$(json_value "$TMP/created.json" "d['num']")" = "CREATE-1" ] \
    || fail "the created transaction lost its number: $BODY"
[ "$(json_value "$TMP/created.json" "d['description']")" \
    = "created by register-create.sh" ] \
    || fail "the created transaction lost its description: $BODY"
echo "$BODY" | grep -q '"date_iso":"2026-01-15' \
    || fail "the created transaction lost its date: $BODY"
echo "$BODY" | grep -q '"memo":"first memo"' \
    || fail "the first split lost its memo: $BODY"
echo "created $CREATED: two splits, balanced, book_dirty:true"

request GET "api/transaction/$CREATED"
[ "$CODE" = "200" ] || fail "GET the created transaction answered $CODE: $BODY"
[ "$(json_value "$TMP/body" "d['guid']")" = "$CREATED" ] \
    || fail "the read route returned a different transaction"
[ "$(json_value "$TMP/body" "len(d['splits'])")" = "2" ] \
    || fail "the read route does not show both splits"

request GET "api/register/$GUID_A"
[ "$CODE" = "200" ] || fail "the register after create answered $CODE: $BODY"
[ "$(register_field "$TMP/body" row_count)" = "$((ROWS_BEFORE + 1))" ] \
    || fail "the register did not gain exactly one row"
python3 -c '
import json, sys
data = json.load(open(sys.argv[1]))
row = next(r for r in data["splits"] if r["transaction_guid"] == sys.argv[2])
assert row["num"] == "CREATE-1", row
assert row["description"] == "created by register-create.sh", row
assert row["memo"] == "first memo", row
assert "12.34" in row["amount"], row
print("row", row["date"], row["amount"], row["memo"])
' "$TMP/body" "$CREATED"
echo "register: the new row is there with its number, description and memo"

echo "== Save writes the created transaction, with a pre-create backup"
cp "$BOOK" "$TMP/before-save.gnucash"
request POST "api/book/save"
[ "$CODE" = "200" ] || fail "the save answered $CODE: $BODY"
echo "$BODY" | grep -q '"changed":true' \
    || fail "the save after a create reported no change: $BODY"
BACKUP=$(echo "$BODY" | sed -n 's/.*"backup":"\([^"]*\)".*/\1/p')
[ -n "$BACKUP" ] || fail "the save named no backup: $BODY"
[ -f "$BACKUP" ] || fail "the named backup does not exist: $BACKUP"
cmp -s "$BACKUP" "$TMP/pre-create.gnucash" \
    || fail "the backup is not the byte-for-byte pre-create book"
cmp -s "$BOOK" "$TMP/pre-create.gnucash" \
    && fail "the book on disk did not change after the save"
echo "save: $(stat -c%s "$BOOK") bytes, backup $BACKUP"

stop
[ ! -e "$BOOK.LCK" ] || fail "the writable server left $BOOK.LCK behind"

echo "== a fresh server re-opens the book and the transaction is there"
start "$TMP/reopen.log"
request GET "api/transaction/$CREATED"
[ "$CODE" = "200" ] || fail "the re-opened transaction answered $CODE: $BODY"
[ "$(json_value "$TMP/body" "len(d['splits'])")" = "2" ] \
    || fail "the re-opened transaction does not have two splits"
[ "$(json_value "$TMP/body" "d['num']")" = "CREATE-1" ] \
    || fail "the re-opened transaction lost its number"
python3 -c '
import json, re, sys
data = json.load(open(sys.argv[1]))
def value(text):
    text = text.replace("\u2066", "").replace("\u2069", "").strip()
    negative = text.startswith("-") or (text.startswith("(") and text.endswith(")"))
    match = re.search(r"[0-9][0-9,]*\.[0-9]+", text)
    number = match.group(0).replace(",", "") if match else text
    return ("-" if negative else "") + number
amounts = sorted(value(s["amount"]) for s in data["splits"])
memos = sorted(s["memo"] for s in data["splits"])
assert amounts == ["-12.34", "12.34"], amounts
assert memos == ["first memo", "second memo"], memos
print("splits", len(data["splits"]), "amounts", amounts, "memos", memos)
' "$TMP/body"
request GET "api/register/$GUID_A"
[ "$CODE" = "200" ] || fail "the re-opened register answered $CODE: $BODY"
echo "$BODY" | grep -q '"transaction_guid":"'"$CREATED"'"' \
    || fail "the re-opened register does not show the created row"
stop
echo "re-open: the created transaction and its splits are on disk"

echo "== a locked book refuses creation and names the lock"
start "$TMP/lockA.log" --writable
A_PID=$SERVER_PID
[ -e "$BOOK.LCK" ] || fail "the first writable server did not take the lock"
cp "$BOOK" "$TMP/locked-copy.gnucash"

start "$TMP/lockB.log" --writable
B_PID=$SERVER_PID
grep -q 'webd mode: read-only (locked:' "$TMP/lockB.log" \
    || fail "the second writable server did not report the lock: $(cat "$TMP/lockB.log")"

create_body "$TMP/locked.json" "2026-01-16" "LOCKED-1" \
    "must not be created" "1.00" "-1.00"
request POST "api/transaction" "$TMP/locked.json"
[ "$CODE" = "409" ] || fail "the locked book answered $CODE (expected 409): $BODY"
echo "$BODY" | grep -q '"error":"locked"' \
    || fail "the locked refusal is not named 'locked': $BODY"
echo "$BODY" | grep -qF "$BOOK.LCK" \
    || fail "the locked refusal does not name the lock file: $BODY"

stop "$B_PID"
stop "$A_PID"
[ ! -e "$BOOK.LCK" ] || fail "a writable server left $BOOK.LCK after a clean stop"
cmp -s "$BOOK" "$TMP/locked-copy.gnucash" \
    || fail "a refused create modified the book anyway"
echo "locked: 409 named, the .LCK path, book unchanged"

echo
echo "register create test passed"
