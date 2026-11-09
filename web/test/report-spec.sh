#!/bin/bash
# Designer report specs: built-ins, charts and the validation around them.
#
# Usage: report-spec.sh <build-dir> <source-dir> [book]
#
# Starts gnc-webd against the book (with an empty report store) and checks:
#   * the built-in specs are served and named in /api/reports;
#   * each built-in renders byte-for-byte the same document as the same spec
#     posted to /api/spec/preview by hand;
#   * a period chart takes its dates from the transactions when the spec says
#     date_preset "all", and its rows come from the table's own amount
#     columns;
#   * a nonsensical chart is a 400 whose body names the field and the problem.
#
# Exit status: 0 when every check passes, 1 otherwise.
set -u

BUILD=${1:?usage: report-spec.sh <build-dir> <source-dir> [book]}
SRC=${2:?usage: report-spec.sh <build-dir> <source-dir> [book]}
BOOK=${3:-$SRC/libgnucash/backend/xml/test/test-files/load-save/sample1.gnucash}

BACKEND="$BUILD/lib/gnucash/libgncmod-backend-xml.so"
SCHEMAS="$BUILD/share/glib-2.0/schemas"
CCACHE=$(ls -d "$BUILD"/lib/*/guile/3.0/site-ccache 2>/dev/null | head -1)

fail() { echo "FAIL: $*" >&2; exit 1; }

[ -x "$BUILD/bin/gnc-webd" ] || fail "gnc-webd was not built"
[ -f "$BOOK" ] || fail "fixture book not found at $BOOK"
[ -f "$SCHEMAS/gschemas.compiled" ] || SCHEMAS=""

TMP=$(mktemp -d)
REPORT_STORE="$TMP/reports.json"
SERVER_PID=
cleanup() {
    kill "$SERVER_PID" 2>/dev/null
    rm -rf "$TMP"
}
trap cleanup EXIT

BACKEND_ARGS=()
[ -f "$BACKEND" ] && BACKEND_ARGS=(--backend "$BACKEND")

env GNC_UNINSTALLED=YES GNC_BUILDDIR="$BUILD" \
    ${SCHEMAS:+GSETTINGS_SCHEMA_DIR="$SCHEMAS"} \
    LD_LIBRARY_PATH="$BUILD/lib:$BUILD/lib/gnucash" \
    GNC_MODULE_PATH="$BUILD/lib/gnucash" \
    GUILE_LOAD_PATH="$BUILD/share/guile/site/3.0" \
    GUILE_LOAD_COMPILED_PATH="${CCACHE:-}" \
    GNC_WEBUI_DIR="$SRC/web/ui" \
    GNC_WEBUI_REPORTS="$REPORT_STORE" \
    "$BUILD/bin/gnc-webd" "${BACKEND_ARGS[@]}" "$BOOK" > "$TMP/server.log" 2>&1 &
SERVER_PID=$!

URL=""
for _ in $(seq 1 300); do
    URL=$(grep -oE 'http://127\.0\.0\.1:[0-9]+/[0-9a-f]+/' "$TMP/server.log" | head -1)
    [ -n "$URL" ] && break
    kill -0 "$SERVER_PID" 2>/dev/null || break
    sleep 0.2
done
[ -n "$URL" ] || { cat "$TMP/server.log" >&2; fail "gnc-webd did not start"; }
echo "server: $URL"

FAILS=0
ok() { echo "ok   $*"; }
bad() { echo "FAIL $*"; FAILS=$((FAILS + 1)); }

fetch() { # path -> status, body in $BODY
    curl -sS -o "$TMP/body" -w '%{http_code}' "${URL%/}$1"
}

post_spec() { # json-file -> status, body in $TMP/body
    curl -sS -o "$TMP/body" -w '%{http_code}' -X POST \
        -H 'Content-Type: application/json' \
        --data-binary @"$1" "${URL%/}/api/spec/preview"
}

expect_status() { # want description path
    local code
    code=$(fetch "$3")
    if [ "$code" = "$1" ]; then
        ok "$code $3 ($2)"
    else
        bad "$3: expected $1, got $code ($2)"
    fi
}

expect_body() { # description needle
    if grep -qF -- "$2" "$TMP/body"; then
        ok "body names '$2' ($1)"
    else
        bad "body does not name '$2' ($1): $(head -c 300 "$TMP/body")"
    fi
}

# The chart data block as "type|labels|series-label:value,...".
chart_summary() { # html-file
    python3 - "$1" <<'PY'
import json, re, sys
html = open(sys.argv[1], encoding="utf-8").read()
match = re.search(
    r"<script type='application/json' id='report-chart-data'>(.*?)</script>",
    html, re.S)
if not match:
    print("none")
    raise SystemExit
data = json.loads(match.group(1))
series = ";".join(
    "%s:%s" % (item["label"], ",".join(str(value) for value in item["values"]))
    for item in data["series"])
print("%s|%s|%s" % (data["type"], "|".join(data["labels"]), series))
PY
}

echo "== the built-in specs are served"
expect_status 200 "spec list" "/api/specs"
cp "$TMP/body" "$TMP/specs.json"
BUILTIN_COUNT=$(python3 - "$TMP/specs.json" <<'PY'
import json, sys
with open(sys.argv[1], encoding="utf-8") as handle:
    data = json.load(handle)
print(len(data.get("builtin", [])))
PY
)
if [ "$BUILTIN_COUNT" -eq 4 ]; then
    ok "4 built-in specs"
else
    bad "expected 4 built-in specs, got $BUILTIN_COUNT"
fi

for pair in \
    "builtin-income-expenses-month|Income vs Expenses by Month" \
    "builtin-expenses-by-account|Expenses by Account" \
    "builtin-income-by-account|Income by Account" \
    "builtin-net-worth-month|Net Worth by Month"
do
    id=${pair%%|*}
    name=${pair#*|}
    expect_status 200 "$name spec" "/api/specs/$id"
    expect_body "$name spec" '"builtin":true'
    expect_body "$name spec" "\"name\":\"$name\""
    expect_status 200 "$name report" "/report/spec/$id"
    if grep -qF "report-chart-data" "$TMP/body"; then
        ok "$name renders a chart"
    else
        # Income by Account legitimately has no rows on a book with no income.
        if [ "$id" = "builtin-income-by-account" ]; then
            ok "$name has no income rows to chart in this book"
        else
            bad "$name has no chart data"
        fi
    fi
done

echo "== the report list names them"
expect_status 200 "report list" "/api/reports"
for name in "Income vs Expenses by Month" "Expenses by Account" \
            "Income by Account" "Net Worth by Month"; do
    if grep -qF "\"$name\"" "$TMP/body"; then
        ok "report list has $name"
    else
        bad "report list has no $name"
    fi
done

echo "== a built-in is the same spec built by hand"
python3 - "$TMP/specs.json" "$TMP" <<'PY'
import json, sys
with open(sys.argv[1], encoding="utf-8") as handle:
    data = json.load(handle)
for spec in data.get("builtin", []):
    with open(sys.argv[2] + "/" + spec["id"] + ".json", "w",
              encoding="utf-8") as handle:
        json.dump(spec, handle)
PY
for pair in \
    "builtin-income-expenses-month|Income vs Expenses by Month" \
    "builtin-expenses-by-account|Expenses by Account" \
    "builtin-income-by-account|Income by Account" \
    "builtin-net-worth-month|Net Worth by Month"
do
    id=${pair%%|*}
    name=${pair#*|}
    # The served document, including "builtin": true and the id, is exactly
    # what a client gets; posting it back is the hand-built spec.
    code=$(post_spec "$TMP/$id.json")
    if [ "$code" != "200" ]; then
        bad "$name preview answered $code: $(head -c 200 "$TMP/body")"
        continue
    fi
    cp "$TMP/body" "$TMP/$id-preview.html"
    fetch "/report/spec/$id" > /dev/null
    cp "$TMP/body" "$TMP/$id-report.html"
    if cmp -s "$TMP/$id-preview.html" "$TMP/$id-report.html"; then
        ok "$name renders identically through preview and the spec route"
    else
        bad "$name preview and saved render differ"
    fi
done

echo "== the chart consumes the spec's rows"
fetch "/report/spec/builtin-income-expenses-month" > /dev/null
cp "$TMP/body" "$TMP/ie.html"
SUMMARY=$(chart_summary "$TMP/ie.html")
if [ "$SUMMARY" = "line|February 2018|Credit:0;Debit:585" ]; then
    ok "income/expenses chart is the period rows' credit and debit columns"
else
    bad "unexpected income/expenses chart: $SUMMARY"
fi

# The table and the chart must agree: the Debit column of the February row is
# the Debit series, and the Credit column is the Credit series.
if grep -qF "February 2018" "$TMP/ie.html" &&
   grep -qF "<td class='number-cell'>\$585.00</td>" "$TMP/ie.html"; then
    ok "the chart's figures are the table's rows"
else
    bad "the period table does not show the chart's figures"
fi

# The same rule for account grouping: the labels are the account rows with
# activity, and the Debit series is the row's Debit column.
fetch "/report/spec/builtin-expenses-by-account" > /dev/null
cp "$TMP/body" "$TMP/ea.html"
SUMMARY=$(chart_summary "$TMP/ea.html")
if [ "$SUMMARY" = "bar|Office Supplies|Debit:585" ]; then
    ok "account grouping charts the account rows"
else
    bad "unexpected expenses-by-account chart: $SUMMARY"
fi

# A period chart with date_preset "all" takes its range from the book.  An
# explicit wider range adds the empty months, so the labels (and therefore the
# chart) change with the period.
cat > "$TMP/wider.json" <<'JSON'
{"name":"Wider range","columns":["account-name","debit","credit"],
 "grouping":"period","period_unit":"month","date_preset":"","start":"2017-01-01",
 "end":"2018-12-31","account_types":["INCOME","EXPENSE"],"chart":"line",
 "chart_series":["credit","debit"]}
JSON
code=$(post_spec "$TMP/wider.json")
if [ "$code" = "200" ]; then
    cp "$TMP/body" "$TMP/wider.html"
    WIDE=$(chart_summary "$TMP/wider.html")
    case "$WIDE" in
        line\|January\ 2017\|*February\ 2018*)
            ok "an explicit range changes the chart's periods" ;;
        *)
            bad "unexpected wider-range chart: $WIDE" ;;
    esac
else
    bad "wider-range preview answered $code: $(head -c 200 "$TMP/body")"
fi

echo "== nonsensical charts are named 400s"
reject() { # description needle json
    printf '%s' "$3" > "$TMP/reject.json"
    local code
    code=$(post_spec "$TMP/reject.json")
    if [ "$code" != "400" ]; then
        bad "$1: expected 400, got $code"
        return
    fi
    if grep -qF -- "$2" "$TMP/body"; then
        ok "400 names '$2' ($1)"
    else
        bad "$1: body does not name '$2': $(head -c 300 "$TMP/body")"
    fi
}

BASE='"name":"rejected","columns":["account-name","balance"],"date_preset":"all"'
reject "unknown chart type" "unknown chart 'pie'" \
       "{$BASE,\"chart\":\"pie\"}"
reject "chart without an amount column" "a chart needs a numeric column" \
       '{"name":"rejected","columns":["account-name"],"date_preset":"all","chart":"bar"}'
reject "text series" "is not an amount column" \
       "{$BASE,\"chart\":\"bar\",\"chart_series\":[\"account-name\"]}"
reject "duplicate series" "appears more than once" \
       "{$BASE,\"chart\":\"bar\",\"chart_series\":[\"balance\",\"balance\"]}"
reject "series not in the columns" "is not one of the report's columns" \
       '{"name":"rejected","columns":["account-name","balance"],"date_preset":"all","chart":"bar","chart_series":["debit"]}'
reject "net and part mixed" "cannot mix the 'balance' series" \
       '{"name":"rejected","columns":["account-name","balance","debit"],"date_preset":"all","chart":"bar","chart_series":["balance","debit"]}'
reject "too many series" "a chart draws one or two series" \
       '{"name":"rejected","columns":["account-name","debit","credit"],"date_preset":"all","chart":"bar","chart_series":["debit","credit","debit"]}'
reject "series without a chart" "needs a chart type in 'chart'" \
       "{$BASE,\"chart_series\":[\"balance\"]}"
reject "period without dates" "needs both a start and an end date, or date_preset 'all'" \
       '{"name":"rejected","columns":["account-name","balance"],"grouping":"period","date_preset":""}'
reject "cumulative without a period" "needs grouping 'period'" \
       "{$BASE,\"period_mode\":\"cumulative\"}"
reject "unknown account type" "unknown account type 'BOGUS'" \
       "{$BASE,\"account_types\":[\"BOGUS\"]}"
reject "root account type" "unknown account type 'ROOT'" \
       "{$BASE,\"account_types\":[\"ROOT\"]}"

echo "== built-ins are not saved reports"
expect_status 200 "get a built-in spec" \
    "/api/specs/builtin-income-expenses-month"
expect_body "get a built-in spec" '"builtin":true'
# DELETE is the method the UI would use for a saved report; a built-in is
# code, so it is refused rather than silently ignored.
code=$(curl -sS -o "$TMP/body" -w '%{http_code}' -X DELETE \
    "${URL%/}/api/specs/builtin-income-expenses-month")
if [ "$code" = "400" ] && grep -qF "cannot be deleted" "$TMP/body"; then
    ok "DELETE of a built-in is refused"
else
    bad "DELETE of a built-in answered $code: $(head -c 200 "$TMP/body")"
fi
expect_status 200 "saved specs stay empty" "/api/specs"
if grep -qF '"specs":[]' "$TMP/body"; then
    ok "the built-in specs are not written to the store"
else
    bad "a built-in leaked into the saved specs: $(head -c 200 "$TMP/body")"
fi

if [ "$FAILS" -ne 0 ]; then
    echo "$FAILS check(s) failed"
    exit 1
fi
echo "report spec surface passed"
