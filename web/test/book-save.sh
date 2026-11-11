#!/bin/bash
# The write path (ADR 0013), in both the read-only default and --writable.
#
# Usage: book-save.sh <build-dir> <source-dir>
#
# Starts gnc-webd against a *copy* of the fixture book and checks:
#   * read-only: POST /api/book/save and /api/book/selftest are 403 with a
#     named error, GET is 405, no .LCK is created, and the book is untouched;
#   * writable: the .LCK exists while the server runs; a clean save says
#     changed:false; one self-test transaction then a save writes the book,
#     names a backup, and the backup is byte-for-byte the pre-save book;
#   * the saved book re-opens in a fresh server with the transaction in it;
#   * a second writable server on the locked book is refused with 409 naming
#     the .LCK, while a read-only server still opens the same book;
#   * a clean stop removes the .LCK;
#   * --writable refuses a non-local book URI.
#
# Exit status: 0 when every check passes, 1 otherwise.
set -u

BUILD=${1:?usage: book-save.sh <build-dir> <source-dir>}
SRC=${2:?usage: book-save.sh <build-dir> <source-dir>}

FIXTURE="$SRC/libgnucash/backend/xml/test/test-files/load-save/sample1.gnucash"
BACKEND="$BUILD/lib/gnucash/libgncmod-backend-xml.so"
SCHEMAS="$BUILD/share/glib-2.0/schemas"
CCACHE=$(ls -d "$BUILD"/lib/*/guile/3.0/site-ccache 2>/dev/null | head -1)

fail() { echo "FAIL: $*" >&2; exit 1; }

[ -x "$BUILD/bin/gnc-webd" ] || fail "gnc-webd was not built"
[ -f "$FIXTURE" ] || fail "fixture book not found at $FIXTURE"
[ -f "$SCHEMAS/gschemas.compiled" ] || SCHEMAS=""

TMP=$(mktemp -d)
PIDS=()
cleanup() {
    for pid in "${PIDS[@]}"; do kill "$pid" 2>/dev/null; done
    rm -rf "$TMP"
}
trap cleanup EXIT

BOOK="$TMP/book.gnucash"
cp "$FIXTURE" "$BOOK"
cp "$FIXTURE" "$TMP/pre.gnucash"

webd_env() {
    env GNC_UNINSTALLED=YES GNC_BUILDDIR="$BUILD" \
        ${SCHEMAS:+GSETTINGS_SCHEMA_DIR="$SCHEMAS"} \
        LD_LIBRARY_PATH="$BUILD/lib:$BUILD/lib/gnucash" \
        GNC_MODULE_PATH="$BUILD/lib/gnucash" \
        GUILE_LOAD_PATH="$BUILD/share/guile/site/3.0" \
        GUILE_LOAD_COMPILED_PATH="${CCACHE:-}" \
        GNC_WEBUI_DIR="$SRC/web/ui" \
        GNC_WEBUI_REPORTS="$TMP/reports.json" \
        "$@"
}

# start <log> [extra gnc-webd args...] -- sets URL and SERVER_PID.
# The env is repeated here rather than going through webd_env(): backgrounding a
# shell function leaves an extra bash process between the test and the server,
# so a kill would reach the wrapper and not the process that holds the lock.
# WRITABLE_ENV=1 selects the environment-variable form of the opt-in instead of
# the flag.
WRITABLE_ENV=0
start() {
    local log=$1
    shift
    local args=()
    local extra=()
    [ -f "$BACKEND" ] && args+=(--backend "$BACKEND")
    [ "$WRITABLE_ENV" = "1" ] && extra+=(GNC_WEBUI_WRITABLE=1)
    env "${extra[@]}" GNC_UNINSTALLED=YES GNC_BUILDDIR="$BUILD" \
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

# stop <pid> -- ordinary SIGTERM; the server is expected to release its lock
stop() {
    local pid=$1
    kill "$pid" 2>/dev/null
    for _ in $(seq 1 100); do
        kill -0 "$pid" 2>/dev/null || break
        sleep 0.1
    done
    wait "$pid" 2>/dev/null || true
}

# request <method> <url> [outfile] -- sets CODE and BODY
request() {
    local method=$1 url=$2 out=${3:-$TMP/body}
    CODE=$(curl -sS -o "$out" -w '%{http_code}' -X "$method" "$url") \
        || fail "curl $method $url failed"
    BODY=$(cat "$out")
}

json_field() { # json_field <name> <body> -- first string value, unquoted
    echo "$2" | sed -n "s/.*\"$1\":\"\([^\"]*\)\".*/\1/p" | head -1
}

echo "== read-only (the default)"
start "$TMP/ro.log"
RO_PID=$SERVER_PID
RO_URL=$URL

[ ! -e "$BOOK.LCK" ] || fail "the read-only server created $BOOK.LCK"
grep -q 'webd mode:' "$TMP/ro.log" && fail "the read-only server printed a mode line"

request POST "${RO_URL}api/book/save"
[ "$CODE" = "403" ] || fail "read-only POST api/book/save answered $CODE (expected 403), body: $BODY"
echo "$BODY" | grep -q '"error":"read-only"' \
    || fail "the read-only refusal is not named 'read-only': $BODY"
echo "$BODY" | grep -q -- '--writable' \
    || fail "the read-only refusal does not say how to enable saving: $BODY"

request POST "${RO_URL}api/book/selftest"
[ "$CODE" = "403" ] || fail "read-only POST api/book/selftest answered $CODE (expected 403)"

request GET "${RO_URL}api/book/save"
[ "$CODE" = "405" ] || fail "GET api/book/save answered $CODE (expected 405)"

PORT=$(echo "$RO_URL" | sed -E 's|.*:([0-9]+)/.*|\1|')
CODE=$(curl -sS -o "$TMP/tokenless" -w '%{http_code}' -X POST \
    "http://127.0.0.1:$PORT/api/book/save")
[ "$CODE" = "404" ] || fail "a tokenless save answered $CODE (expected 404)"

cmp -s "$BOOK" "$TMP/pre.gnucash" \
    || fail "the read-only server changed the book on disk"

stop "$RO_PID"
[ ! -e "$BOOK.LCK" ] || fail "the read-only server left a lock file"
echo "read-only: 403 named, 405, 404, book untouched"

echo "== writable"
start "$TMP/rw.log" --writable
grep -q 'webd mode: writable' "$TMP/rw.log" \
    || fail "the writable server did not say so: $(cat "$TMP/rw.log")"
[ -e "$BOOK.LCK" ] || fail "the writable server did not take $BOOK.LCK"

request POST "${URL}api/book/save"
[ "$CODE" = "200" ] || fail "a clean save answered $CODE: $BODY"
echo "$BODY" | grep -q '"changed":false' \
    || fail "a clean book reported a change: $BODY"
echo "$BODY" | grep -q '"backup":null' \
    || fail "a clean save reported a backup: $BODY"

request POST "${URL}api/book/selftest"
[ "$CODE" = "200" ] || fail "the self-test answered $CODE: $BODY"
FROM_GUID=$(echo "$BODY" | sed -n 's/.*"from":{"account":"[^"]*","guid":"\([^"]*\)"}.*/\1/p')
[ -n "$FROM_GUID" ] || fail "the self-test returned no from-account guid: $BODY"
echo "$BODY" | grep -q '"description":"gnc-webd write-path selftest"' \
    || fail "the self-test described its transaction differently: $BODY"

request POST "${URL}api/book/save"
[ "$CODE" = "200" ] || fail "the save after a transaction answered $CODE: $BODY"
echo "$BODY" | grep -q '"changed":true' \
    || fail "the save after a transaction reported no change: $BODY"

BACKUP=$(json_field backup "$BODY")
[ -n "$BACKUP" ] || fail "the save named no backup: $BODY"
[ -f "$BACKUP" ] || fail "the named backup does not exist: $BACKUP"
cmp -s "$BACKUP" "$TMP/pre.gnucash" \
    || fail "the backup is not byte-for-byte the pre-save book"
cmp -s "$BOOK" "$TMP/pre.gnucash" \
    && fail "the book on disk did not change"
BYTES=$(echo "$BODY" | sed -n 's/.*"bytes":\([0-9]*\).*/\1/p')
[ "$BYTES" = "$(stat -c%s "$BOOK")" ] \
    || fail "the reported size ($BYTES) is not the file's ($(stat -c%s "$BOOK"))"
BACKUP_BYTES=$(echo "$BODY" | sed -n 's/.*"backup_bytes":\([0-9]*\).*/\1/p')
[ "$BACKUP_BYTES" = "$(stat -c%s "$BACKUP")" ] \
    || fail "the reported backup size is not the backup's"
echo "writable: saved $(stat -c%s "$BOOK") bytes, backup $BACKUP"

stop "$SERVER_PID"
[ ! -e "$BOOK.LCK" ] || fail "a clean stop left $BOOK.LCK behind"

echo "== GNC_WEBUI_WRITABLE=1 is the same opt-in"
WRITABLE_ENV=1
start "$TMP/env.log"
[ -e "$BOOK.LCK" ] || fail "the environment opt-in did not make the server writable"
grep -q 'webd mode: writable' "$TMP/env.log" \
    || fail "the environment opt-in did not report writable mode"
request POST "${URL}api/book/save"
[ "$CODE" = "200" ] || fail "a save under the environment opt-in answered $CODE: $BODY"
stop "$SERVER_PID"
WRITABLE_ENV=0
[ ! -e "$BOOK.LCK" ] || fail "the environment opt-in left $BOOK.LCK behind"

echo "== a failed backup refuses the save and leaves the book alone"
start "$TMP/fail.log" --writable
request POST "${URL}api/book/selftest"
[ "$CODE" = "200" ] || fail "the self-test before the failure answered $CODE: $BODY"
cp "$BOOK" "$TMP/before-failure.gnucash"
# The engine makes the backup in the book's directory.  Taking write permission
# away there makes backup_file() fail, which is the precondition the save must
# not proceed without.
chmod u-w "$TMP"
request POST "${URL}api/book/save"
chmod u+w "$TMP"
[ "$CODE" = "500" ] || fail "a save with no backup answered $CODE (expected 500): $BODY"
echo "$BODY" | grep -q '"error":"backup-failed"' \
    || fail "the failed-backup refusal is not named 'backup-failed': $BODY"
echo "$BODY" | grep -q '"path_exists":true' \
    || fail "the failed save does not say the book is still there: $BODY"
cmp -s "$BOOK" "$TMP/before-failure.gnucash" \
    || fail "the book changed even though the backup failed"
stop "$SERVER_PID"
echo "backup failure: 500 named, book untouched"

echo "== the saved book re-opens with the change"
start "$TMP/reopen.log"
request GET "${URL}api/register/$FROM_GUID"
[ "$CODE" = "200" ] || fail "re-opening the account register answered $CODE: $BODY"
echo "$BODY" | grep -q 'gnc-webd write-path selftest' \
    || fail "the transaction the self-test created is not in the saved book"
stop "$SERVER_PID"
echo "re-open: the transaction is on disk"

echo "== a locked book refuses writes, and names the lock"
start "$TMP/lockA.log" --writable
A_PID=$SERVER_PID
[ -e "$BOOK.LCK" ] || fail "the first writable server did not take the lock"

# A read-only process still opens the same book: the lock is not a read barrier.
start "$TMP/lockRO.log"
request GET "${URL}api/book"
[ "$CODE" = "200" ] || fail "a read-only server could not open a locked book: $CODE"
stop "$SERVER_PID"

start "$TMP/lockB.log" --writable
B_PID=$SERVER_PID
grep -q 'webd mode: read-only (locked:' "$TMP/lockB.log" \
    || fail "the second writable server did not report the lock: $(cat "$TMP/lockB.log")"
[ ! -e "$BOOK.LCK.tmp" ] || fail "the locked server created a second lock file"

request POST "${URL}api/book/save"
[ "$CODE" = "409" ] || fail "the locked book answered $CODE (expected 409): $BODY"
echo "$BODY" | grep -q '"error":"locked"' \
    || fail "the locked refusal is not named 'locked': $BODY"
echo "$BODY" | grep -qF "$BOOK.LCK" \
    || fail "the locked refusal does not name the lock file: $BODY"

request POST "${URL}api/book/selftest"
[ "$CODE" = "409" ] || fail "the locked self-test answered $CODE (expected 409)"

cp "$BOOK" "$TMP/locked-copy.gnucash"
stop "$B_PID"
stop "$A_PID"
[ ! -e "$BOOK.LCK" ] || fail "the writable server left $BOOK.LCK after a clean stop"
cmp -s "$BOOK" "$TMP/locked-copy.gnucash" \
    || fail "a refused save modified the book anyway"
echo "locked: 409 named, book unchanged"

echo "== --break-lock is explicit, and does not take over the lock"
: > "$BOOK.LCK"
start "$TMP/stale.log" --writable
grep -q 'webd mode: read-only (locked:' "$TMP/stale.log" \
    || fail "a stale lock was not reported: $(cat "$TMP/stale.log")"
request POST "${URL}api/book/save"
[ "$CODE" = "409" ] || fail "a stale lock answered $CODE (expected 409)"
stop "$SERVER_PID"

start "$TMP/break.log" --writable --break-lock
grep -q 'webd mode: writable' "$TMP/break.log" \
    || fail "--break-lock did not open the book writable: $(cat "$TMP/break.log")"
request POST "${URL}api/book/save"
[ "$CODE" = "200" ] || fail "--break-lock could not save: $CODE $BODY"
[ -e "$BOOK.LCK" ] \
    || fail "--break-lock removed a lock file it did not create"
stop "$SERVER_PID"
rm -f "$BOOK.LCK"
echo "break-lock: writable over a stale lock, the lock left alone"

echo "== --writable refuses a non-local book"
CODE=0
OUT=$(webd_env "$BUILD/bin/gnc-webd" --writable "mysql://example/db" 2>&1) || CODE=$?
[ "$CODE" -ne 0 ] || fail "--writable accepted a non-local book URI"
echo "$OUT" | grep -q 'local book files' \
    || fail "the non-local refusal does not explain itself: $OUT"

echo "book-save test passed"
