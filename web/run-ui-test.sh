#!/bin/bash
# Run the browser UI test against a freshly started server.
#
# Usage: run-ui-test.sh <build-dir> <source-dir> [screenshot.png]
#
# Needs node and playwright (or playwright-core) on NODE_PATH. Set PW_CHANNEL
# to use an installed browser (e.g. PW_CHANNEL=chrome) instead of the bundled
# Chromium, and PW_NO_SANDBOX=1 in containers that disable user namespaces.
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
command -v node >/dev/null || fail "node is not installed"

if [ ! -f "$SCHEMAS/gschemas.compiled" ]; then
    echo "note: no compiled schemas in $SCHEMAS (running with defaults)"
    SCHEMAS=""
fi

LOG=$(mktemp)
trap 'kill "$SERVER_PID" 2>/dev/null; rm -f "$LOG"' EXIT

env GNC_UNINSTALLED=YES GNC_BUILDDIR="$BUILD" \
    ${SCHEMAS:+GSETTINGS_SCHEMA_DIR="$SCHEMAS"} \
    LD_LIBRARY_PATH="$BUILD/lib:$BUILD/lib/gnucash" \
    GNC_MODULE_PATH="$BUILD/lib/gnucash" \
    GUILE_LOAD_PATH="$BUILD/share/guile/site/3.0" \
    GUILE_LOAD_COMPILED_PATH="${CCACHE:-}" \
    GNC_WEBUI_DIR="$SRC/web/ui" \
    "$BUILD/bin/gnc-webd" "$BOOK" > "$LOG" 2>&1 &
SERVER_PID=$!

URL=""
for _ in $(seq 1 150); do
    URL=$(grep -oE 'http://127\.0\.0\.1:[0-9]+/[0-9a-f]+/' "$LOG" | head -1)
    [ -n "$URL" ] && break
    kill -0 "$SERVER_PID" 2>/dev/null || break
    sleep 0.2
done
[ -n "$URL" ] || { cat "$LOG" >&2; fail "server did not start"; }

echo "server: $URL"
node "$SRC/web/test/ui-test.mjs" "$URL" ${SHOT:+"$SHOT"}
