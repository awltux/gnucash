#!/bin/bash
# Lots specs: a hand-written spec against the native Lot Viewer.
#
# Usage: report-spec-lots.sh <build-dir> <source-dir> <book> [start] [end]
#                            [account[=rows]...]
#
# <book> is a book with stock lots: web/test/make-investment-book.py --two-accounts
# writes one (four lots on Assets:Investments:Brokerage, one on
# Assets:Investments:Retirement, a closed lot, a price database), and the
# default accounts and start/end dates below are that fixture's.  A book that
# names its lot accounts differently passes them; "Name=Rows" pins the page's
# row count as well, which is what makes the gate catch a page that loses rows
# on both sides at once.
#
# Starts gnc-webd against the book, then for each lot account -- and for the
# whole book, which is the report's page with no Account chosen -- renders the
# native Lot Viewer and a hand-written lots spec through /api/spec/preview and
# compares the two documents cell for cell:
#
#   * the rows, in order: the lot title row, the lot GUID row, the Document row,
#     one row per transaction in the period and the Balance row;
#   * the amounts, as numbers, so the report's ":" for a lot a transaction did
#     not touch and the spec's zero are the same cell, and so the whole-book
#     page's default-monetary format ("$20.00") and the account page's share
#     format ("20. ACME") compare as the numbers they are;
#   * the column headings, when the report prints any (it does not: its heading
#     is a body row, which is compared as rows);
#   * the explanation paragraph, because the report prints one instead of a
#     table when the period holds no split.
#
# The lot columns are the hard part and the reason this harness exists: the
# report writes one column per lot its transactions touch, so the lot columns
# are the book's, not a vocabulary.  The spec's column list names the middle of
# the table with the placeholder "lots", and the producer writes one column per
# lot in the report's order; the comparison therefore also proves the lot
# columns' number, order, titles and GUIDs, because those are cells of the rows.
#
# The Description Filter is checked too: a spec that sets it and the route that
# sets it must print the same page, and the filter must remove rows (otherwise
# the check would pass on a field that is accepted and ignored).
#
# The empty state is checked on sample1, which has no stock account at all: the
# report's default page is the explanation and no table, and the spec's must be
# the same page.  The default page is also the designer's starting point, which
# is compared against the report's own default page.
#
# The fields the report does not have are checked as well: each one, posted on
# an otherwise valid lots spec, must be a 400 whose body names the field.  So
# are two accounts (the report's Account option holds one), a column list that
# is not the report's, and a lots-only field on a spec of another source.
# /api/spec/options is checked to advertise the source, its fixed columns, its
# options and a note that says the lot columns are the book's.
#
# Exit status: 0 when every check passes, 1 otherwise.
set -u

USAGE="usage: report-spec-lots.sh <build> <source> <book> [start] [end] [account[=rows]...]"
[ $# -ge 3 ] || { echo "$USAGE" >&2; exit 1; }
BUILD=$1
SRC=$2
BOOK=$3
START=2026-01-01
END=2026-06-30
if [ $# -gt 3 ]; then START=$4; fi
if [ $# -gt 4 ]; then END=$5; fi
if [ $# -gt 5 ]; then shift 5; else set --; fi

EMPTY_BOOK=$SRC/libgnucash/backend/xml/test/test-files/load-save/sample1.gnucash

# The lot accounts to compare, with the fixture's own row counts.
ACCOUNTS=("$@")
if [ ${#ACCOUNTS[@]} -eq 0 ]; then
    ACCOUNTS=("Brokerage=14" "Retirement=6")
fi

fail() { echo "FAIL: $*" >&2; exit 1; }

[ -x "$BUILD/bin/gnc-webd" ] || fail "gnc-webd was not built"
[ -f "$BOOK" ] || fail "fixture book not found at $BOOK"
case "$START" in
[0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9]) ;;
*) fail "start date must be YYYY-MM-DD, got '$START'" ;;
esac
case "$END" in
[0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9]) ;;
*) fail "end date must be YYYY-MM-DD, got '$END'" ;;
esac

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

# The spec documents, written by python so the dates and GUIDs are
# JSON-escaped, not shell-quoted.  $1 is the account, $2 a second account for
# the two-account refusal (may be empty).
write_specs() {
    python3 - "$TMP" "$START" "$END" "$1" "${2:-}" <<'PY'
import json, os, sys
out, start, end, guid, other = sys.argv[1:6]
scope = {"accounts": [guid]} if guid else {}
specs = {
    "page": dict({"name": "Lot Viewer as a spec",
                  "title": "Lot Viewer as a spec",
                  "source": "lots", "start": start, "end": end}, **scope),
    "filter": dict({"name": "Lot Viewer, sells only",
                    "title": "Lot Viewer, sells only",
                    "source": "lots", "start": start, "end": end,
                    "description_filter": "Sell"}, **scope),
    "default": {"name": "Lot Viewer as a spec",
                "title": "Lot Viewer as a spec", "source": "lots"},
}
for name, spec in specs.items():
    with open(os.path.join(out, name + ".json"), "w", encoding="utf-8") as h:
        json.dump(spec, h)

# One mutation per field the report does not have.  Each is otherwise a valid
# lots spec, so a 400 can only be about the field under test.
base = specs["page"]
variants = {
    "date-preset": {"date_preset": "this-year"},
    "currency": {"currency": "USD"},
    "grouping": {"grouping": "period"},
    "period-unit": {"period_unit": "month"},
    "period-mode": {"period_mode": "cumulative"},
    "account-types": {"account_types": ["STOCK"]},
    "chart": {"chart": "bar"},
    "chart-series": {"chart_series": ["balance"]},
    "split-grouping": {"split_grouping": "account"},
    "invoice-owner": {"invoice_owner": "payable"},
    "invoice-grouping": {"invoice_grouping": "document"},
    "bucket-basis": {"bucket_basis": "posted"},
    "columns-extra": {"columns": ["date", "description", "type", "lots",
                                  "other-account", "balance"]},
    "columns-order": {"columns": ["type", "date", "description", "lots",
                                  "other-account"]},
    "columns-lot-dropped": {"columns": ["date", "description", "type",
                                        "other-account"]},
}
if guid and other and other != guid:
    variants["accounts-two"] = {"accounts": [guid, other]}
for name, extra in variants.items():
    spec = dict(base)
    spec.update(extra)
    with open(os.path.join(out, "bad-" + name + ".json"), "w",
              encoding="utf-8") as h:
        json.dump(spec, h)
PY
}

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

# Compare two documents cell for cell.  The extractor is the one
# report-spec-aging.sh uses -- every body row becomes the sequence of its
# non-empty cells, an amount cell as a number and every other cell as text, plus
# the explanation paragraphs -- because the Lot Viewer prints one instead of a
# table when the period holds no split.  That is the report's claim: the same
# rows, in the same order, their lot columns, their values and their
# explanation, independent of each renderer's cell classes and blank padding.
compare() { # description native-html spec-html
    python3 - "$1" "$2" "$3" <<'PY'
import re, sys
from decimal import Decimal
from html.parser import HTMLParser

def amount(text):
    text = text.replace("\u00a0", " ").replace("\u2066", "")
    text = text.replace("\u2069", "")
    negative = text.strip().startswith("(") and text.strip().endswith(")")
    text = re.sub(r"[^0-9.,\-]", "", text)
    if text in ("", "-", "."):
        return Decimal(0)
    value = Decimal(text.replace(",", ""))
    return -value if negative else value

class Cells(HTMLParser):
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.rows, self.row, self.cell = [], None, None
        self.cls, self.head, self.para = "", False, False
        self.headings, self.paragraphs = [], []

    def handle_starttag(self, tag, attrs):
        if tag == "tr":
            self.row = []
        elif tag in ("td", "th") and self.row is not None:
            self.cell, self.cls, self.head = [], attrs and dict(attrs).get("class", "") or "", tag == "th"
        elif tag == "p":
            self.cell, self.cls, self.head, self.para = [], "", False, True

    def handle_endtag(self, tag):
        if tag in ("td", "th") and self.cell is not None and self.row is not None:
            text = " ".join("".join(self.cell).split())
            if self.head:
                self.headings.append(text)
            else:
                self.row.append((text, self.cls))
            self.cell = None
        elif tag == "p" and self.cell is not None:
            self.paragraphs.append(" ".join("".join(self.cell).split()))
            self.cell, self.para = None, False
        elif tag == "tr" and self.row is not None:
            if self.row:
                self.rows.append(self.row)
            self.row = None

    def handle_data(self, data):
        if self.cell is not None:
            self.cell.append(data)

def canonical(path):
    parser = Cells()
    with open(path, encoding="utf-8", errors="replace") as handle:
        parser.feed(handle.read())
    rows = []
    for row in parser.rows:
        cells = []
        for text, cls in row:
            if not text.strip():
                continue
            tokens = cls.split()
            if "number-cell" in tokens or "total-number-cell" in tokens:
                cells.append(("N", amount(text)))
            else:
                cells.append(("T", text))
        if cells:
            rows.append(tuple(cells))
    return (rows, [h for h in parser.headings if h],
            [p for p in parser.paragraphs if p])

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

if native_headings and native_headings != spec_headings:
    failed = True
    print(f"  headings: native={native_headings!r} spec={spec_headings!r}")
elif native_headings:
    print(f"  headings: {len(native_headings)} match")

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
# still a failure.  The report's pages have one <tr> per row.
row_count() { # html expected description
    local count
    count=$(grep -o '<tr>' "$1" | wc -l)
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
echo "lots:   ${ACCOUNTS[*]}"

python3 - "$TMP" "$START" "$END" <<'PY'
import json, os, sys
out, start, end = sys.argv[1:4]
base = {"name": "Lot Viewer with no account",
        "title": "Lot Viewer with no account",
        "source": "lots", "start": start, "end": end}
with open(os.path.join(out, "whole-book.json"), "w", encoding="utf-8") as h:
    json.dump(base, h)
# A lots-only field on a spec of another source is refused, not ignored.
with open(os.path.join(out, "bad-filter-on-accounts.json"), "w",
          encoding="utf-8") as h:
    json.dump({"name": "accounts with a lots field", "source": "accounts",
               "columns": ["account-name", "balance"], "date_preset": "all",
               "description_filter": "Sell"}, h)
PY

echo "== each lot account: the report's page"
# Resolve every account first: the two-account refusal needs two real GUIDs.
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
        bad "no account named '$name' in /api/accounts (pass the book's lot account names)"
    fi
    GUIDS+=("$guid")
done
if [ ${#GUIDS[@]} -gt 1 ]; then
    SECOND=${GUIDS[1]}
else
    SECOND=
fi

FIRST=
index=0
for entry in "${ACCOUNTS[@]}"; do
    name=${entry%%=*}
    expected=
    if [ "$entry" != "$name" ]; then expected=${entry##*=}; fi
    guid=${GUIDS[$index]}
    index=$((index + 1))
    if [ -z "$guid" ]; then
        continue
    fi
    [ -n "$FIRST" ] || FIRST=$name
    echo "-- $name ($guid)"
    write_specs "$guid" "$SECOND"
    [ -f "$TMP/first-page.json" ] || cp "$TMP/page.json" "$TMP/first-page.json"
    slug=$(echo "$name" | tr 'A-Z' 'a-z')
    fetch "$slug-native" "/report/Lot%20Viewer?start=$START&end=$END&account=$guid"
    preview "$slug-spec" "$TMP/page.json"
    if [ -f "$TMP/$slug-native.html" ]; then
        compare "$name" "$TMP/$slug-native.html" "$TMP/$slug-spec.html"
        if [[ "$expected" =~ ^[0-9]+$ ]]; then
            row_count "$TMP/$slug-native.html" "$expected" "$name row count"
            row_count "$TMP/$slug-spec.html" "$expected" "$name spec row count"
        fi
    fi
    # The Description Filter is the report's, and the spec's field of the same
    # name: the same page, and fewer rows than the unfiltered one.
    fetch "$slug-filter-native" \
        "/report/Lot%20Viewer?start=$START&end=$END&account=$guid&description-filter=Sell"
    preview "$slug-filter-spec" "$TMP/filter.json"
    if [ -f "$TMP/$slug-filter-native.html" ]; then
        compare "$name (Description Filter)" \
            "$TMP/$slug-filter-native.html" "$TMP/$slug-filter-spec.html"
        all=$(grep -o '<tr>' "$TMP/$slug-native.html" | wc -l)
        filtered=$(grep -o '<tr>' "$TMP/$slug-filter-native.html" | wc -l)
        if [ "$filtered" -ge "$all" ]; then
            bad "$name: the Description Filter removed no row ($filtered of $all)"
        else
            ok "$name: the Description Filter lists $filtered of $all rows"
        fi
    fi
done

echo "== the whole book, which is the report's page with no Account"
fetch whole-native "/report/Lot%20Viewer?start=$START&end=$END"
preview whole-spec "$TMP/whole-book.json"
[ -f "$TMP/whole-native.html" ] && compare "the whole book" \
    "$TMP/whole-native.html" "$TMP/whole-spec.html"

# The saved report is the first account's spec, kept aside because the loop
# above writes the last account's page over $TMP/page.json.
if [ -f "$TMP/first-page.json" ]; then
    cp "$TMP/first-page.json" "$TMP/page.json"
fi

echo "== a saved report, not only a preview"
SAVED=$(curl -sS -X POST -H 'Content-Type: application/json' \
    --data-binary @"$TMP/page.json" "${URL%/}/api/specs" | python3 -c '
import json, sys
print(json.load(sys.stdin)["id"])
')
if [ -n "$SAVED" ] && curl -sS -o "$TMP/saved.html" -w '%{http_code}' \
        "${URL%/}/report/spec/$SAVED" | grep -q 200; then
    first_slug=$(echo "$FIRST" | tr 'A-Z' 'a-z')
    compare "saved lots report" "$TMP/$first_slug-native.html" "$TMP/saved.html"
else
    bad "the saved report did not render at /report/spec/$SAVED"
fi
curl -sS "${URL%/}/api/specs/$SAVED" > "$TMP/saved.json"
python3 - "$TMP/saved.json" "$START" "$END" <<'PY'
import json, sys
with open(sys.argv[1], encoding="utf-8") as handle:
    spec = json.load(handle)
failures = 0
for field, expected in (("source", "lots"), ("start", sys.argv[2]),
                        ("end", sys.argv[3]),
                        ("columns", ["date", "description", "type", "lots",
                                     "other-account"])):
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
# The report's own columns, which is what the source publishes: three row
# labels, the "lots" placeholder for the book's lot columns and the
# transaction's remaining splits.
columns = ["date", "description", "type", "lots", "other-account"]

failures = 0
def check(condition, message):
    global failures
    if condition:
        print("ok   " + message)
    else:
        failures += 1
        print("FAIL " + message)

sources = {entry["name"]: entry for entry in options.get("sources") or []}
check("lots" in sources, "the source list names 'lots'")
check("Lot Viewer" in (sources.get("lots") or {}).get("label", ""),
      "its label names the report")
lots = options.get("lots") or {}
check(lots.get("columns") == columns,
      "it publishes the source's fixed columns")
check(lots.get("options") == ["start", "end", "accounts", "description_filter"],
      "it publishes the source's only options")
note = lots.get("note") or ""
check("lot" in note and "book" in note,
      "its note says the lot columns are the book's")
check("portfolio" in note,
      "its note says the portfolio reports are not this source")
raise SystemExit(1 if failures else 0)
PY
[ $? -eq 0 ] || FAILS=$((FAILS + 1))

echo "== the designer's starting point is the report"
curl -sS "${URL%/}/api/report/Lot%20Viewer/designer" > "$TMP/designer.json"
curl -sS "${URL%/}/report/Lot%20Viewer" > "$TMP/designer-native.html"
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
check(spec.get("source") == "lots", "its source is 'lots'")
check(spec.get("columns") == ["date", "description", "type", "lots",
                              "other-account"],
      "its columns are the report's own, lot columns included")
check(not spec.get("accounts"),
      "it has no account, which is the report's default page")
check(isinstance(spec.get("start"), str) and len(spec["start"]) == 10,
      "it carries the report's default start date")
check(isinstance(spec.get("end"), str) and len(spec["end"]) == 10,
      "it carries the report's default end date")
note = info.get("note") or ""
check("lot" in note and "transaction" in note,
      "its note explains the transaction rows and the lot columns")
check("sum" in note,
      "its note says where a cell can differ: more than one split in a lot")
# The starting spec must render the report's default page.  The copy has its
# own name and title; keep them out of the comparison.
spec["name"] = spec["title"] = "Lot Viewer (copy)"
with open(os.path.join(tmp, "designer-spec.json"), "w", encoding="utf-8") as h:
    json.dump(spec, h)
raise SystemExit(1 if failures else 0)
PY
[ $? -eq 0 ] || FAILS=$((FAILS + 1))
preview designer "$TMP/designer-spec.json"
[ -f "$TMP/designer-native.html" ] && compare "the report's default page" \
    "$TMP/designer-native.html" "$TMP/designer.html"

echo "== the fields the report does not have"
for pair in \
    "date-preset:'date_preset'" "currency:'currency'" "grouping:'grouping'" \
    "period-unit:'period_unit'" "period-mode:'period_mode'" \
    "account-types:'account_types'" "chart:'chart'" \
    "chart-series:'chart_series'" "split-grouping:'split_grouping'" \
    "invoice-owner:'invoice_owner'" "invoice-grouping:'invoice_grouping'" \
    "bucket-basis:'bucket_basis'" "columns-extra:'columns'" \
    "columns-order:'columns'" "columns-lot-dropped:'columns'"
do
    slug=${pair%%:*}
    field=${pair#*:}
    expect_400 "$slug" "$TMP/bad-$slug.json" "$field"
done
if [ -f "$TMP/bad-accounts-two.json" ]; then
    expect_400 "accounts-two" "$TMP/bad-accounts-two.json" "'accounts'"
fi
expect_400 "a lots field on an accounts spec" \
    "$TMP/bad-filter-on-accounts.json" "'description_filter'"

stop_server

echo "== the empty page, on a book with no stock account"
if [ -f "$EMPTY_BOOK" ]; then
    start_server "$EMPTY_BOOK"
    echo "-- $(basename "$EMPTY_BOOK")"
    fetch empty-native "/report/Lot%20Viewer"
    preview empty-spec "$TMP/default.json"
    if [ -f "$TMP/empty-native.html" ]; then
        compare "the default page on $(basename "$EMPTY_BOOK")" \
            "$TMP/empty-native.html" "$TMP/empty-spec.html"
        page_shape "$TMP/empty-native.html" empty \
            "$(basename "$EMPTY_BOOK") Lot Viewer is the empty page"
        page_shape "$TMP/empty-spec.html" empty \
            "$(basename "$EMPTY_BOOK") lots spec is the empty page"
    fi
    stop_server
else
    bad "empty-state book not found at $EMPTY_BOOK"
fi

if [ "$FAILS" -ne 0 ]; then
    echo "$FAILS check(s) failed"
    exit 1
fi
echo "lots specs match the native Lot Viewer"
