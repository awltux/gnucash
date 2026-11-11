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
#   phase 1  sample1.gnucash  -> ui-test.mjs      the page, reports, designer,
#                                                 menu bar
#   phase 2  generated demo   -> register-ui.mjs  register grid, keyboard,
#                                                 transaction panel, menu items
#   phase 3  a copy of the demo, --writable
#                             -> register-edit-ui.mjs
#                                                 editing a cell, the dirty
#                                                 markers, Save, reload,
#                                                 unsaved-changes guard
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
SCHEMAS="$BUILD/share/glib-2.0/schemas"
CCACHE=$(ls -d "$BUILD"/lib/*/guile/3.0/site-ccache 2>/dev/null | head -1)

fail() { echo "FAIL: $*" >&2; exit 1; }

[ -x "$BUILD/bin/gnc-webd" ] || fail "gnc-webd was not built"
[ -f "$BOOK" ] || fail "fixture book not found at $BOOK"
[ -f "$SRC/web/test/ui-test.mjs" ] || fail "ui-test.mjs not found"
[ -f "$SRC/web/test/register-ui.mjs" ] || fail "register-ui.mjs not found"
[ -f "$SRC/web/test/register-edit-ui.mjs" ] || fail "register-edit-ui.mjs not found"
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

STATUS=0

echo "== the page, reports, designer and menu bar"
start_server "$BOOK" "$TMP/sample.log" "$TMP/sample-reports.json" \
    || fail "server did not start"
echo "server: $URL"
node "$SRC/web/test/ui-test.mjs" "$URL" ${SHOT:+"$SHOT"} || STATUS=1

echo
echo "== the read-only register: grid, keyboard, transaction panel"
python3 "$SRC/web/test/make-demo-book.py" "$TMP/demo.gnucash" >/dev/null \
    || fail "could not generate the demo book"
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

exit "$STATUS"
