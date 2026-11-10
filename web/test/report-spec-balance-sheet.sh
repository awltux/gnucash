#!/bin/bash
# Balance sheet specs: a hand-written spec against the native Balance Sheet.
#
# Usage: report-spec-balance-sheet.sh <build-dir> <source-dir> [book] [as-of]
#
# Starts gnc-webd against the book, then renders the native Balance Sheet and a
# balance-sheet spec through /api/spec/preview for three as-of settings and
# compares the two documents cell for cell:
#
#   * the given as-of date (default 2025-12-31), which is the report's normal
#     page: the Assets, Liabilities and Equity sections, one row per account,
#     the sign reversal for liabilities and equity, the Retained Earnings line,
#     the section totals and Total Liabilities & Equity;
#   * "all dates" (the route's period=all, a spec with no `end`), which is the
#     same table drawn with no upper bound;
#   * an as-of date before the book's first transaction, the report's empty
#     page: the same sections and totals with every amount zero and no
#     Retained Earnings line.
#
# The comparison reads each document's body rows and its column headings, and
# treats an amount as a number, so currency symbols and markup do not count as
# a difference.  The document title is deliberately not compared: a spec is a
# saved report with its own name and title, which is the one thing the copy
# does not take from the report (the route's title carries the report date).
#
# The fields the report does not have are checked too: each one, posted on an
# otherwise valid balance-sheet spec, must be a 400 whose body names the
# field.  The report's own designer description is checked to say what is now
# true, for this report and for the multicolumn form it cannot describe.
#
# Exit status: 0 when every check passes, 1 otherwise.
set -u

USAGE="usage: report-spec-balance-sheet.sh <build> <source> [book] [as-of]"
BUILD=${1:?$USAGE}
SRC=${2:?$USAGE}
BOOK=${3:-$SRC/libgnucash/backend/xml/test/test-files/load-save/sample1.gnucash}
END=${4:-2025-12-31}

fail() { echo "FAIL: $*" >&2; exit 1; }

[ -x "$BUILD/bin/gnc-webd" ] || fail "gnc-webd was not built"
[ -f "$BOOK" ] || fail "fixture book not found at $BOOK"
case "$END" in
[0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9]) ;;
*) fail "as-of date must be YYYY-MM-DD, got '$END'" ;;
esac

BACKEND="$BUILD/lib/gnucash/libgncmod-backend-xml.so"
SCHEMAS="$BUILD/share/glib-2.0/schemas"
CCACHE=$(ls -d "$BUILD"/lib/*/guile/3.0/site-ccache 2>/dev/null | head -1)
[ -f "$SCHEMAS/gschemas.compiled" ] || SCHEMAS=""

TMP=$(mktemp -d)
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
    GNC_WEBUI_REPORTS="$TMP/reports.json" \
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
echo "book:   $BOOK"
echo "as of:  $END"

FAILS=0
ok() { echo "ok   $*"; }
bad() { echo "FAIL $*"; FAILS=$((FAILS + 1)); }

# The native page and the spec, one document each.  The spec documents are
# written by python so the dates are JSON-escaped, not shell-quoted.
python3 - "$TMP" "$END" <<'PY'
import json, os, sys
out, end = sys.argv[1], sys.argv[2]

def write(name, spec):
    with open(os.path.join(out, name + ".json"), "w", encoding="utf-8") as h:
        json.dump(spec, h)

base = {"name": "Balance Sheet as a spec",
        "title": "Balance Sheet as a spec",
        "source": "balance-sheet"}
write("as-of", dict(base, end=end))
write("all-dates", dict(base))
write("empty", dict(base, end="1970-01-01"))

# One mutation per field the report does not have.  Each is otherwise a valid
# balance-sheet spec, so a 400 can only be about the field under test.
variants = {
    "start": {"start": "2020-01-01"},
    "date-preset": {"date_preset": "this-year"},
    "grouping": {"grouping": "account-tree"},
    "period-unit": {"period_unit": "month"},
    "period-mode": {"period_mode": "cumulative"},
    "account-types": {"account_types": ["ASSET"]},
    "chart": {"chart": "bar"},
    "chart-series": {"chart_series": ["balance"]},
    "columns-extra": {"columns": ["account-name", "balance", "debit"]},
    "columns-order": {"columns": ["balance", "account-name"]},
}
for name, extra in variants.items():
    write("bad-" + name, dict(base, end=end, **extra))
PY

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
# report-spec-listings.sh uses: every body row becomes the sequence of its
# non-empty cells, an amount cell as a number and every other cell as text.
# That is the report's claim -- the rows, their order, their values, their
# signs and their totals -- independent of each renderer's cell classes,
# colspans and blank padding.  The column headings are compared too.
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
        self.cls, self.head = "", False
        self.headings = []

    def handle_starttag(self, tag, attrs):
        if tag == "tr":
            self.row = []
        elif tag in ("td", "th") and self.row is not None:
            self.cell, self.cls, self.head = [], attrs and dict(attrs).get("class", "") or "", tag == "th"

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
    return rows, [h for h in parser.headings if h]

description, native_path, spec_path = sys.argv[1:4]
native, native_headings = canonical(native_path)
spec, spec_headings = canonical(spec_path)

failed = False
if native != spec:
    failed = True
    print(f"{description}: DIFFER: native {len(native)} rows, spec {len(spec)}")
    differing = 0
    for index in range(max(len(native), len(spec))):
        left = native[index] if index < len(native) else None
        right = spec[index] if index < len(spec) else None
        if left == right:
            continue
        differing += 1
        if differing <= 12:
            print(f"  row {index}: native={left!r} spec={right!r}")
    print(f"  {differing} differing row(s)")
else:
    print(f"{description}: {len(native)} rows match")

if native_headings and native_headings != spec_headings:
    failed = True
    print(f"  headings: native={native_headings!r} spec={spec_headings!r}")
elif native_headings:
    print(f"  headings: {len(native_headings)} match")

if failed:
    raise SystemExit(1)
PY
    if [ $? -eq 0 ]; then
        ok "$1"
    else
        bad "$1"
    fi
}

# A spec that sets a field the report does not have must be refused, naming it.
expect_400() { # description json-file field
    local code
    code=$(curl -sS -o "$TMP/refused.html" -w '%{http_code}' -X POST \
        -H 'Content-Type: application/json' \
        --data-binary @"$2" "${URL%/}/api/spec/preview")
    if [ "$code" != "400" ]; then
        bad "$1: expected 400, got $code: $(head -c 200 "$TMP/refused.html")"
    elif ! grep -q -- "$3" "$TMP/refused.html"; then
        bad "$1: 400 does not name '$3': $(head -c 200 "$TMP/refused.html")"
    else
        ok "400 names the field ($1)"
    fi
}

echo "== the report's own page"
fetch native "/report/Balance%20Sheet?end=$END"
preview spec "$TMP/as-of.json"
[ -f "$TMP/native.html" ] && compare "Balance Sheet as of $END" \
    "$TMP/native.html" "$TMP/spec.html"

echo "== all dates"
fetch native-all "/report/Balance%20Sheet?period=all"
preview spec-all "$TMP/all-dates.json"
[ -f "$TMP/native-all.html" ] && compare "Balance Sheet (all dates)" \
    "$TMP/native-all.html" "$TMP/spec-all.html"

echo "== the empty page, before the book's first transaction"
fetch native-empty "/report/Balance%20Sheet?end=1970-01-01"
preview spec-empty "$TMP/empty.json"
[ -f "$TMP/native-empty.html" ] && compare "Balance Sheet (as of 1970-01-01)" \
    "$TMP/native-empty.html" "$TMP/spec-empty.html"

echo "== a saved report, not only a preview"
SAVED=$(curl -sS -X POST -H 'Content-Type: application/json' \
    --data-binary @"$TMP/as-of.json" "${URL%/}/api/specs" | python3 -c '
import json, sys
print(json.load(sys.stdin)["id"])
')
if [ -n "$SAVED" ] && curl -sS -o "$TMP/saved.html" -w '%{http_code}' \
        "${URL%/}/report/spec/$SAVED" | grep -q 200; then
    compare "saved balance-sheet report" "$TMP/native.html" "$TMP/saved.html"
else
    bad "the saved report did not render at /report/spec/$SAVED"
fi

echo "== the designer's starting point is the report"
curl -sS "${URL%/}/api/report/Balance%20Sheet/designer" \
    > "$TMP/designer-start.json"
python3 - "$TMP" <<'PY'
import json, os, sys
tmp = sys.argv[1]
with open(os.path.join(tmp, "designer-start.json"), encoding="utf-8") as handle:
    spec = json.load(handle)["spec"]
# The starting point has its own name and title, as a copy does; everything
# else is what a client would save.  Keep the report's own title out of the
# comparison by naming it here.
spec["name"] = spec["title"] = "Balance Sheet (copy)"
with open(os.path.join(tmp, "designer-spec.json"), "w", encoding="utf-8") as h:
    json.dump(spec, h)
PY
preview designer "$TMP/designer-spec.json"
# The report with no options is its own defaults, which is what the starting
# point says it is.
fetch native-default "/report/Balance%20Sheet"
[ -f "$TMP/native-default.html" ] && compare "the report's default page" \
    "$TMP/native-default.html" "$TMP/designer.html"

echo "== the fields the report does not have"
for pair in \
    "start:start" "date_preset:date-preset" "grouping:grouping" \
    "period_unit:period-unit" "period_mode:period-mode" \
    "account_types:account-types" "chart:chart" "chart_series:chart-series" \
    "columns:columns-extra" "columns:columns-order"
do
    field=${pair%%:*}
    slug=${pair##*:}
    expect_400 "$field" "$TMP/bad-$slug.json" "'$field'"
done

# The account scope is the one field whose 400 needs a real GUID: the validator
# checks the GUID against the book before it can say the field does not apply.
GUID=$(curl -sS "${URL%/}/api/accounts" | python3 -c '
import json, sys
tree = json.load(sys.stdin)
stack = [tree]
while stack:
    node = stack.pop()
    stack.extend(node.get("children") or [])
    if node.get("guid"):
        print(node["guid"])
        break
')
if [ -n "$GUID" ]; then
    python3 - "$TMP" "$END" "$GUID" <<'PY'
import json, os, sys
out, end, guid = sys.argv[1:4]
spec = {"name": "Balance Sheet with a scope",
        "title": "Balance Sheet with a scope",
        "source": "balance-sheet", "end": end, "accounts": [guid]}
with open(os.path.join(out, "bad-accounts.json"), "w", encoding="utf-8") as h:
    json.dump(spec, h)
PY
    expect_400 "accounts" "$TMP/bad-accounts.json" "'accounts'"
else
    bad "no account GUID from /api/accounts, cannot test the 'accounts' refusal"
fi

echo "== the designer's description"
curl -sS "${URL%/}/api/report/Balance%20Sheet/designer" \
    > "$TMP/designer-balance-sheet.json"
curl -sS "${URL%/}/api/report/Balance%20Sheet%20(Multicolumn)/designer" \
    > "$TMP/designer-multicolumn.json"
curl -sS "${URL%/}/api/spec/options" > "$TMP/options.json"
python3 - "$TMP" <<'PY'
import json, os, sys

tmp = sys.argv[1]

def load(name):
    with open(os.path.join(tmp, name), encoding="utf-8") as handle:
        return json.load(handle)

failures = 0

def check(condition, message):
    global failures
    if condition:
        print("ok   " + message)
    else:
        failures += 1
        print("FAIL " + message)

single = load("designer-balance-sheet.json")
check(single.get("equivalence") == "exact",
      "the Balance Sheet's designer description is exact")
spec = single.get("spec") or {}
check(spec.get("source") == "balance-sheet",
      "its starting spec has source balance-sheet")
check(spec.get("columns") == ["account-name", "balance"],
      "its starting spec carries the report's two columns")
check(isinstance(spec.get("end"), str) and len(spec["end"]) == 10,
      "its starting spec carries the report's default as-of date")
check(spec.get("start") == "",
      "its starting spec has no start date")
check("Retained Earnings" in (single.get("note") or ""),
      "its note explains the retained-earnings line")

multicolumn = load("designer-multicolumn.json")
check(multicolumn.get("equivalence") == "none" and not multicolumn.get("spec"),
      "the multicolumn report has no starting spec")
note = multicolumn.get("note") or ""
check("column" in note and "period" in note,
      "its note says why: the spec has no period column axis")

options = load("options.json")
sources = {entry["name"]: entry for entry in options.get("sources") or []}
check("balance-sheet" in sources,
      "/api/spec/options advertises the balance-sheet source")
fixed = options.get("balance_sheet") or {}
check(fixed.get("columns") == ["account-name", "balance"],
      "it publishes the source's fixed columns")
check(fixed.get("options") == ["end", "currency"],
      "it publishes the source's only options")

raise SystemExit(1 if failures else 0)
PY
if [ $? -ne 0 ]; then
    FAILS=$((FAILS + 1))
fi

if [ "$FAILS" -ne 0 ]; then
    echo "$FAILS check(s) failed"
    exit 1
fi
echo "balance sheet specs match the native Balance Sheet"
