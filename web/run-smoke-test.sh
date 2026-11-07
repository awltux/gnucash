#!/bin/bash
# Headless smoke test for gnc-webd.
#
# Usage: run-smoke-test.sh <build-dir> <source-dir>
#
# Starts the server on a loopback ephemeral port, reads the token it prints,
# and checks the read-only API. Everything runs against the fixture book, and
# the server is killed on exit.
set -u

BUILD=${1:?usage: run-smoke-test.sh <build-dir> <source-dir>}
SRC=${2:?usage: run-smoke-test.sh <build-dir> <source-dir>}

BOOK="$SRC/libgnucash/backend/xml/test/test-files/load-save/sample1.gnucash"
BACKEND="$BUILD/lib/gnucash/libgncmod-backend-xml.so"
SCHEMAS="$BUILD/share/glib-2.0/schemas"
CCACHE=$(ls -d "$BUILD"/lib/*/guile/3.0/site-ccache 2>/dev/null | head -1)

fail() { echo "FAIL: $*" >&2; exit 1; }

[ -x "$BUILD/bin/gnc-webd" ] || fail "gnc-webd was not built"
[ -f "$BOOK" ] || fail "fixture book not found at $BOOK"
if [ ! -f "$SCHEMAS/gschemas.compiled" ]; then
    # The schemas live in the GNOME tree; a build without it has none. The
    # server works from defaults, but say so rather than pretend all is well.
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
    "$BUILD/bin/gnc-webd" --backend "$BACKEND" "$BOOK" > "$LOG" 2>&1 &
SERVER_PID=$!

# The server prints "webd listening on http://127.0.0.1:<port>/<token>/".
URL=""
for _ in $(seq 1 150); do
    URL=$(grep -oE 'http://127\.0\.0\.1:[0-9]+/[0-9a-f]+/' "$LOG" | head -1)
    [ -n "$URL" ] && break
    kill -0 "$SERVER_PID" 2>/dev/null || break
    sleep 0.2
done
[ -n "$URL" ] || { cat "$LOG" >&2; fail "server did not start"; }

echo "server: $URL"

book=$(curl -fsS "${URL}api/book") || { cat "$LOG" >&2; fail "GET api/book failed"; }
echo "book: $book"

echo "$book" | grep -q '"accounts":104' || fail "expected 104 accounts, got: $book"
echo "$book" | grep -q '"top_level":5' || fail "expected 5 top level accounts, got: $book"

accounts=$(curl -fsS "${URL}api/accounts") || fail "GET api/accounts failed"
echo "$accounts" | grep -q '"name"' || fail "account tree has no names"
echo "$accounts" | grep -q '"total"' || fail "account tree has no totals"

# Without the token the same path must not answer.
PORT=$(echo "$URL" | sed -E 's|.*:([0-9]+)/.*|\1|')
tokenless=$(curl -sS -o /dev/null -w '%{http_code}' "http://127.0.0.1:$PORT/api/book")
[ "$tokenless" = "404" ] || fail "a request without the token answered $tokenless"

# The UI page is served.
curl -fsS "${URL}" | grep -q "<title>GnuCash</title>" || fail "the UI page was not served"

echo "webd smoke test passed"
