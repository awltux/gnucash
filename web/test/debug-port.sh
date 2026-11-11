#!/bin/bash
# --debug-port: a stable, tokenless development URL on loopback (ADR 0012).
#
# Usage: debug-port.sh <build-dir> <source-dir>
#
# Starts gnc-webd against a *copy* of the fixture book and checks:
#   * the default mode still mints a per-run token: the token URL works, a
#     tokenless "/" and "/api/book" are 404, no warning is printed and the
#     page carries no debug banner;
#   * --debug-port <port> serves the natural paths on that exact port with no
#     token segment: "/", "/api/book" and "/api/session" all answer, the
#     session says debug_port, the startup warning names the risk, and the
#     page carries the visible banner;
#   * --debug-port with --writable is allowed and prints both warnings, and a
#     save still works;
#   * bare --debug-port uses 8984;
#   * a busy port is refused by name, with no silent fallback to an ephemeral
#     port and no "webd listening" line;
#   * a non-loopback bind (0.0.0.0:port, a LAN address) is refused by name
#     before the engine starts.
#
# The bare-flag check needs 8984 to be free; if something else on the machine
# holds it the test fails with that named reason rather than skipping.
#
# Exit status: 0 when every check passes, 1 otherwise.
set -u

BUILD=${1:?usage: debug-port.sh <build-dir> <source-dir>}
SRC=${2:?usage: debug-port.sh <build-dir> <source-dir>}

FIXTURE="$SRC/libgnucash/backend/xml/test/test-files/load-save/sample1.gnucash"
BACKEND="$BUILD/lib/gnucash/libgncmod-backend-xml.so"
SCHEMAS="$BUILD/share/glib-2.0/schemas"
CCACHE=$(ls -d "$BUILD"/lib/*/guile/3.0/site-ccache 2>/dev/null | head -1)

fail() { echo "FAIL: $*" >&2; exit 1; }

[ -x "$BUILD/bin/gnc-webd" ] || fail "gnc-webd was not built"
[ -f "$FIXTURE" ] || fail "fixture book not found at $FIXTURE"
[ -f "$SCHEMAS/gschemas.compiled" ] || SCHEMAS=""
command -v curl >/dev/null || fail "curl is required"
command -v python3 >/dev/null || fail "python3 is required"

TMP=$(mktemp -d)
PIDS=()
cleanup() {
    for pid in "${PIDS[@]}"; do kill "$pid" 2>/dev/null; done
    rm -rf "$TMP"
}
trap cleanup EXIT

BOOK="$TMP/book.gnucash"
cp "$FIXTURE" "$BOOK"

# free_port -- a port the kernel says is free right now.
free_port() {
    python3 -c '
import socket
sock = socket.socket()
sock.bind(("127.0.0.1", 0))
print(sock.getsockname()[1])
sock.close()
'
}

# start <log> <expected-port|""> [extra gnc-webd args...] -- sets URL and
# SERVER_PID.  With an expected port the debug URL must appear on it; with ""
# the normal token URL is read.
start() {
    local log=$1 expected=$2
    shift 2
    local args=()
    [ -f "$BACKEND" ] && args+=(--backend "$BACKEND")
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
    if [ -n "$expected" ]; then
        URL="http://127.0.0.1:$expected/"
        for _ in $(seq 1 300); do
            grep -qF "webd listening on $URL" "$log" 2>/dev/null && return 0
            kill -0 "$SERVER_PID" 2>/dev/null || break
            sleep 0.2
        done
    else
        for _ in $(seq 1 300); do
            URL=$(grep -oE 'http://127\.0\.0\.1:[0-9]+/[0-9a-f]+/' "$log" \
                2>/dev/null | head -1)
            [ -n "$URL" ] && return 0
            kill -0 "$SERVER_PID" 2>/dev/null || break
            sleep 0.2
        done
    fi
    cat "$log" >&2
    fail "server did not start as expected"
}

stop() {
    local pid=${1:-$SERVER_PID}
    kill "$pid" 2>/dev/null
    for _ in $(seq 1 100); do
        kill -0 "$pid" 2>/dev/null || break
        sleep 0.1
    done
    wait "$pid" 2>/dev/null || true
}

request() { # request <method> <url> [outfile] -- sets CODE and BODY
    local method=$1 url=$2 out=${3:-$TMP/body}
    CODE=$(curl -sS -o "$out" -w '%{http_code}' -X "$method" "$url") \
        || fail "curl $method $url failed"
    BODY=$(cat "$out")
}

port_of() { echo "$1" | sed -E 's|.*:([0-9]+)/.*|\1|'; }

echo "== the default mode still mints a token and serves nothing without it"
start "$TMP/default.log" ""
DEFAULT_URL=$URL
DEFAULT_PORT=$(port_of "$DEFAULT_URL")

request GET "${DEFAULT_URL}api/book"
[ "$CODE" = "200" ] || fail "the token URL's /api/book answered $CODE: $BODY"
echo "$BODY" | grep -q '"accounts"' \
    || fail "the token URL did not serve the book: $BODY"

request GET "http://127.0.0.1:$DEFAULT_PORT/api/book"
[ "$CODE" = "404" ] \
    || fail "the default mode served /api/book without a token ($CODE)"
request GET "http://127.0.0.1:$DEFAULT_PORT/"
[ "$CODE" = "404" ] || fail "the default mode served / without a token ($CODE)"

grep -q 'WARNING' "$TMP/default.log" \
    && fail "the default mode printed a debug warning"
request GET "${DEFAULT_URL}"
echo "$BODY" | grep -q 'debug-port-banner' \
    && fail "the default page carries the debug banner"
echo "default: token URL works, tokenless / and /api/book are 404, no banner"

request GET "${DEFAULT_URL}api/session"
echo "$BODY" | grep -q '"debug_port"' \
    && fail "the default session body carries debug_port: $BODY"
stop
echo "default: /api/session has no debug field"

echo "== --debug-port <port> serves the natural paths on that exact port"
DEBUG_PORT=$(free_port)
start "$TMP/debug.log" "$DEBUG_PORT" --debug-port "$DEBUG_PORT"
DEBUG_URL=http://127.0.0.1:$DEBUG_PORT/

request GET "$DEBUG_URL"
[ "$CODE" = "200" ] || fail "the debug page answered $CODE"
echo "$BODY" | grep -q 'id="debug-port-banner"' \
    || fail "the debug page has no visible banner"
echo "$BODY" | grep -q "no token in the URL" \
    || fail "the banner does not say the token is gone"
echo "$BODY" | grep -q "with --writable it can edit and save the book" \
    || fail "the banner does not name the writable risk"
echo "$BODY" | grep -q -- '--debug-port '"$DEBUG_PORT" \
    || fail "the banner does not name the debug port"

request GET "${DEBUG_URL}api/book"
[ "$CODE" = "200" ] || fail "/api/book without a token answered $CODE: $BODY"
echo "$BODY" | grep -q '"accounts"' \
    || fail "/api/book without a token did not serve the book: $BODY"

request GET "${DEBUG_URL}api/session"
[ "$CODE" = "200" ] || fail "/api/session answered $CODE: $BODY"
echo "$BODY" | grep -q "\"debug_port\":$DEBUG_PORT" \
    || fail "the session does not name the debug port: $BODY"

grep -q 'WARNING: --debug-port' "$TMP/debug.log" \
    || fail "the startup warning is missing: $(cat "$TMP/debug.log")"
grep -q 'without a token' "$TMP/debug.log" \
    || fail "the warning does not say the token is gone"
grep -q 'any local process can drive this UI' "$TMP/debug.log" \
    || fail "the warning does not name the risk"
grep -q 'webd listening on http://127.0.0.1:'"$DEBUG_PORT"'/$' "$TMP/debug.log" \
    || fail "the server did not print the tokenless URL on the given port"
stop
echo "debug: port $DEBUG_PORT, / and /api/book without a token, banner and warning"

echo "== --debug-port with --writable prints both warnings and can save"
WRITABLE_PORT=$(free_port)
start "$TMP/debug-writable.log" "$WRITABLE_PORT" --debug-port "$WRITABLE_PORT" --writable
WRITABLE_URL=http://127.0.0.1:$WRITABLE_PORT/
[ -e "$BOOK.LCK" ] || fail "the writable debug server did not take $BOOK.LCK"
WARNINGS=$(grep -c 'WARNING' "$TMP/debug-writable.log" || true)
[ "$WARNINGS" -ge 2 ] \
    || fail "the writable debug server printed $WARNINGS warning line(s), expected 2: $(cat "$TMP/debug-writable.log")"
grep -q 'with --writable it can edit and save the book' "$TMP/debug-writable.log" \
    || fail "the token warning does not name the writable risk"
grep -q 'WARNING: --writable with --debug-port' "$TMP/debug-writable.log" \
    || fail "the second warning does not name the writable book"
grep -qF "$BOOK" "$TMP/debug-writable.log" \
    || fail "the writable warning does not name the book"

request POST "${WRITABLE_URL}api/book/save"
[ "$CODE" = "200" ] || fail "a save through the debug URL answered $CODE: $BODY"
echo "$BODY" | grep -q '"saved":true' \
    || fail "the save through the debug URL did not succeed: $BODY"
stop
[ ! -e "$BOOK.LCK" ] || fail "the writable debug server left $BOOK.LCK behind"
echo "writable debug: two warnings, save 200, lock released"

echo "== bare --debug-port uses 8984"
# The bare-flag check is only meaningful if 8984 is free.  A previous run's
# accepted connections can leave the port in TIME_WAIT for a few seconds, so
# wait briefly before calling it busy; then say so by name rather than skip.
BARE_READY=0
for _ in $(seq 1 100); do
    if python3 -c '
import socket
sock = socket.socket()
sock.bind(("127.0.0.1", 8984))
sock.close()
' 2>/dev/null; then
        BARE_READY=1
        break
    fi
    sleep 0.2
done
[ "$BARE_READY" = "1" ] \
    || fail "port 8984 is still in use after 20s, so the bare --debug-port check cannot run"
start "$TMP/bare.log" 8984 --debug-port
request GET "http://127.0.0.1:8984/api/book"
[ "$CODE" = "200" ] || fail "bare --debug-port did not serve /api/book on 8984: $CODE"
grep -q 'webd listening on http://127.0.0.1:8984/$' "$TMP/bare.log" \
    || fail "bare --debug-port did not print the 8984 URL: $(cat "$TMP/bare.log")"
stop
echo "bare --debug-port: 8984"

echo "== a busy port is refused by name, with no ephemeral fallback"
BUSY_PORT=$(free_port)
python3 - "$BUSY_PORT" > "$TMP/busy-listener.log" 2>&1 <<'PY' &
import socket
import sys
import time

sock = socket.socket()
sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
sock.bind(("127.0.0.1", int(sys.argv[1])))
sock.listen(1)
print("listening", flush=True)
time.sleep(120)
PY
BUSY_PID=$!
PIDS+=("$BUSY_PID")
for _ in $(seq 1 100); do
    grep -q 'listening' "$TMP/busy-listener.log" 2>/dev/null && break
    sleep 0.1
done

CODE=0
env GNC_UNINSTALLED=YES GNC_BUILDDIR="$BUILD" \
    ${SCHEMAS:+GSETTINGS_SCHEMA_DIR="$SCHEMAS"} \
    LD_LIBRARY_PATH="$BUILD/lib:$BUILD/lib/gnucash" \
    GNC_MODULE_PATH="$BUILD/lib/gnucash" \
    GUILE_LOAD_PATH="$BUILD/share/guile/site/3.0" \
    GNC_WEBUI_DIR="$SRC/web/ui" \
    GNC_WEBUI_REPORTS="$TMP/reports.json" \
    "$BUILD/bin/gnc-webd" --debug-port "$BUSY_PORT" "$BOOK" \
    > "$TMP/busy.log" 2>&1 || CODE=$?
[ "$CODE" -ne 0 ] || fail "a busy --debug-port was accepted (exit 0)"
grep -q -- "--debug-port $BUSY_PORT is already in use" "$TMP/busy.log" \
    || fail "the busy-port refusal is not named: $(cat "$TMP/busy.log")"
grep -q 'refusing to fall back to an ephemeral port' "$TMP/busy.log" \
    || fail "the busy-port refusal does not say it refused to fall back"
grep -q 'webd listening on' "$TMP/busy.log" \
    && fail "the busy-port server still printed a listening line"
echo "busy port: refused by name ($BUSY_PORT), no fallback"

echo "== a non-loopback bind is refused by name"
for SPEC in "0.0.0.0:$(free_port)" "192.168.1.5:$(free_port)"; do
    CODE=0
    env GNC_UNINSTALLED=YES GNC_BUILDDIR="$BUILD" \
        ${SCHEMAS:+GSETTINGS_SCHEMA_DIR="$SCHEMAS"} \
        LD_LIBRARY_PATH="$BUILD/lib:$BUILD/lib/gnucash" \
        GNC_MODULE_PATH="$BUILD/lib/gnucash" \
        GUILE_LOAD_PATH="$BUILD/share/guile/site/3.0" \
        GNC_WEBUI_DIR="$SRC/web/ui" \
        GNC_WEBUI_REPORTS="$TMP/reports.json" \
        "$BUILD/bin/gnc-webd" --debug-port "$SPEC" "$BOOK" \
        > "$TMP/nonloop.log" 2>&1 || CODE=$?
    [ "$CODE" -ne 0 ] || fail "--debug-port accepted the non-loopback $SPEC"
    grep -q 'loopback only' "$TMP/nonloop.log" \
        || fail "the non-loopback refusal does not say loopback only: $(cat "$TMP/nonloop.log")"
    grep -qF "${SPEC%%:*}" "$TMP/nonloop.log" \
        || fail "the non-loopback refusal does not name ${SPEC%%:*}"
done
CODE=0
env GNC_UNINSTALLED=YES GNC_BUILDDIR="$BUILD" \
    ${SCHEMAS:+GSETTINGS_SCHEMA_DIR="$SCHEMAS"} \
    LD_LIBRARY_PATH="$BUILD/lib:$BUILD/lib/gnucash" \
    GNC_MODULE_PATH="$BUILD/lib/gnucash" \
    GUILE_LOAD_PATH="$BUILD/share/guile/site/3.0" \
    GNC_WEBUI_DIR="$SRC/web/ui" \
    GNC_WEBUI_REPORTS="$TMP/reports.json" \
    "$BUILD/bin/gnc-webd" --debug-port 99999 "$BOOK" \
    > "$TMP/badport.log" 2>&1 || CODE=$?
[ "$CODE" -ne 0 ] || fail "--debug-port accepted the out-of-range 99999"
grep -q 'between 1 and 65535' "$TMP/badport.log" \
    || fail "the bad-port refusal is not named: $(cat "$TMP/badport.log")"
echo "non-loopback and out-of-range: refused by name"

echo "debug-port test passed"
