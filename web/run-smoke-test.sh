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
REPORTS_FILE=$(mktemp)
SPEC_STORE=$(mktemp)
trap 'kill "$SERVER_PID" 2>/dev/null; rm -f "$LOG" "$REPORTS_FILE" "$SPEC_STORE"' EXIT

env GNC_UNINSTALLED=YES GNC_BUILDDIR="$BUILD" \
    ${SCHEMAS:+GSETTINGS_SCHEMA_DIR="$SCHEMAS"} \
    LD_LIBRARY_PATH="$BUILD/lib:$BUILD/lib/gnucash" \
    GNC_MODULE_PATH="$BUILD/lib/gnucash" \
    GUILE_LOAD_PATH="$BUILD/share/guile/site/3.0" \
    GUILE_LOAD_COMPILED_PATH="${CCACHE:-}" \
    GNC_WEBUI_DIR="$SRC/web/ui" \
    GNC_WEBUI_REPORTS="$SPEC_STORE" \
    "$BUILD/bin/gnc-webd" --backend "$BACKEND" "$BOOK" > "$LOG" 2>&1 &
SERVER_PID=$!

# The server prints "webd listening on http://127.0.0.1:<port>/<token>/".
# The Guile build's prelude can take tens of seconds on a cold cache, so wait
# up to a minute rather than thirty seconds.
URL=""
for _ in $(seq 1 300); do
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

# Reports are native in every build: the Trial Balance is rendered by the
# C++ report layer, which does not need Guile.  A Guile build additionally
# lists the Scheme reports and falls back to them for names the native layer
# does not know; a build without Guile answers 501 for those.
HAS_GUILE=0
if [ -f "$BUILD/lib/gnucash/libgnc-report.so" ] ||
   [ -f "$BUILD/lib/libgnc-report.so" ]; then
    HAS_GUILE=1
fi

reports_code=$(curl -sS -o "$REPORTS_FILE" -w '%{http_code}' "${URL}api/reports") \
    || { cat "$LOG" >&2; fail "GET api/reports failed"; }
[ "$reports_code" = "200" ] || fail "GET api/reports answered $reports_code"
reports=$(cat "$REPORTS_FILE")
echo "$reports" | grep -q '"Trial Balance"' || fail "report list has no Trial Balance"
for native_report in "Balance Sheet" "Income Statement" "Profit & Loss" \
                     "Receivable Aging" "Payable Aging" "Investment Lots"; do
    echo "$reports" | grep -q "\"$native_report\"" \
        || fail "report list has no $native_report"
done
# The built-in designer specs are reports in every build: without Guile they
# are what the list offers beyond the native nine.
for builtin_report in "Income vs Expenses by Month" "Expenses by Account" \
                      "Income by Account" "Net Worth by Month"; do
    echo "$reports" | grep -q "\"$builtin_report\"" \
        || fail "report list has no built-in $builtin_report"
done
report_count=$(echo "$reports" | tr ',' '\n' | sed 's/^\[//' | grep -c '^"')
if [ "$HAS_GUILE" = 1 ]; then
    [ "$report_count" -gt 20 ] || fail "report list looks short ($report_count entries)"
fi
echo "reports listed: $report_count"

# Reports are rendered in this process, not by a helper.
report=$(curl -fsS "${URL}report/Trial%20Balance") || { cat "$LOG" >&2; fail "GET report failed"; }
echo "$report" | grep -q "<html" || fail "report is not HTML"
echo "$report" | grep -q "number-cell" || fail "native report has no amount cells"
echo "$report" | grep -q "Accounts Payable" || fail "native report has no account rows"
[ "${#report}" -gt 5000 ] || fail "report looks empty (${#report} bytes)"
echo "report bytes: ${#report}"

# The balance sheet and the P&L are native too: same engine, different
# grouping.  Check the numbers that make them recognisable rather than just
# that HTML came back.
balance_sheet=$(curl -fsS "${URL}report/Balance%20Sheet") \
    || { cat "$LOG" >&2; fail "GET Balance Sheet failed"; }
echo "$balance_sheet" | grep -q "Total Assets" || fail "balance sheet has no assets total"
echo "$balance_sheet" | grep -q "Total Liabilities &amp; Equity" \
    || fail "balance sheet has no liabilities+equity total"
echo "$balance_sheet" | grep -q "10,000.00" || fail "balance sheet lost the asset total"
echo "$balance_sheet" | grep -q "report-chart-canvas" \
    || fail "balance sheet has no chart canvas"
echo "$balance_sheet" | grep -q "report-chart-data" \
    || fail "balance sheet chart has no data block"

# Stage 4 ships Chart.js from the vendored copy; the report's <script> is only
# useful if this route serves it.  No CDN is involved.
chartjs=$(curl -fsS "${URL}vendor/chart.umd.js") \
    || { cat "$LOG" >&2; fail "GET vendor/chart.umd.js failed"; }
[ "${#chartjs}" -gt 100000 ] || fail "the vendored Chart.js looks truncated (${#chartjs} bytes)"

# The designer's vocabulary is published by the server, so the form is built
# from the same column/grouping/option names the renderer accepts.
curl -fsS "${URL}api/spec/options" | grep -q '"groupings"' \
    || fail "the designer vocabulary is not served"

# A built-in spec is a designer report with its own chart, served outside the
# book.  Check both halves: the table and the data the canvas draws.
builtin=$(curl -fsS "${URL}report/spec/builtin-income-expenses-month") \
    || { cat "$LOG" >&2; fail "GET a built-in spec report failed"; }
echo "$builtin" | grep -q "February 2018" \
    || fail "the built-in period report has no period row"
echo "$builtin" | grep -q "report-chart-canvas" \
    || fail "the built-in spec has no chart canvas"
echo "$builtin" | grep -qF '"series":[{"label":"Credit"' \
    || fail "the built-in chart has no two-series data block"
builtin_specs=$(curl -fsS "${URL}api/specs") \
    || fail "GET api/specs failed"
echo "$builtin_specs" | grep -q '"builtin":\[' \
    || fail "api/specs does not list the built-in specs"
echo "$builtin_specs" | grep -q '"specs":\[\]' \
    || fail "the built-in specs leaked into the saved store"

income_statement=$(curl -fsS "${URL}report/Income%20Statement") \
    || { cat "$LOG" >&2; fail "GET Income Statement failed"; }
echo "$income_statement" | grep -q "Total Revenue" || fail "income statement has no revenue total"
echo "$income_statement" | grep -q "Total Expenses" || fail "income statement has no expense total"
echo "$income_statement" | grep -q "Net income for Period\|Net loss for Period" \
    || fail "income statement has no net line"

# The aging reports read the business data: the fixture book has one posted
# vendor bill, so the Payable Aging prints its vendor and the Receivable Aging
# has nothing to age and prints the engine's explanation instead of a table.
payable_aging=$(curl -fsS "${URL}report/Payable%20Aging") \
    || { cat "$LOG" >&2; fail "GET Payable Aging failed"; }
echo "$payable_aging" | grep -q "Pre-Payment" \
    || fail "payable aging has no bucket headings"
echo "$payable_aging" | grep -q "Yoyodyne, Inc." \
    || fail "payable aging lost the book's vendor"
echo "$payable_aging" | grep -q "number-cell" \
    || fail "payable aging has no amount cells"
receivable_aging=$(curl -fsS "${URL}report/Receivable%20Aging") \
    || { cat "$LOG" >&2; fail "GET Receivable Aging failed"; }
echo "$receivable_aging" | grep -q "no suitable transactions" \
    || fail "receivable aging does not explain the empty fixture book"

# The Investment Lots report is native too.  sample1 has no stock account, so
# there are no lots to print; that it answers at all is this test's claim, and
# the lots themselves are compared cell by cell with the Scheme report by
# report-parity.sh on a generated book (web/test/make-investment-book.py).
investment_lots=$(curl -fsS "${URL}report/Investment%20Lots") \
    || { cat "$LOG" >&2; fail "GET Investment Lots failed"; }
echo "$investment_lots" | grep -q "<html" || fail "investment lots is not HTML"
echo "$investment_lots" | grep -q "Investment Lots" \
    || fail "investment lots report has no title"

if [ "$HAS_GUILE" = 1 ]; then
    # The Scheme engine is still the fallback for reports not ported yet.
    curl -fsS "${URL}report/Income%20Statement%20(Multicolumn)" | grep -q "<html" \
        || fail "Scheme fallback did not render Income Statement (Multicolumn)"
else
    unknown_code=$(curl -sS -o /dev/null -w '%{http_code}' \
        "${URL}report/Income%20Statement%20(Multicolumn)")
    [ "$unknown_code" = "501" ] \
        || fail "a non-native report answered $unknown_code without Guile (expected 501)"
fi

# Without the token the same path must not answer.
PORT=$(echo "$URL" | sed -E 's|.*:([0-9]+)/.*|\1|')
tokenless=$(curl -sS -o /dev/null -w '%{http_code}' "http://127.0.0.1:$PORT/api/book")
[ "$tokenless" = "404" ] || fail "a request without the token answered $tokenless"

# The UI page is served.
curl -fsS "${URL}" | grep -q "<title>GnuCash</title>" || fail "the UI page was not served"

echo "webd smoke test passed"
