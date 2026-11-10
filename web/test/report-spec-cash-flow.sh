#!/bin/bash
# Cash-flow specs: a hand-written spec against the native Cash Flow statement.
#
# Usage: report-spec-cash-flow.sh <build-dir> <source-dir> <book> [start] [end]
#
# <book> is a book with activity in a cash-flow account: the default fixture is
# doc/examples/taxreport.gnucash, whose 2000-2001 activity gives the statement
# 56 rows (two section headings, 51 contributing accounts and the Money In,
# Money Out and Difference totals).  The period defaults to that fixture's own.
#
# Starts gnc-webd against the book, then renders the native Cash Flow statement
# and a hand-written cash-flow spec through /api/spec/preview and compares the
# two documents row for row:
#
#   * every body row's label and its number cells, in order -- the sections'
#     account rows, the two totals and the Difference -- so a spec that reused
#     a different classification, a different account order or a different
#     conversion would show up as a differing row;
#   * the row count, because a page that loses rows on both sides at once
#     would otherwise still "match";
#   * a scoped run: the statement's Account option is the spec's 'accounts',
#     so both must narrow to the same rows.
#
# The spec's figures come from cash_flow_flows(), the same classification the
# statement and the Cash Flow Barchart plot; the harness proves it by comparing
# against the statement's own cells, not by reading the producer.
#
# The fields the report does not have are checked as well: each one, posted on
# an otherwise valid cash-flow spec, must be a 400 whose body names the field.
# The barchart is checked to have no starting spec, with a note saying why
# (its axis is a date-interval series, not the statement's account rows).
#
# /api/spec/options is checked to advertise the source, its fixed columns, its
# four options and a note that says where the figures come from.
#
# Exit status: 0 when every check passes, 1 otherwise.
set -u

USAGE="usage: report-spec-cash-flow.sh <build> <source> <book> [start] [end]"
[ $# -ge 3 ] || { echo "$USAGE" >&2; exit 1; }
BUILD=$1
SRC=$2
BOOK=$3
START=${4:-2000-01-01}
END=${5:-2001-12-31}

EMPTY_BOOK=$SRC/libgnucash/backend/xml/test/test-files/load-save/sample1.gnucash
# A book whose cash-flow accounts have no activity in the default period: the
# statement's default page is its two sections and three totals, five rows.
OTHER_BOOK=$EMPTY_BOOK

fail() { echo "FAIL: $*" >&2; exit 1; }

[ -x "$BUILD/bin/gnc-webd" ] || fail "gnc-webd was not built"
[ -f "$BOOK" ] || fail "fixture book not found at $BOOK"

BACKEND="$BUILD/lib/gnucash/libgncmod-backend-xml.so"
SCHEMAS="$BUILD/share/glib-2.0/schemas"
CCACHE=$(ls -d "$BUILD"/lib/*/guile/3.0/site-ccache 2>/dev/null | head -1)
[ -f "$SCHEMAS/gschemas.compiled" ] || SCHEMAS=""

TMP=$(mktemp -d)
SERVER_PID=
cleanup() {
    [ -n "$SERVER_PID" ] && kill "$SERVER_PID" 2>/dev/null
    rm -rf "$TMP"
}
trap cleanup EXIT

BACKEND_ARGS=()
[ -f "$BACKEND" ] && BACKEND_ARGS=(--backend "$BACKEND")

start_server() { # book
    local log="$TMP/server.log"
    : > "$log"
    env GNC_UNINSTALLED=YES GNC_BUILDDIR="$BUILD" \
        ${SCHEMAS:+GSETTINGS_SCHEMA_DIR="$SCHEMAS"} \
        LD_LIBRARY_PATH="$BUILD/lib:$BUILD/lib/gnucash" \
        GNC_MODULE_PATH="$BUILD/lib/gnucash" \
        GUILE_LOAD_PATH="$BUILD/share/guile/site/3.0" \
        GUILE_LOAD_COMPILED_PATH="${CCACHE:-}" \
        GNC_WEBUI_DIR="$SRC/web/ui" \
        GNC_WEBUI_REPORTS="$TMP/reports.json" \
        "$BUILD/bin/gnc-webd" "${BACKEND_ARGS[@]}" "$1" > "$log" 2>&1 &
    SERVER_PID=$!
    URL=""
    for _ in $(seq 1 300); do
        URL=$(grep -oE 'http://127\.0\.0\.1:[0-9]+/[0-9a-f]+/' "$log" | head -1)
        [ -n "$URL" ] && break
        kill -0 "$SERVER_PID" 2>/dev/null || break
        sleep 0.2
    done
    [ -n "$URL" ] || { cat "$log" >&2; fail "gnc-webd did not start"; }
}

stop_server() {
    [ -n "$SERVER_PID" ] && kill "$SERVER_PID" 2>/dev/null
    wait "$SERVER_PID" 2>/dev/null
    SERVER_PID=
    URL=
}

FAILS=0
ok() { echo "ok   $*"; }
bad() { echo "FAIL $*"; FAILS=$((FAILS + 1)); }

fetch() { # slug path
    local code
    code=$(curl -sS -o "$TMP/$1.html" -w '%{http_code}' "${URL%/}$2") || {
        bad "curl $2 failed"
        return 1
    }
    if [ "$code" != "200" ]; then
        bad "$2 answered $code: $(head -c 200 "$TMP/$1.html")"
        return 1
    fi
}

preview() { # slug json-file
    local code
    code=$(curl -sS -o "$TMP/$1.html" -w '%{http_code}' -X POST \
        -H 'Content-Type: application/json' \
        --data-binary @"$2" "${URL%/}/api/spec/preview") || {
        bad "preview $1 failed"
        return 1
    }
    if [ "$code" != "200" ]; then
        bad "preview $1 answered $code: $(head -c 300 "$TMP/$1.html")"
        return 1
    fi
    return 0
}

# Compare two documents row for row: the label and the number cells of every
# body row, as report-parity.sh's cash-flow extractor reads them.  A row's
# number cells are the cells whose class is number-cell or total-number-cell
# and that print something, so a heading's empty cell never counts as a zero.
compare() { # description native-html spec-html
    python3 - "$1" "$2" "$3" <<'PY'
import re, sys
from decimal import Decimal
from html.parser import HTMLParser

def amount(text):
    text = text.replace("\u00a0", " ").replace("\u2066", "").replace("\u2069", "")
    negative = text.strip().startswith("(") and text.strip().endswith(")")
    text = re.sub(r"[^0-9.,\-]", "", text)
    if text in ("", "-", "."):
        return Decimal(0)
    value = Decimal(text.replace(",", ""))
    return -value if negative else value

class Page(HTMLParser):
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.rows, self.row, self.cell = [], None, None
        self.cls, self.head = "", False

    def handle_starttag(self, tag, attrs):
        if tag == "tr":
            self.row = []
        elif tag in ("td", "th") and self.row is not None:
            self.cell, self.cls, self.head = [], (dict(attrs).get("class") or ""), tag == "th"

    def handle_endtag(self, tag):
        if tag in ("td", "th") and self.cell is not None and self.row is not None:
            text = " ".join("".join(self.cell).split())
            if not self.head:
                self.row.append((text, self.cls))
            self.cell = None
        elif tag == "tr" and self.row is not None:
            if self.row:
                self.rows.append(self.row)
            self.row = None

    def handle_data(self, data):
        if self.cell is not None:
            self.cell.append(data)

def canonical(path):
    page = Page()
    with open(path, encoding="utf-8", errors="replace") as handle:
        page.feed(handle.read())
    rows = []
    for row in page.rows:
        if not row:
            continue
        label = " ".join(row[0][0].replace("\u00a0", " ").split())
        if not label:
            continue
        numbers = tuple(amount(t) for t, c in row[1:]
                        if ("number-cell" in c.split() or
                            "total-number-cell" in c.split()) and t.strip())
        rows.append((label, numbers))
    return rows

left, right = canonical(sys.argv[2]), canonical(sys.argv[3])
if left != right:
    print(f"{sys.argv[1]}: DIFFER: native {len(left)} rows, spec {len(right)}")
    shown = 0
    for index in range(max(len(left), len(right))):
        a = left[index] if index < len(left) else None
        b = right[index] if index < len(right) else None
        if a == b:
            continue
        shown += 1
        if shown <= 12:
            print(f"  row {index}: native={a!r} spec={b!r}")
    print(f"  {shown} differing row(s)")
    raise SystemExit(1)
print(f"{sys.argv[1]}: {len(left)} rows match")
PY
    if [ $? -eq 0 ]; then ok "$1"; else bad "$1"; fi
}

# A spec that sets a field the report does not have must be refused, naming it.
expect_400() { # description json-file substring
    local code
    code=$(curl -sS -o "$TMP/refused.html" -w '%{http_code}' -X POST \
        -H 'Content-Type: application/json' \
        --data-binary @"$2" "${URL%/}/api/spec/preview")
    if [ "$code" != "400" ]; then
        bad "$1: expected 400, got $code: $(head -c 200 "$TMP/refused.html")"
    elif ! grep -q -- "$3" "$TMP/refused.html"; then
        bad "$1: 400 does not say '$3': $(head -c 300 "$TMP/refused.html")"
    else
        ok "400 names it ($1)"
    fi
}

expect_400_body() { # description json-body substring
    printf '%s' "$2" > "$TMP/refused-body.json"
    expect_400 "$1" "$TMP/refused-body.json" "$3"
}

row_count() { # html expected description
    local count
    count=$(python3 - "$1" <<'PY'
import re, sys
raw = open(sys.argv[1], encoding="utf-8", errors="replace").read()
start, end = raw.find("<tbody>"), raw.find("</tbody>")
body = raw[start:end] if start >= 0 and end > start else raw
print(len(re.findall(r"<tr[ >]", body)))
PY
)
    if [ "$count" -ne "$2" ]; then
        bad "$3: $count row(s), expected $2"
    else
        ok "$3: $count rows"
    fi
}

start_server "$BOOK"
echo "server: $URL"
echo "book:   $BOOK"
echo "period: $START to $END"

python3 - "$TMP" "$START" "$END" <<'PY'
import json, os, sys
out, start, end = sys.argv[1:4]
specs = {
    "statement": {"name": "Cash Flow as a spec",
                  "title": "Cash Flow",
                  "source": "cash-flow",
                  "columns": ["account", "money-in", "money-out", "net"],
                  "start": start,
                  "end": end},
    "default": {"name": "Cash Flow as a spec", "title": "Cash Flow",
                "source": "cash-flow"},
}
for name, spec in specs.items():
    with open(os.path.join(out, name + ".json"), "w", encoding="utf-8") as h:
        json.dump(spec, h)
PY

echo "== the statement's rows, from the spec"
fetch statement-native "/report/Cash%20Flow?start=$START&end=$END"
preview statement-spec "$TMP/statement.json"
[ -f "$TMP/statement-native.html" ] && compare "the statement" \
    "$TMP/statement-native.html" "$TMP/statement-spec.html"
[ -f "$TMP/statement-native.html" ] && row_count "$TMP/statement-native.html" 56 \
    "the statement row count"
[ -f "$TMP/statement-spec.html" ] && row_count "$TMP/statement-spec.html" 56 \
    "the spec row count"

echo "== a saved report, not only a preview"
SAVED=$(curl -sS -X POST -H 'Content-Type: application/json' \
    --data-binary @"$TMP/statement.json" "${URL%/}/api/specs" | python3 -c '
import json, sys
print(json.load(sys.stdin)["id"])
')
if [ -n "$SAVED" ] && curl -sS -o "$TMP/saved.html" -w '%{http_code}' \
        "${URL%/}/report/spec/$SAVED" | grep -q 200; then
    compare "saved cash-flow report" "$TMP/statement-native.html" "$TMP/saved.html"
else
    bad "the saved report did not render at /report/spec/$SAVED"
fi

echo "== the report's Account option is the spec's 'accounts'"
GUID=$(curl -sS "${URL%/}/api/accounts" | python3 -c '
import json, sys
tree = json.load(sys.stdin)
stack = [tree]
while stack:
    node = stack.pop()
    stack.extend(node.get("children") or [])
    if node.get("name") == "Checking One":
        print(node.get("guid") or "")
        break
')
if [ -n "$GUID" ]; then
    python3 - "$TMP" "$GUID" "$START" "$END" <<'PY'
import json, os, sys
out, guid, start, end = sys.argv[1:5]
spec = {"name": "Cash Flow, one account", "title": "Cash Flow",
        "source": "cash-flow",
        "columns": ["account", "money-in", "money-out", "net"],
        "start": start, "end": end, "accounts": [guid]}
with open(os.path.join(out, "scoped.json"), "w", encoding="utf-8") as h:
    json.dump(spec, h)
PY
    fetch scoped-native "/report/Cash%20Flow?start=$START&end=$END&accounts=$GUID"
    preview scoped-spec "$TMP/scoped.json"
    [ -f "$TMP/scoped-native.html" ] && compare "one account" \
        "$TMP/scoped-native.html" "$TMP/scoped-spec.html"
else
    bad "no account named 'Checking One' in /api/accounts"
fi

echo "== /api/spec/options advertises the source"
curl -sS "${URL%/}/api/spec/options" > "$TMP/options.json"
python3 - "$TMP/options.json" <<'PY'
import json, sys
with open(sys.argv[1], encoding="utf-8") as handle:
    options = json.load(handle)
failures = 0
def check(condition, message):
    global failures
    if condition:
        print("ok   " + message)
    else:
        failures += 1
        print("FAIL " + message)
sources = {entry["name"]: entry for entry in options.get("sources") or []}
check("cash-flow" in sources, "the source list names 'cash-flow'")
check("Cash flow" in (sources.get("cash-flow") or {}).get("label", ""),
      "its label names the report")
cash = options.get("cash-flow") or {}
check(cash.get("columns") == ["account", "money-in", "money-out", "net"],
      "it publishes the account column and the three figures")
check(cash.get("options") == ["start", "end", "accounts", "currency"],
      "it publishes the report's four options")
note = cash.get("note") or ""
check("cash_flow_flows" in note,
      "its note says the figures are the statement's own classification")
check("barchart" in note.lower(), "its note says the barchart is not this source")
raise SystemExit(1 if failures else 0)
PY
[ $? -eq 0 ] || FAILS=$((FAILS + 1))

echo "== the designer's starting point is the statement"
curl -sS "${URL%/}/api/report/Cash%20Flow/designer" > "$TMP/designer.json"
curl -sS "${URL%/}/report/Cash%20Flow" > "$TMP/designer-native.html"
python3 - "$TMP" <<'PY'
import json, os, sys
tmp = sys.argv[1]
failures = 0
def check(condition, message):
    global failures
    if condition:
        print("ok   " + message)
    else:
        failures += 1
        print("FAIL " + message)
with open(os.path.join(tmp, "designer.json"), encoding="utf-8") as handle:
    info = json.load(handle)
spec = info.get("spec") or {}
check(info.get("equivalence") == "exact",
      "the designer says the starting spec is exact")
check(spec.get("source") == "cash-flow", "its source is 'cash-flow'")
check(spec.get("columns") == ["account", "money-in", "money-out", "net"],
      "its columns are the statement's figures")
check(bool(spec.get("start")) and bool(spec.get("end")),
      "it carries the report's two dates")
note = info.get("note") or ""
check("cash_flow_flows" in note, "its note says where the figures come from")
check("Selected Accounts" in note,
      "its note says the Selected Accounts list is not a cell")
spec["name"] = spec["title"] = "Cash Flow (copy)"
with open(os.path.join(tmp, "designer-spec.json"), "w", encoding="utf-8") as h:
    json.dump(spec, h)
raise SystemExit(1 if failures else 0)
PY
[ $? -eq 0 ] || FAILS=$((FAILS + 1))
preview designer "$TMP/designer-spec.json"
[ -f "$TMP/designer-native.html" ] && compare "the statement's default page" \
    "$TMP/designer-native.html" "$TMP/designer.html"

echo "== the barchart has no spec"
curl -sS "${URL%/}/api/report/Cash%20Flow%20Barchart/designer" > "$TMP/barchart.json"
python3 - "$TMP/barchart.json" <<'PY'
import json, sys
with open(sys.argv[1], encoding="utf-8") as handle:
    info = json.load(handle)
failures = 0
def check(condition, message):
    global failures
    if condition:
        print("ok   " + message)
    else:
        failures += 1
        print("FAIL " + message)
check(info.get("equivalence") == "none",
      "the barchart has no equivalent spec")
check(not info.get("spec"), "the designer offers no starting spec")
note = info.get("note") or ""
check("interval" in note.lower(),
      "its note says its axis is a date-interval series")
raise SystemExit(1 if failures else 0)
PY
[ $? -eq 0 ] || FAILS=$((FAILS + 1))

python3 - "$TMP" <<'PY'
import json, os, sys
out = sys.argv[1]
base = {"name": "Cash Flow as a spec", "source": "cash-flow",
        "columns": ["account", "money-in", "money-out", "net"]}
variants = {
    "grouping": {"grouping": "period"},
    "period-unit": {"period_unit": "month"},
    "period-mode": {"period_mode": "cumulative"},
    "account-types": {"account_types": ["BANK"]},
    "chart": {"chart": "bar"},
    "chart-series": {"chart_series": ["balance"]},
    "split-grouping": {"split_grouping": "account"},
    "description-filter": {"description_filter": "Rent"},
    "invoice-owner": {"invoice_owner": "payable"},
    "invoice-grouping": {"invoice_grouping": "document"},
    "bucket-basis": {"bucket_basis": "posted"},
    "date-preset": {"date_preset": "this-year"},
    "document": {"document": "Printable Invoice"},
    "invoice": {"invoice": "INV-1001"},
    "document-options": {"document_options": {"show_date": "false"}},
}
for name, extra in variants.items():
    spec = dict(base)
    spec.update(extra)
    with open(os.path.join(out, "bad-" + name + ".json"), "w",
              encoding="utf-8") as h:
        json.dump(spec, h)
PY

echo "== the fields the report does not have"
for pair in \
    "grouping:'grouping'" "period-unit:'period_unit'" \
    "period-mode:'period_mode'" "account-types:'account_types'" \
    "chart:'chart'" "chart-series:'chart_series'" \
    "split-grouping:'split_grouping'" \
    "description-filter:'description_filter'" \
    "invoice-owner:'invoice_owner'" "invoice-grouping:'invoice_grouping'" \
    "bucket-basis:'bucket_basis'" "date-preset:'date_preset'" \
    "document:'document'" "invoice:'invoice'" \
    "document-options:'document_options'"
do
    slug=${pair%%:*}
    field=${pair#*:}
    expect_400 "$slug" "$TMP/bad-$slug.json" "$field"
done
# The column list is the statement's: it cannot be shortened or reordered.
expect_400_body "columns-short" \
    '{"name":"x","source":"cash-flow","columns":["account","net"]}' \
    "'columns'"
expect_400_body "columns-wrong-order" \
    '{"name":"x","source":"cash-flow","columns":["net","account","money-in","money-out"]}' \
    "'columns'"
expect_400_body "columns-extra" \
    '{"name":"x","source":"cash-flow","columns":["account","money-in","money-out","net","total"]}' \
    "'columns'"

stop_server

echo "== a book with no cash-flow activity: the same five rows"
if [ -f "$OTHER_BOOK" ]; then
    start_server "$OTHER_BOOK"
    echo "-- $(basename "$OTHER_BOOK")"
    fetch other-native "/report/Cash%20Flow"
    preview other-spec "$TMP/default.json"
    if [ -f "$TMP/other-native.html" ]; then
        compare "no contributions" "$TMP/other-native.html" \
            "$TMP/other-spec.html"
        row_count "$TMP/other-native.html" 5 "no-contribution row count"
        row_count "$TMP/other-spec.html" 5 "no-contribution spec row count"
    fi
    stop_server
else
    bad "second book not found at $OTHER_BOOK"
fi

if [ "$FAILS" -ne 0 ]; then
    echo "$FAILS check(s) failed"
    exit 1
fi
echo "cash-flow specs match the native Cash Flow statement"
