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

# The table as one "cell | cell" string per row, so two documents can be
# compared cell for cell without depending on the markup's quote style.
table_cells() { # html-file -> stdout
    python3 - "$1" <<'PY'
import sys
from html.parser import HTMLParser

class Cells(HTMLParser):
    def __init__(self):
        super().__init__()
        self.rows = []
        self.row = None
        self.cell = None
    def handle_starttag(self, tag, attrs):
        if tag == "tr":
            self.row = []
        elif tag in ("td", "th") and self.row is not None:
            self.cell = []
    def handle_endtag(self, tag):
        if tag == "tr" and self.row is not None:
            self.rows.append([" ".join("".join(c).split()) for c in self.row])
            self.row = None
        elif tag in ("td", "th") and self.cell is not None:
            self.row.append(self.cell)
            self.cell = None
    def handle_data(self, data):
        if self.cell is not None:
            self.cell.append(data)

parser = Cells()
parser.feed(open(sys.argv[1], encoding="utf-8").read())
for row in parser.rows:
    print(" | ".join(row))
PY
}

compare_cells() { # description native-html preview-html
    local native preview
    native=$(mktemp)
    preview=$(mktemp)
    table_cells "$2" > "$native"
    table_cells "$3" > "$preview"
    if cmp -s "$native" "$preview"; then
        ok "$1"
    else
        bad "$1"
        diff -u "$native" "$preview" | head -20 >&2
    fi
    rm -f "$native" "$preview"
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
if [ "$BUILTIN_COUNT" -eq 6 ]; then
    ok "6 built-in specs"
else
    bad "expected 6 built-in specs, got $BUILTIN_COUNT"
fi

for pair in \
    "builtin-income-expenses-month|Income vs Expenses by Month" \
    "builtin-expenses-by-account|Expenses by Account" \
    "builtin-income-by-account|Income by Account" \
    "builtin-net-worth-month|Net Worth by Month" \
    "builtin-net-worth-barchart|Net Worth Barchart" \
    "builtin-net-worth-linechart|Net Worth Linechart"
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
            "Income by Account" "Net Worth by Month" \
            "Net Worth Barchart" "Net Worth Linechart"; do
    if grep -qF "\"$name\"" "$TMP/body"; then
        ok "report list has $name"
    else
        bad "report list has no $name"
    fi
done

echo "== tags filter the list"
# The ids in a /api/specs response, saved specs first then built-ins, in the
# order the endpoint printed them.
list_ids() { # json-file
    python3 - "$1" <<'PY'
import json, sys
with open(sys.argv[1], encoding="utf-8") as handle:
    data = json.load(handle)
for spec in data.get("specs", []) + data.get("builtin", []):
    print(spec["id"])
PY
}

expect_ids() { # description expected-space-separated path
    local code got
    code=$(fetch "$3")
    if [ "$code" != "200" ]; then
        bad "$3: expected 200, got $code ($1)"
        return
    fi
    got=$(list_ids "$TMP/body" | paste -sd ' ' -)
    if [ "$got" = "$2" ]; then
        ok "$1 -> [$got]"
    else
        bad "$1: expected [$2], got [$got]"
    fi
}

# A distinct tag selects exactly the built-in that carries it.
expect_ids "one built-in by tag" "builtin-net-worth-barchart" \
    "/api/specs?tag=bar-chart"
# A shared tag selects all of them, not just the first.
expect_ids "two built-ins by tag" \
    "builtin-expenses-by-account builtin-income-by-account" \
    "/api/specs?tag=breakdown"
expect_ids "tag matching is case-insensitive" "builtin-net-worth-linechart" \
    "/api/specs?tag=LINE-CHART"
expect_ids "an unknown tag matches nothing" "" "/api/specs?tag=no-such-tag"
expect_status 200 "unknown tag" "/api/specs?tag=no-such-tag"
if [ "$(cat "$TMP/body")" = '{"specs":[],"builtin":[]}' ]; then
    ok "an unknown tag is an empty array, not an error"
else
    bad "an unknown tag answered: $(head -c 200 "$TMP/body")"
fi

# ?q= searches the name, the description and the tags.  "worth" is in three
# names; "trend" is in no name and in exactly two tag lists.
expect_ids "q matches names" \
    "builtin-net-worth-month builtin-net-worth-barchart builtin-net-worth-linechart" \
    "/api/specs?q=worth"
expect_ids "q matches tags" \
    "builtin-income-expenses-month builtin-net-worth-month" \
    "/api/specs?q=trend"
expect_ids "q is case-insensitive" \
    "builtin-income-expenses-month builtin-net-worth-month" \
    "/api/specs?q=TREND"
# Both filters compose: net-worth keeps three, then "bar" keeps the barchart.
expect_ids "tag and q compose" "builtin-net-worth-barchart" \
    "/api/specs?tag=net-worth&q=bar"
expect_ids "tag and q compose to nothing" "" \
    "/api/specs?tag=breakdown&q=worth"

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
    "builtin-net-worth-month|Net Worth by Month" \
    "builtin-net-worth-barchart|Net Worth Barchart" \
    "builtin-net-worth-linechart|Net Worth Linechart"
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

echo "== the designer describes native reports honestly"

# A native report is C++.  The designer route must name the implementation,
# and it may only offer a starting point it can back up with cells.  Trial
# Balance is the report whose C++ table is exactly a spec, so its starting
# point must render the native report's cells; the income statements are
# labelled approximate; a listing report gets no fields at all.
expect_status 200 "Trial Balance designer" \
    "/api/report/Trial%20Balance/designer"
cp "$TMP/body" "$TMP/tb-designer.json"
expect_body "Trial Balance designer names the implementation" \
    '"file":"libgnucash/reports/gnc-report.cpp"'
expect_body "Trial Balance designer names the function" \
    '"symbol":"trial_balance()"'
expect_body "Trial Balance starting point is exact" '"equivalence":"exact"'

python3 - "$TMP/tb-designer.json" "$TMP/tb-start.json" <<'PY'
import json, sys
with open(sys.argv[1], encoding="utf-8") as handle:
    info = json.load(handle)
assert info["equivalence"] == "exact", info["equivalence"]
assert info["spec"]["grouping"] == "none", info["spec"]
assert info["spec"]["columns"] == ["account-name", "debit", "credit"]
with open(sys.argv[2], "w", encoding="utf-8") as handle:
    json.dump(info["spec"], handle)
PY
curl -sS -o "$TMP/tb-native.html" "${URL%/}/report/Trial%20Balance"
code=$(post_spec "$TMP/tb-start.json")
if [ "$code" = "200" ]; then
    cp "$TMP/body" "$TMP/tb-start.html"
    compare_cells "the Trial Balance starting point renders the native cells" \
        "$TMP/tb-native.html" "$TMP/tb-start.html"
else
    bad "Trial Balance starting point preview answered $code: $(head -c 200 "$TMP/body")"
fi

# The income statements have no exact spec, so their starting point must be
# labelled approximate and must say how it differs: the account set and the
# period are the report's, the columns, signs and totals are not.
for pair in "Income%20Statement|Income Statement" "Profit%20%26%20Loss|Profit & Loss"; do
    enc=${pair%%|*}
    name=${pair#*|}
    expect_status 200 "$name designer" "/api/report/$enc/designer"
    cp "$TMP/body" "$TMP/is-designer.json"
    expect_body "$name starting point is approximate" \
        '"equivalence":"approximate"'
    expect_body "$name says how it is approximate" \
        "sign-reversed Balance column"
    python3 - "$TMP/is-designer.json" "$TMP/is-start.json" <<'PY'
import json, sys
with open(sys.argv[1], encoding="utf-8") as handle:
    info = json.load(handle)
assert info["equivalence"] == "approximate", info["equivalence"]
assert info["spec"]["account_types"] == ["INCOME", "EXPENSE"], info["spec"]
assert sorted(info["spec"]["columns"]) == ["account-name", "credit", "debit"]
with open(sys.argv[2], "w", encoding="utf-8") as handle:
    json.dump(info["spec"], handle)
PY
    code=$(post_spec "$TMP/is-start.json")
    title_ok=$(python3 - "$TMP/body" "$name (starting point)" <<'PY'
import html, re, sys
text = open(sys.argv[1], encoding="utf-8").read()
match = re.search(r"<title>(.*?)</title>", text, re.S)
print("yes" if match and html.unescape(match.group(1)) == sys.argv[2]
      else "no")
PY
)
    if [ "$code" = "200" ] && [ "$title_ok" = "yes" ]; then
        ok "$name approximate starting point previews"
    else
        bad "$name approximate preview answered $code: $(head -c 200 "$TMP/body")"
    fi
done

# A listing report has no equivalent: no spec and a note saying what the
# designer's account-row vocabulary cannot express.
expect_status 200 "Transaction Report designer" \
    "/api/report/Transaction%20Report/designer"
expect_body "Transaction Report has no equivalent" '"equivalence":"none"'
expect_body "Transaction Report has no starting spec" '"spec":null'
expect_body "Transaction Report says why" "lists transactions and splits"

# Every native report answers, so no report can silently lack a description
# (an unknown name is a 404, checked below).
expect_status 200 "native report list with options" "/api/reports?options=1"
cp "$TMP/body" "$TMP/native-options.json"
NATIVE_NAMES=$(python3 - "$TMP/native-options.json" <<'PY'
import json, sys
with open(sys.argv[1], encoding="utf-8") as handle:
    data = json.load(handle)
print("\n".join(entry["report"] for entry in data))
PY
)
DESIGNER_MISSING=0
DESIGNER_TOTAL=0
while IFS= read -r native; do
    [ -n "$native" ] || continue
    DESIGNER_TOTAL=$((DESIGNER_TOTAL + 1))
    encoded=$(python3 -c \
        'import sys,urllib.parse; print(urllib.parse.quote(sys.argv[1]))' \
        "$native")
    code=$(fetch "/api/report/$encoded/designer")
    if [ "$code" != "200" ]; then
        bad "designer description for $native answered $code"
        DESIGNER_MISSING=$((DESIGNER_MISSING + 1))
    elif ! grep -qE '"equivalence":"(exact|approximate|none)"' "$TMP/body"; then
        bad "designer description for $native has no equivalence"
        DESIGNER_MISSING=$((DESIGNER_MISSING + 1))
    fi
done <<< "$NATIVE_NAMES"
if [ "$DESIGNER_MISSING" -eq 0 ] && [ "$DESIGNER_TOTAL" -eq 39 ]; then
    ok "all 39 native reports publish a designer description"
else
    bad "$DESIGNER_MISSING of $DESIGNER_TOTAL native reports lack one"
fi
expect_status 404 "designer description for an unknown report" \
    "/api/report/No%20Such%20Report/designer"

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

# The two net-worth built-ins port the Scheme reports' three series: the
# asset side, the liability side and the net.  Their labels are the built-in
# period rows, not the Scheme report's step dates.
for pair in "builtin-net-worth-barchart|bar" "builtin-net-worth-linechart|line"; do
    id=${pair%%|*}
    kind=${pair#*|}
    fetch "/report/spec/$id" > /dev/null
    cp "$TMP/body" "$TMP/$id.html"
    SUMMARY=$(chart_summary "$TMP/$id.html")
    if [ "$SUMMARY" = "$kind|February 2018|Debit:10000;Credit:585;Balance:9415" ]; then
        ok "$id charts the asset, liability and net series"
    else
        bad "unexpected $id chart: $SUMMARY"
    fi
done

# The chart's three figures are the table's own Debit/Credit/Balance cells.
if grep -qF "<td class='number-cell'>\$10,000.00</td>" \
        "$TMP/builtin-net-worth-barchart.html" &&
   grep -qF "<td class='number-cell'>\$585.00</td>" \
        "$TMP/builtin-net-worth-barchart.html" &&
   grep -qF "<td class='number-cell'>\$9,415.00</td>" \
        "$TMP/builtin-net-worth-barchart.html"; then
    ok "the net-worth chart's figures are the table's rows"
else
    bad "the net-worth table does not show the chart's figures"
fi

echo "== a period chart can draw the third series"
cat > "$TMP/three.json" <<'JSON'
{"name":"Three series","columns":["account-name","debit","credit","balance"],
 "grouping":"period","period_unit":"month","date_preset":"",
 "start":"2018-01-01","end":"2018-02-28",
 "account_types":["INCOME","EXPENSE"],"chart":"bar",
 "chart_series":["credit","debit","balance"]}
JSON
code=$(post_spec "$TMP/three.json")
if [ "$code" = "200" ]; then
    cp "$TMP/body" "$TMP/three.html"
    THREE=$(chart_summary "$TMP/three.html")
    if [ "$THREE" = "bar|January 2018|February 2018|Credit:0,0;Debit:0,585;Balance:0,585" ]; then
        ok "the period chart draws credit, debit and the net balance"
    else
        bad "unexpected three-series chart: $THREE"
    fi
else
    bad "three-series preview answered $code: $(head -c 200 "$TMP/body")"
fi

# A narrower scope changes the series: assets only drop the liability side.
cat > "$TMP/nw-assets.json" <<'JSON'
{"name":"Assets only","columns":["account-name","debit","credit","balance"],
 "grouping":"period","period_unit":"month","period_mode":"cumulative",
 "date_preset":"all",
 "account_types":["BANK","CASH","ASSET","STOCK","MUTUAL","CURRENCY",
                  "RECEIVABLE"],
 "chart":"bar","chart_series":["debit","credit","balance"]}
JSON
code=$(post_spec "$TMP/nw-assets.json")
if [ "$code" = "200" ]; then
    cp "$TMP/body" "$TMP/nw-assets.html"
    ASSETS=$(chart_summary "$TMP/nw-assets.html")
    if [ "$ASSETS" = "bar|February 2018|Debit:10000;Credit:0;Balance:10000" ]; then
        ok "a narrower account scope changes the three series"
    else
        bad "unexpected assets-only chart: $ASSETS"
    fi
else
    bad "assets-only preview answered $code: $(head -c 200 "$TMP/body")"
fi

# And a wider period changes the labels and the series length.  The axis is
# the Scheme net-worth chart's: the from-date, then the from-date stepped by
# the period, then the range end when the step does not land on it.  A 12/01
# start is not a month end, so the first point is December 2017 itself and the
# 12/31 end is appended after December 2018 -- two December 2018 labels, the
# last one the range end.
cat > "$TMP/nw-wide.json" <<'JSON'
{"name":"Net worth over 2018","columns":["account-name","debit","credit","balance"],
 "grouping":"period","period_unit":"month","period_mode":"cumulative",
 "date_preset":"","start":"2017-12-01","end":"2018-12-31",
 "account_types":["BANK","CASH","ASSET","STOCK","MUTUAL","CURRENCY",
                  "CREDIT","LIABILITY","RECEIVABLE","PAYABLE"],
 "chart":"bar","chart_series":["debit","credit","balance"]}
JSON
code=$(post_spec "$TMP/nw-wide.json")
if [ "$code" = "200" ]; then
    cp "$TMP/body" "$TMP/nw-wide.html"
    WIDE=$(chart_summary "$TMP/nw-wide.html")
    case "$WIDE" in
        bar\|December\ 2017\|January\ 2018\|*November\ 2018\|December\ 2018\|December\ 2018\|Debit:0,0,0,10000,*)
            ok "an explicit mid-month period starts at the from-date and appends the range end" ;;
        *)
            bad "unexpected wider net-worth chart: $WIDE" ;;
    esac
else
    bad "wider net-worth preview answered $code: $(head -c 200 "$TMP/body")"
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
reject "too many series" "a chart draws one, two or three series" \
       '{"name":"rejected","columns":["account-name","debit","credit"],"date_preset":"all","chart":"bar","chart_series":["debit","credit","balance","debit"]}'
reject "three series on a flat list" "needs grouping 'period'" \
       '{"name":"rejected","columns":["account-name","debit","credit","balance"],"date_preset":"all","chart":"bar","chart_series":["debit","credit","balance"]}'
reject "balance with only one side" "unless it draws all three" \
       '{"name":"rejected","columns":["account-name","debit","balance"],"grouping":"period","date_preset":"all","chart":"bar","chart_series":["debit","balance"]}'
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

echo "== a spec can list splits, and says which source it means"

# /api/spec/options advertises the source list and the split vocabulary, so
# the designer form (and a hand-written spec) can be built from the native
# layer's own names.
fetch "/api/spec/options" > /dev/null
expect_body "the options advertise the source list" '"name":"splits"'
expect_body "the options advertise the split columns" '"name":"running-balance"'
expect_body "the options advertise the split groupings" '"name":"account-code"'

cat > "$TMP/split.json" <<'JSON'
{"name":"Split listing","source":"splits","split_grouping":"account",
 "columns":["date","num","description","memo","transfer","debit","credit"]}
JSON
code=$(post_spec "$TMP/split.json")
if [ "$code" = "200" ]; then
    ok "a split spec previews"
    cp "$TMP/body" "$TMP/split-preview.html"
    if grep -q "column-heading-right" "$TMP/split-preview.html" &&
        grep -q "<th>Date</th>" "$TMP/split-preview.html"; then
        ok "the split table prints its typed columns"
    else
        bad "the split table does not print its columns: $(head -c 300 "$TMP/split-preview.html")"
    fi
else
    bad "a split spec preview answered $code: $(head -c 300 "$TMP/body")"
fi

# A missing source is the account report it has always been.
cat > "$TMP/no-source.json" <<'JSON'
{"name":"No source","columns":["account-name","balance"],"date_preset":"all"}
JSON
code=$(post_spec "$TMP/no-source.json")
if [ "$code" = "200" ]; then
    ok "a spec without 'source' is an account report"
else
    bad "a spec without 'source' answered $code: $(head -c 300 "$TMP/body")"
fi

# A column from the other source is named, and so is the vocabulary that would
# have been accepted.
reject "an account column in a split spec" "unknown split column 'balance'" \
       '{"name":"rejected","source":"splits","columns":["balance"]}'
reject "a split column in an account spec" "unknown column 'running-balance'" \
       '{"name":"rejected","columns":["running-balance"]}'
reject "an unknown source" "unknown source 'periods'" \
       '{"name":"rejected","source":"periods","columns":["debit"]}'
reject "split_grouping on an account spec" "'split_grouping' is for a spec" \
       '{"name":"rejected","columns":["balance"],"split_grouping":"account"}'
reject "an unknown split_grouping" "unknown split_grouping 'month'" \
       '{"name":"rejected","source":"splits","columns":["debit"],"split_grouping":"month"}'
reject "the account grouping on a split spec" "a split listing has no 'grouping'" \
       '{"name":"rejected","source":"splits","columns":["debit"],"grouping":"period"}'
reject "a chart on a split listing" "a split listing has no chart" \
       '{"name":"rejected","source":"splits","columns":["debit"],"chart":"bar"}'
reject "dates on the transaction layout" "covers the whole book" \
       '{"name":"rejected","source":"splits","split_grouping":"transaction","columns":["debit"],"start":"2018-01-01"}'
reject "an account scope on the transaction layout" "covers the whole book" \
       '{"name":"rejected","source":"splits","split_grouping":"transaction","columns":["debit"],"accounts":["547d8ff75f4347e3b3607c40b40a3fef"]}'

# The source survives the store, and a saved split spec renders exactly what
# its preview rendered: the designer-parity invariant covers the new source.
code=$(curl -sS -o "$TMP/body" -w '%{http_code}' -X POST \
    -H 'Content-Type: application/json' --data-binary @"$TMP/split.json" \
    "${URL%/}/api/specs")
if [ "$code" = "201" ]; then
    ok "201 saving a split spec"
    cp "$TMP/body" "$TMP/split-saved.json"
    if grep -q '"source":"splits"' "$TMP/body" &&
        grep -q '"split_grouping":"account"' "$TMP/body"; then
        ok "the save response keeps the source and the split grouping"
    else
        bad "the save response does not keep the source: $(head -c 300 "$TMP/body")"
    fi
    SPLIT_ID=$(python3 -c \
        'import json,sys; print(json.load(open(sys.argv[1]))["id"])' \
        "$TMP/split-saved.json")
    fetch "/report/spec/$SPLIT_ID" > /dev/null
    cp "$TMP/body" "$TMP/split-report.html"
    if cmp -s "$TMP/split-preview.html" "$TMP/split-report.html"; then
        ok "a split spec renders identically through preview and the spec route"
    else
        bad "a saved split spec renders differently from its preview"
    fi
    # The store is re-read on every request: this GET is the reload.
    expect_status 200 "the split spec after a store reload" "/api/specs/$SPLIT_ID"
    expect_body "the split spec after a store reload" '"source":"splits"'
    code=$(curl -sS -o "$TMP/body" -w '%{http_code}' -X DELETE \
        "${URL%/}/api/specs/$SPLIT_ID")
    [ "$code" = "200" ] && ok "the split spec can be deleted again" ||
        bad "deleting the split spec answered $code"
else
    bad "saving a split spec answered $code: $(head -c 300 "$TMP/body")"
fi

echo "== invalid tags are named 400s"
reject "uppercase tag" "invalid tag 'Net Worth'" \
       "{$BASE,\"tags\":[\"ok\",\"Net Worth\"]}"
reject "tag with an underscore" "invalid tag 'bad_tag'" \
       "{$BASE,\"tags\":[\"bad_tag\"]}"
reject "tag longer than 24 characters" \
       "invalid tag 'abcdefghijklmnopqrstuvwxyz'" \
       "{$BASE,\"tags\":[\"abcdefghijklmnopqrstuvwxyz\"]}"
reject "more than 12 tags" "'tags' has more than 12 entries" \
       "{$BASE,\"tags\":[\"t1\",\"t2\",\"t3\",\"t4\",\"t5\",\"t6\",\"t7\",\"t8\",\"t9\",\"t10\",\"t11\",\"t12\",\"t13\"]}"
reject "tags must be strings" "'tags' entries must be strings" \
       "{$BASE,\"tags\":[\"ok\",7]}"
reject "tags must be an array" "'tags' must be an array" \
       "{$BASE,\"tags\":\"ok\"}"

echo "== a saved spec's tags live in the store"
cat > "$TMP/tagged.json" <<'JSON'
{"name":"Tagged report","columns":["account-name","balance"],
 "date_preset":"all","tags":[" spending ","monthly","spending"]}
JSON
code=$(curl -sS -o "$TMP/body" -w '%{http_code}' -X POST \
    -H 'Content-Type: application/json' --data-binary @"$TMP/tagged.json" \
    "${URL%/}/api/specs")
if [ "$code" = "201" ]; then
    ok "201 saving a spec with tags"
    cp "$TMP/body" "$TMP/tagged-saved.json"
    if grep -qF '"tags":["spending","monthly"]' "$TMP/body"; then
        ok "the save response has the trimmed, deduplicated tags"
    else
        bad "unexpected tags in the save response: $(head -c 300 "$TMP/body")"
    fi
    TAG_ID=$(python3 -c \
        'import json,sys; print(json.load(open(sys.argv[1]))["id"])' \
        "$TMP/tagged-saved.json")

    # The store is a file, re-read on every request: this GET is the reload
    # after the save, and the tags must still be there.
    expect_status 200 "the tagged spec after a store reload" \
        "/api/specs/$TAG_ID"
    expect_body "the tagged spec after a store reload" \
        '"tags":["spending","monthly"]'
    if grep -qF '"tags":["spending","monthly"]' "$REPORT_STORE"; then
        ok "the tags are written to the report store file"
    else
        bad "tags missing from the store file $REPORT_STORE"
    fi

    # Saved and built-in specs are filtered by the same query: "monthly" is a
    # saved tag and "breakdown" a built-in one.
    expect_ids "the API filters saved tags too" "$TAG_ID" \
        "/api/specs?tag=monthly"
    expect_ids "q finds the saved spec by name" "$TAG_ID" \
        "/api/specs?q=tagged"

    code=$(curl -sS -o "$TMP/body" -w '%{http_code}' -X DELETE \
        "${URL%/}/api/specs/$TAG_ID")
    if [ "$code" = "200" ]; then
        ok "the tagged spec can be deleted again"
    else
        bad "deleting the tagged spec answered $code"
    fi
else
    bad "saving a tagged spec answered $code: $(head -c 300 "$TMP/body")"
fi

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
