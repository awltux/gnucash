#!/bin/bash
# Budget specs: a hand-written spec against the native Budget Report.
#
# Usage: report-spec-budgets.sh <build-dir> <source-dir> <book> [account[=rows]...]
#
# <book> is a book with a budget: web/test/make-budget-book.py writes one (a
# three-month budget, January to March 2026, four top-level accounts, one
# closing entry and one transaction outside the budget).  The default accounts
# and row counts below are that fixture's; a book that names its accounts
# differently passes them, and "Name=Rows" pins the page's row count as well,
# which is what makes the gate catch a page that loses rows on both sides at
# once.
#
# Starts gnc-webd against the book, then renders the native Budget Report and
# a hand-written budgets spec through /api/spec/preview and compares the two
# documents cell for cell:
#
#   * the rows, in order: the account label and the budget/actual numbers for
#     every period of the book's budget;
#   * the headings, which are the proof that the spec's column axis is the
#     budget's own periods and not a range invented from dates ("Bgt
#     01/01/26", "Act 01/01/26", ...);
#   * the amounts as numbers, so the report's "." for an unset budget and the
#     spec's zero are the same cell, and the currency format does not count as
#     a diff;
#   * the explanation paragraph on a book with no budget, because that is the
#     page the report prints there.
#
# The default page is the whole chart of accounts, and each named account is
# the report's Account option: the native route expands a GUID to its subtree,
# and the spec must select the same accounts.  Expenses is the interesting
# scope because its third level (Food: Groceries) is below the report's display
# depth and still feeds Food's rolled-up budget and actual.
#
# The empty state is checked on sample1, which has no budget at all: the
# report's page is the "No budgets exist..." explanation and no table, and the
# spec's must be the same page.  The default spec is the designer's starting
# point, which is compared against the report's own default page.
#
# The fields the report does not have are checked as well: each one, posted on
# an otherwise valid budgets spec, must be a 400 whose body names the field.
# So are two column lists (one pair short, and another order), a column the
# report does not print, a budget selection (the native route always uses the
# book's default budget, so a spec has no 'budget' field), and the four budget
# reports this source does not express: their designer notes must name what
# the report would need instead.
#
# /api/spec/options is checked to advertise the source, its fixed columns, its
# only option and a note that says the periods are the budget's.
#
# Exit status: 0 when every check passes, 1 otherwise.
set -u

USAGE="usage: report-spec-budgets.sh <build> <source> <book> [account[=rows]...]"
[ $# -ge 3 ] || { echo "$USAGE" >&2; exit 1; }
BUILD=$1
SRC=$2
BOOK=$3
shift 3

EMPTY_BOOK=$SRC/libgnucash/backend/xml/test/test-files/load-save/sample1.gnucash

# The account scopes to compare, with the fixture's own row counts.  The whole
# book is compared as well, and it is the report's default page.
ACCOUNTS=("$@")
if [ ${#ACCOUNTS[@]} -eq 0 ]; then
    ACCOUNTS=("Expenses=3" "Income=3")
fi

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
        bad "$2 answered $code"
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
}

# Compare two documents cell for cell.  The extractor is report-parity.sh's
# budget_report(): every body row becomes its label and its number cells, a
# cell with the number-cell class only (so the report's "." is a zero amount
# and a text cell never passes for an amount).  The heading row is compared as
# a row of headings, which is where the budget's period structure shows.
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

class Budget(HTMLParser):
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.rows, self.row, self.cell = [], None, None
        self.cls, self.head = "", False
        self.headings, self.paragraphs = [], []
        self.para = None

    def handle_starttag(self, tag, attrs):
        if tag == "tr":
            self.row = []
        elif tag in ("td", "th") and self.row is not None:
            self.cell, self.cls, self.head = [], (dict(attrs).get("class") or ""), tag == "th"
        elif tag == "p":
            self.cell, self.para = [], []

    def handle_endtag(self, tag):
        if tag in ("td", "th") and self.cell is not None and self.row is not None:
            text = " ".join("".join(self.cell).split())
            if self.head:
                self.headings.append(text)
            else:
                self.row.append((text, self.cls))
            self.cell = None
        elif tag == "tr" and self.row is not None:
            if self.row:
                self.rows.append(self.row)
            self.row = None
        elif tag == "p" and self.para is not None:
            self.paragraphs.append(" ".join("".join(self.para).split()))
            self.para = None

    def handle_data(self, data):
        if self.cell is not None:
            self.cell.append(data)

def canonical(path):
    page = Budget()
    with open(path, encoding="utf-8", errors="replace") as handle:
        page.feed(handle.read())
    rows = []
    for row in page.rows:
        if not row:
            continue
        label = " ".join(row[0][0].replace("\u00a0", " ").split())
        if not label:
            continue
        numbers = tuple(
            amount(text) for text, cls in row[1:]
            if ("number-cell" in cls.split() or "total-number-cell" in cls.split())
            and text.strip())
        rows.append((label, numbers))
    return rows, [h for h in page.headings if h], [p for p in page.paragraphs if p]

description, native_path, spec_path = sys.argv[1:4]
native_rows, native_headings, native_paragraphs = canonical(native_path)
spec_rows, spec_headings, spec_paragraphs = canonical(spec_path)

failed = False
if native_rows != spec_rows:
    failed = True
    print(f"{description}: DIFFER: native {len(native_rows)} rows, spec {len(spec_rows)}")
    differing = 0
    for index in range(max(len(native_rows), len(spec_rows))):
        left = native_rows[index] if index < len(native_rows) else None
        right = spec_rows[index] if index < len(spec_rows) else None
        if left == right:
            continue
        differing += 1
        if differing <= 12:
            print(f"  row {index}: native={left!r} spec={right!r}")
    print(f"  {differing} differing row(s)")

if native_headings != spec_headings:
    failed = True
    print(f"  headings: native={native_headings!r} spec={spec_headings!r}")
elif native_headings:
    print(f"  headings: {len(native_headings)} match (the budget's periods)")

if native_paragraphs != spec_paragraphs:
    failed = True
    print(f"  explanation: native={native_paragraphs!r} spec={spec_paragraphs!r}")
elif native_paragraphs:
    print(f"  explanation: {native_paragraphs[0]!r}")

if failed:
    raise SystemExit(1)
print(f"{description}: {len(native_rows)} rows match")
PY
    if [ $? -eq 0 ]; then
        ok "$1"
    else
        bad "$1"
    fi
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

# The page's shape: the explanation and no table row, or a table.
page_shape() { # html expected(empty|table) description
    python3 - "$1" "$2" "$3" <<'PY'
import sys
from html.parser import HTMLParser

class Page(HTMLParser):
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.tds, self.paras, self.cell = 0, [], None
    def handle_starttag(self, tag, attrs):
        if tag == "td":
            self.tds += 1
        elif tag == "p":
            self.cell = []
    def handle_endtag(self, tag):
        if tag == "p" and self.cell is not None:
            self.paras.append(" ".join("".join(self.cell).split()))
            self.cell = None
    def handle_data(self, data):
        if self.cell is not None:
            self.cell.append(data)

path, expected, description = sys.argv[1:4]
page = Page()
with open(path, encoding="utf-8", errors="replace") as handle:
    page.feed(handle.read())
if expected == "empty":
    if page.tds:
        print(f"{description}: FAIL page has {page.tds} cells, expected the explanation only")
        raise SystemExit(1)
    if not page.paras:
        print(f"{description}: FAIL page has no explanation paragraph")
        raise SystemExit(1)
    print(f"{description}: empty page, explanation {page.paras[0]!r}")
else:
    if not page.tds:
        print(f"{description}: FAIL page has no table rows")
        raise SystemExit(1)
    print(f"{description}: {page.tds} cells")
raise SystemExit(0)
PY
    if [ $? -eq 0 ]; then ok "$3"; else bad "$3"; fi
}

# The page's body-row count, so a page that loses rows on both sides at once is
# still a failure.
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
echo "scopes: ${ACCOUNTS[*]}"

# The whole book is the report's default page and the spec's default page.
python3 - "$TMP" <<'PY'
import json, os, sys
out = sys.argv[1]
specs = {
    "page": {"name": "Budget Report as a spec",
             "title": "Budget Report as a spec",
             "source": "budgets",
             "columns": ["account", "budget", "actual"]},
    "default": {"name": "Budget Report as a spec",
                "title": "Budget Report as a spec",
                "source": "budgets"},
}
for name, spec in specs.items():
    with open(os.path.join(out, name + ".json"), "w", encoding="utf-8") as h:
        json.dump(spec, h)
PY

echo "== the whole book: the report's default page"
fetch whole-native "/report/Budget%20Report"
preview whole-spec "$TMP/page.json"
[ -f "$TMP/whole-native.html" ] && compare "the whole book" \
    "$TMP/whole-native.html" "$TMP/whole-spec.html"
[ -f "$TMP/whole-native.html" ] && row_count "$TMP/whole-native.html" 11 \
    "the whole book row count"

echo "== each named account: the report's Account option"
GUIDS=()
for entry in "${ACCOUNTS[@]}"; do
    name=${entry%%=*}
    guid=$(curl -sS "${URL%/}/api/accounts" | python3 -c '
import json, sys
tree = json.load(sys.stdin)
want = sys.argv[1]
stack = [tree]
while stack:
    node = stack.pop()
    stack.extend(node.get("children") or [])
    if node.get("name") == want:
        print(node.get("guid") or "")
        break
' "$name")
    if [ -z "$guid" ]; then
        bad "no account named '$name' in /api/accounts (pass the book's account names)"
    fi
    GUIDS+=("$guid")
done

index=0
for entry in "${ACCOUNTS[@]}"; do
    name=${entry%%=*}
    expected=
    if [ "$entry" != "$name" ]; then expected=${entry##*=}; fi
    guid=${GUIDS[$index]}
    index=$((index + 1))
    [ -n "$guid" ] || continue
    echo "-- $name ($guid)"
    python3 - "$TMP" "$guid" "$name" <<'PY'
import json, os, sys
out, guid, name = sys.argv[1:4]
spec = {"name": "Budget Report, " + name,
        "title": "Budget Report, " + name,
        "source": "budgets",
        "columns": ["account", "budget", "actual"],
        "accounts": [guid]}
with open(os.path.join(out, "scoped.json"), "w", encoding="utf-8") as h:
    json.dump(spec, h)
PY
    slug=$(echo "$name" | tr 'A-Z' 'a-z')
    fetch "$slug-native" "/report/Budget%20Report?accounts=$guid"
    preview "$slug-spec" "$TMP/scoped.json"
    if [ -f "$TMP/$slug-native.html" ]; then
        compare "$name" "$TMP/$slug-native.html" "$TMP/$slug-spec.html"
        if [[ "$expected" =~ ^[0-9]+$ ]]; then
            row_count "$TMP/$slug-native.html" "$expected" "$name row count"
            row_count "$TMP/$slug-spec.html" "$expected" "$name spec row count"
        fi
    fi
done

echo "== a saved report, not only a preview"
SAVED=$(curl -sS -X POST -H 'Content-Type: application/json' \
    --data-binary @"$TMP/page.json" "${URL%/}/api/specs" | python3 -c '
import json, sys
print(json.load(sys.stdin)["id"])
')
if [ -n "$SAVED" ] && curl -sS -o "$TMP/saved.html" -w '%{http_code}' \
        "${URL%/}/report/spec/$SAVED" | grep -q 200; then
    compare "saved budgets report" "$TMP/whole-native.html" "$TMP/saved.html"
else
    bad "the saved report did not render at /report/spec/$SAVED"
fi
curl -sS "${URL%/}/api/specs/$SAVED" > "$TMP/saved.json"
python3 - "$TMP/saved.json" <<'PY'
import json, sys
with open(sys.argv[1], encoding="utf-8") as handle:
    spec = json.load(handle)
failures = 0
for field, expected in (("source", "budgets"),
                        ("columns", ["account", "budget", "actual"])):
    if spec.get(field) == expected:
        print(f"ok   the store keeps {field}={expected!r}")
    else:
        failures += 1
        print(f"FAIL the store has {field}={spec.get(field)!r}, expected {expected!r}")
raise SystemExit(1 if failures else 0)
PY
[ $? -eq 0 ] || FAILS=$((FAILS + 1))

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
check("budgets" in sources, "the source list names 'budgets'")
check("Budget" in (sources.get("budgets") or {}).get("label", ""),
      "its label names the report")
budgets = options.get("budgets") or {}
check(budgets.get("columns") == ["account", "budget", "actual"],
      "it publishes the account column and the Bgt/Act pair")
check(budgets.get("options") == ["accounts"],
      "it publishes the source's only option")
note = budgets.get("note") or ""
check("period" in note and "budget" in note,
      "its note says the column axis is the budget's periods")
check("default budget" in note,
      "its note says the report uses the book's default budget")
check("No budgets exist" in note,
      "its note says what a book with no budget renders")
raise SystemExit(1 if failures else 0)
PY
[ $? -eq 0 ] || FAILS=$((FAILS + 1))

echo "== the designer's starting point is the report"
curl -sS "${URL%/}/api/report/Budget%20Report/designer" > "$TMP/designer.json"
curl -sS "${URL%/}/report/Budget%20Report" > "$TMP/designer-native.html"
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
check(spec.get("source") == "budgets", "its source is 'budgets'")
check(spec.get("columns") == ["account", "budget", "actual"],
      "its columns are the account column and the Bgt/Act pair")
check(not spec.get("start") and not spec.get("end") and
      not spec.get("date_preset"),
      "it has no date range: the periods are the budget's")
check(not spec.get("currency"),
      "it has no currency: the report prints each account's commodity")
check(not spec.get("accounts"),
      "it has no accounts, which is the report's default selection")
note = info.get("note") or ""
check("default budget" in note,
      "its note says the report always uses the book's default budget")
check("period" in note and "budget" in note,
      "its note explains that the columns are the budget's periods")
check("rolled-up" in note,
      "its note explains the report's rolled-up budget figure")
check("closing" in note,
      "its note says the actuals exclude closing entries")
# The starting spec must render the report's default page.  The copy has its
# own name and title; keep them out of the comparison.
spec["name"] = spec["title"] = "Budget Report (copy)"
with open(os.path.join(tmp, "designer-spec.json"), "w", encoding="utf-8") as h:
    json.dump(spec, h)
raise SystemExit(1 if failures else 0)
PY
[ $? -eq 0 ] || FAILS=$((FAILS + 1))
preview designer "$TMP/designer-spec.json"
[ -f "$TMP/designer-native.html" ] && compare "the report's default page" \
    "$TMP/designer-native.html" "$TMP/designer.html"

echo "== the four budget reports this source does not express"
for report in "Budget Income Statement:statement" \
              "Budget Profit & Loss:statement" \
              "Budget Balance Sheet:initial balance" \
              "Budget Flow:Period"
do
    name=${report%%:*}
    needle=${report#*:}
    encoded=$(python3 -c 'import sys, urllib.parse; print(urllib.parse.quote(sys.argv[1]))' "$name")
    curl -sS "${URL%/}/api/report/$encoded/designer" > "$TMP/none.json"
    python3 - "$TMP/none.json" "$name" "$needle" <<'PY'
import json, sys
path, name, needle = sys.argv[1:4]
with open(path, encoding="utf-8") as handle:
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
      f"{name}: the designer has no exact spec")
check(not info.get("spec"),
      f"{name}: the designer offers no starting spec")
note = info.get("note") or ""
check("budgets" in note,
      f"{name}: its note names the source the report is not")
check(needle.lower() in note.lower(),
      f"{name}: its note names what it would need instead ({needle!r})")
raise SystemExit(1 if failures else 0)
PY
    [ $? -eq 0 ] || FAILS=$((FAILS + 1))
done

# One mutation per field the report does not have.  Each is otherwise a valid
# budgets spec, so a 400 can only be about the field under test.
python3 - "$TMP" <<'PY'
import json, os, sys
out = sys.argv[1]
base = {"name": "Budget Report as a spec", "source": "budgets",
        "columns": ["account", "budget", "actual"]}
variants = {
    "start": {"start": "2026-01-01"},
    "end": {"end": "2026-03-31"},
    "date-preset": {"date_preset": "this-year"},
    "currency": {"currency": "USD"},
    "grouping": {"grouping": "period"},
    "period-unit": {"period_unit": "month"},
    "period-mode": {"period_mode": "cumulative"},
    "account-types": {"account_types": ["EXPENSE"]},
    "chart": {"chart": "bar"},
    "chart-series": {"chart_series": ["balance"]},
    "split-grouping": {"split_grouping": "account"},
    "description-filter": {"description_filter": "Rent"},
    "invoice-owner": {"invoice_owner": "payable"},
    "invoice-grouping": {"invoice_grouping": "document"},
    "bucket-basis": {"bucket_basis": "posted"},
    "budget": {"budget": "Three Month Budget"},
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
    "start:'start'" "end:'end'" "date-preset:'date_preset'" \
    "currency:'currency'" "grouping:'grouping'" \
    "period-unit:'period_unit'" "period-mode:'period_mode'" \
    "account-types:'account_types'" "chart:'chart'" \
    "chart-series:'chart_series'" "split-grouping:'split_grouping'" \
    "description-filter:'description_filter'" \
    "invoice-owner:'invoice_owner'" "invoice-grouping:'invoice_grouping'" \
    "bucket-basis:'bucket_basis'" "budget:'budget'"
do
    slug=${pair%%:*}
    field=${pair#*:}
    expect_400 "$slug" "$TMP/bad-$slug.json" "$field"
done
# The column list is the report's: the pair cannot be dropped or reordered, and
# no fourth column exists.
expect_400_body "columns-one-pair-short" \
    '{"name":"x","source":"budgets","columns":["account","budget"]}' \
    "'columns'"
expect_400_body "columns-wrong-order" \
    '{"name":"x","source":"budgets","columns":["budget","account","actual"]}' \
    "'columns'"
expect_400_body "columns-extra" \
    '{"name":"x","source":"budgets","columns":["account","budget","actual","variance"]}' \
    "'columns'"

stop_server

echo "== the no-budget page, on a book with no budget"
if [ -f "$EMPTY_BOOK" ]; then
    start_server "$EMPTY_BOOK"
    echo "-- $(basename "$EMPTY_BOOK")"
    fetch empty-native "/report/Budget%20Report"
    preview empty-spec "$TMP/default.json"
    if [ -f "$TMP/empty-native.html" ]; then
        compare "the default page on $(basename "$EMPTY_BOOK")" \
            "$TMP/empty-native.html" "$TMP/empty-spec.html"
        page_shape "$TMP/empty-native.html" empty \
            "$(basename "$EMPTY_BOOK") Budget Report is the no-budget page"
        page_shape "$TMP/empty-spec.html" empty \
            "$(basename "$EMPTY_BOOK") budgets spec is the no-budget page"
    fi
    stop_server
else
    bad "empty-state book not found at $EMPTY_BOOK"
fi

if [ "$FAILS" -ne 0 ]; then
    echo "$FAILS check(s) failed"
    exit 1
fi
echo "budgets specs match the native Budget Report"
