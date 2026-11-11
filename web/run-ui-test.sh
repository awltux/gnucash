#!/bin/bash
# Run the browser UI tests against freshly started servers.
#
# Usage: run-ui-test.sh <build-dir> <source-dir> [screenshot.png]
#
# One line, no manual cleanup:
#   bash web/run-ui-test.sh <build-dir> <source-dir>
#
# Each phase starts its own gnc-webd with its own throwaway
# GNC_WEBUI_REPORTS store, so a run never sees a report that another run (or a
# manual server on the default store) left behind.  The store is a file under a
# temporary directory that the trap removes; nothing under ~/.local/share is
# read or written.  The URL of each phase is printed.
#
#   phase 0  (no browser) run register-ui.mjs against phase 1's sample1
#                         server; it must refuse with one line.  This is the
#                         guard for the fixture declarations itself.
#   phase 1  sample1.gnucash  -> ui-test.mjs      the page, reports, designer,
#                                                 menu bar
#   phase 2  generated demo   -> register-ui.mjs  register grid, keyboard,
#                                                 transaction panel, menu items
#   phase 3  a copy of the demo, --writable
#                             -> register-edit-ui.mjs
#                                                 editing a cell, the dirty
#                                                 markers, Save, reload,
#                                                 unsaved-changes guard
#   phase 4  sample1.gnucash, --debug-port
#                             -> debug-port-ui.mjs
#                                                 the tokenless URL, the
#                                                 visible banner, natural
#                                                 /api/book and /report paths
#   phase 5  sample1.gnucash  -> pages-ui.mjs     the page URLs: direct
#                                                 navigation, reload, Back and
#                                                 Forward, the Book page's tabs,
#                                                 the menu's page items
#   phase 6  sample1.gnucash, --debug-port
#                             -> pages-ui.mjs     the same page checks without a
#                                                 token, where the base is "/"
#
# Every .mjs phase declares its FIXTURE and refuses a server that does not
# have that book (and, for register-edit-ui.mjs, a writable session) before it
# runs a single check.  GNC_UI_DEMO_ACCOUNTS carries the generated demo
# book's account count, read from the <book>.expect.json the generator wrote.
#
# Needs node and playwright (or playwright-core) on NODE_PATH, and python3 for
# the demo book.  Set PW_CHANNEL to use an installed browser (e.g.
# PW_CHANNEL=chrome) instead of the bundled Chromium, and PW_NO_SANDBOX=1 in
# containers that disable user namespaces.
set -u

BUILD=${1:?usage: run-ui-test.sh <build-dir> <source-dir> [screenshot.png]}
SRC=${2:?usage: run-ui-test.sh <build-dir> <source-dir> [screenshot.png]}
SHOT=${3:-}

BOOK="$SRC/libgnucash/backend/xml/test/test-files/load-save/sample1.gnucash"
SAMPLE1_ACCOUNTS=104
SCHEMAS="$BUILD/share/glib-2.0/schemas"
CCACHE=$(ls -d "$BUILD"/lib/*/guile/3.0/site-ccache 2>/dev/null | head -1)

fail() { echo "FAIL: $*" >&2; exit 1; }

[ -x "$BUILD/bin/gnc-webd" ] || fail "gnc-webd was not built"
[ -f "$BOOK" ] || fail "fixture book not found at $BOOK"
[ -f "$SRC/web/test/ui-test.mjs" ] || fail "ui-test.mjs not found"
[ -f "$SRC/web/test/register-ui.mjs" ] || fail "register-ui.mjs not found"
[ -f "$SRC/web/test/register-edit-ui.mjs" ] || fail "register-edit-ui.mjs not found"
[ -f "$SRC/web/test/debug-port-ui.mjs" ] || fail "debug-port-ui.mjs not found"
[ -f "$SRC/web/test/pages-ui.mjs" ] || fail "pages-ui.mjs not found"
[ -f "$SRC/web/test/fixture.mjs" ] || fail "fixture.mjs not found"
[ -f "$SRC/web/test/make-demo-book.py" ] || fail "make-demo-book.py not found"
command -v node >/dev/null || fail "node is not installed"
command -v python3 >/dev/null || fail "python3 is required for the demo book"

if [ ! -f "$SCHEMAS/gschemas.compiled" ]; then
    echo "note: no compiled schemas in $SCHEMAS (running with defaults)"
    SCHEMAS=""
fi

TMP=$(mktemp -d)
PIDS=()
cleanup() {
    for pid in "${PIDS[@]}"; do kill "$pid" 2>/dev/null; done
    rm -rf "$TMP"
}
trap cleanup EXIT

# The demo book is generated once, before the phases, so its expectation (the
# account count the generator's own arithmetic produced) is available to the
# fixture guard as well as to phase 2.  The expect file is written next to the
# book by make-demo-book.py.
python3 "$SRC/web/test/make-demo-book.py" "$TMP/demo.gnucash" >/dev/null \
    || fail "could not generate the demo book"
DEMO_ACCOUNTS=$(python3 -c '
import json, sys
with open(sys.argv[1], encoding="utf-8") as handle:
    print(json.load(handle)["accounts"])
' "$TMP/demo.gnucash.expect.json") || fail "could not read the demo book's expectations"
export GNC_UI_DEMO_ACCOUNTS="$DEMO_ACCOUNTS"

# start_server <book> <log> <store> [extra gnc-webd args...] -- sets URL.
start_server() {
    local book=$1 log=$2 store=$3
    shift 3
    env GNC_UNINSTALLED=YES GNC_BUILDDIR="$BUILD" \
        ${SCHEMAS:+GSETTINGS_SCHEMA_DIR="$SCHEMAS"} \
        LD_LIBRARY_PATH="$BUILD/lib:$BUILD/lib/gnucash" \
        GNC_MODULE_PATH="$BUILD/lib/gnucash" \
        GUILE_LOAD_PATH="$BUILD/share/guile/site/3.0" \
        GUILE_LOAD_COMPILED_PATH="${CCACHE:-}" \
        GNC_WEBUI_DIR="$SRC/web/ui" \
        GNC_WEBUI_REPORTS="$store" \
        "$BUILD/bin/gnc-webd" "$@" "$book" > "$log" 2>&1 &
    local pid=$!
    PIDS+=("$pid")

    # The Guile prelude can take tens of seconds on a cold cache, so wait a
    # minute for the token URL rather than thirty seconds.
    URL=""
    for _ in $(seq 1 300); do
        URL=$(grep -oE 'http://127\.0\.0\.1:[0-9]+/[0-9a-f]+/' "$log" | head -1)
        [ -n "$URL" ] && return 0
        kill -0 "$pid" 2>/dev/null || break
        sleep 0.2
    done
    cat "$log" >&2
    return 1
}

# start_debug_server <book> <log> <store> <port> -- sets URL to the tokenless
# --debug-port URL.  The URL is known before the server starts; the wait is for
# its "webd listening" line, not for a token.
start_debug_server() {
    local book=$1 log=$2 store=$3 port=$4
    env GNC_UNINSTALLED=YES GNC_BUILDDIR="$BUILD" \
        ${SCHEMAS:+GSETTINGS_SCHEMA_DIR="$SCHEMAS"} \
        LD_LIBRARY_PATH="$BUILD/lib:$BUILD/lib/gnucash" \
        GNC_MODULE_PATH="$BUILD/lib/gnucash" \
        GUILE_LOAD_PATH="$BUILD/share/guile/site/3.0" \
        GUILE_LOAD_COMPILED_PATH="${CCACHE:-}" \
        GNC_WEBUI_DIR="$SRC/web/ui" \
        GNC_WEBUI_REPORTS="$store" \
        "$BUILD/bin/gnc-webd" --debug-port "$port" "$book" > "$log" 2>&1 &
    local pid=$!
    PIDS+=("$pid")
    URL="http://127.0.0.1:$port/"
    for _ in $(seq 1 300); do
        grep -qF "webd listening on $URL" "$log" 2>/dev/null && return 0
        kill -0 "$pid" 2>/dev/null || break
        sleep 0.2
    done
    cat "$log" >&2
    return 1
}

STATUS=0

echo "== the page, reports, designer and menu bar"
start_server "$BOOK" "$TMP/sample.log" "$TMP/sample-reports.json" \
    || fail "server did not start"
echo "server: $URL"
node "$SRC/web/test/ui-test.mjs" "$URL" ${SHOT:+"$SHOT"} || STATUS=1

echo
echo "== the fixture guard: the demo-register phase must refuse sample1"
# The same phase that phase 2 runs against the demo book is pointed at this
# sample1 server.  It must refuse with one line naming the expected fixture
# and the found account count, and must print no check.
REFUSAL_LOG="$TMP/refusal.log"
REFUSAL_RC=0
node "$SRC/web/test/register-ui.mjs" "$URL" >"$REFUSAL_LOG" 2>&1 \
    || REFUSAL_RC=$?
REFUSED_LINE="REFUSED: register-ui.mjs needs the demo fixture ($DEMO_ACCOUNTS accounts), but the server has $SAMPLE1_ACCOUNTS accounts"
if [ "$REFUSAL_RC" -eq 0 ]; then
    echo "FAIL the demo-register phase accepted a sample1 server"
    STATUS=1
elif ! grep -qFx "$REFUSED_LINE" "$REFUSAL_LOG"; then
    echo "FAIL the refusal did not name the expected fixture and the found book"
    sed 's/^/     /' "$REFUSAL_LOG"
    STATUS=1
elif grep -qE '^(ok  |FAIL )' "$REFUSAL_LOG"; then
    echo "FAIL the refused phase still ran checks"
    STATUS=1
else
    echo "ok  a demo phase refuses a sample1 server before any check"
fi

echo
echo "== the read-only register: grid, keyboard, transaction panel"
start_server "$TMP/demo.gnucash" "$TMP/demo.log" "$TMP/demo-reports.json" \
    || fail "register server did not start"
echo "server: $URL"
node "$SRC/web/test/register-ui.mjs" "$URL" || STATUS=1

echo
echo "== the editable register: editing, dirty state, Save, reload"
# The edit phase writes, so it gets its own copy of the demo book: phase 2's
# book must stay exactly as generated, and phase 3 saves.
cp "$TMP/demo.gnucash" "$TMP/demo-edit.gnucash"
start_server "$TMP/demo-edit.gnucash" "$TMP/demo-edit.log" \
    "$TMP/demo-edit-reports.json" --writable \
    || fail "editable register server did not start"
echo "server: $URL"
node "$SRC/web/test/register-edit-ui.mjs" "$URL" || STATUS=1

echo
echo "== the tokenless --debug-port URL: banner and natural paths"
DEBUG_PORT=$(python3 -c '
import socket
sock = socket.socket()
sock.bind(("127.0.0.1", 0))
print(sock.getsockname()[1])
sock.close()
')
start_debug_server "$BOOK" "$TMP/debug.log" "$TMP/debug-reports.json" \
    "$DEBUG_PORT" || fail "debug-port server did not start"
echo "server: $URL"
grep -q 'WARNING: --debug-port' "$TMP/debug.log" \
    || fail "the debug-port server printed no warning"
node "$SRC/web/test/debug-port-ui.mjs" "$URL" || STATUS=1

echo
echo "== the page split: URLs, reload, history, the Book page's tabs"
start_server "$BOOK" "$TMP/pages.log" "$TMP/pages-reports.json" \
    || fail "pages server did not start"
echo "server: $URL"
node "$SRC/web/test/pages-ui.mjs" "$URL" || STATUS=1

echo
echo "== the page split under --debug-port: the same checks without a token"
DEBUG_PORT2=$(python3 -c '
import socket
sock = socket.socket()
sock.bind(("127.0.0.1", 0))
print(sock.getsockname()[1])
sock.close()
')
start_debug_server "$BOOK" "$TMP/pages-debug.log" \
    "$TMP/pages-debug-reports.json" "$DEBUG_PORT2" \
    || fail "tokenless pages server did not start"
echo "server: $URL"
node "$SRC/web/test/pages-ui.mjs" "$URL" || STATUS=1

exit "$STATUS"
