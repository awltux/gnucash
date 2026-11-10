#!/bin/bash
# Split listing specs: a designer spec against the native listing it describes.
#
# Usage: report-spec-listings.sh <build-dir> <source-dir> <book> <start> <end>
#                               [account-guid]
#
# Starts gnc-webd against the book, then for each of the native listings a
# split spec can describe -- the Transaction Report, the General Ledger and the
# General Journal -- renders the native report and a hand-written spec through
# /api/spec/preview, and compares the two documents cell for cell.  The
# comparison reads the body rows (the reports' own data) and each report's
# column headings, and treats an amount as a number, so currency symbols and
# markup do not count as a difference.
#
# The empty state is checked too: the Transaction Report and the General Ledger
# are rendered over a range with no splits, and both documents must have no
# body rows.  (The General Journal's layout is the whole book's journal, so its
# empty state needs a book with no transactions; see the report's note.)
#
# [account-guid], when given, is used as the account scope of a second
# Transaction Report and General Ledger pair, so the spec's `accounts` field
# and the route's `accounts=` option are compared on the same subtree.
#
# The Transaction Breakdown Report is deliberately absent: its columns are one
# per account the transactions touch, under a multi-row account-tree header,
# which a fixed column list cannot express.
#
# Exit status: 0 when every listing matches, 1 otherwise.
set -u

USAGE="usage: report-spec-listings.sh <build> <source> <book> <start> <end> [account]"
BUILD=${1:?$USAGE}
SRC=${2:?}
BOOK=${3:?}
START=${4:?}
END=${5:?}
SCOPE=${6:-}

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

FAILS=0
ok() { echo "ok   $*"; }
bad() { echo "FAIL $*"; FAILS=$((FAILS + 1)); }

# The spec documents, one per listing.  They are written by python so the
# dates and the optional account GUID are JSON-escaped, not shell-quoted.
python3 - "$TMP" "$START" "$END" "$SCOPE" <<'PY'
import json, os, sys
out, start, end = sys.argv[1], sys.argv[2], sys.argv[3]
scope = sys.argv[4]
base = {"source": "splits", "period_unit": "year", "period_mode": "activity"}
specs = {
    "transaction-report": {
        "name": "Transaction Report as a spec",
        "split_grouping": "account",
        "columns": ["date", "num", "description", "memo", "transfer",
                    "debit", "credit"],
    },
    "general-ledger": {
        "name": "General Ledger as a spec",
        "split_grouping": "account-code",
        "columns": ["date", "description", "debit", "credit",
                    "running-balance"],
    },
    "general-journal": {
        "name": "General Journal as a spec",
        "split_grouping": "transaction",
        "columns": ["date", "description", "account", "debit", "credit"],
    },
}
for name, spec in specs.items():
    spec.update(base)
    spec["title"] = spec["name"]
    if name != "general-journal":
        spec["start"] = start
        spec["end"] = end
    with open(os.path.join(out, name + ".json"), "w", encoding="utf-8") as h:
        json.dump(spec, h)
    # A second copy scoped to one account, so the spec's `accounts` field and
    # the route's `accounts=` option are compared on the same subtree.
    if scope and name != "general-journal":
        spec["accounts"] = [scope]
        with open(os.path.join(out, name + "-scope.json"), "w",
                  encoding="utf-8") as h:
            json.dump(spec, h)
# The same two listings over a range with no splits, for the empty state.
empty = {"source": "splits", "split_grouping": "account",
         "columns": ["date", "num", "description", "debit", "credit"],
         "start": "1970-01-01", "end": "1970-01-02",
         "name": "Empty Transaction Report",
         "title": "Empty Transaction Report"}
with open(os.path.join(out, "empty-transaction-report.json"), "w") as h:
    json.dump(empty, h)
empty["split_grouping"] = "account-code"
empty["columns"] = ["date", "description", "debit", "credit",
                    "running-balance"]
empty["name"] = empty["title"] = "Empty General Ledger"
with open(os.path.join(out, "empty-general-ledger.json"), "w") as h:
    json.dump(empty, h)
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

# Compare two documents cell for cell.  The extractor is deliberately shared by
# both sides: every body row becomes the sequence of its non-empty cells, an
# amount cell as a number and every other cell as text.  That is the listing's
# claim -- the rows, their order, their values and their totals -- independent
# of each renderer's cell classes, colspans and blank padding.  The column
# headings are compared too when the native report prints them (the
# Transaction Report prints none).
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

# A report that prints a heading row must print the same headings; the
# Transaction Report's native table has no heading row at all, and the spec's
# own legend is the only one.
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

echo "== the listings a split spec can describe"

fetch tr "/report/Transaction%20Report?start=$START&end=$END&accounts=all"
preview tr-spec "$TMP/transaction-report.json"
[ -f "$TMP/tr.html" ] && compare "Transaction Report" "$TMP/tr.html" "$TMP/tr-spec.html"

fetch gl "/report/General%20Ledger?start=$START&end=$END&accounts=all"
preview gl-spec "$TMP/general-ledger.json"
[ -f "$TMP/gl.html" ] && compare "General Ledger" "$TMP/gl.html" "$TMP/gl-spec.html"

fetch gj "/report/General%20Journal"
preview gj-spec "$TMP/general-journal.json"
[ -f "$TMP/gj.html" ] && compare "General Journal" "$TMP/gj.html" "$TMP/gj-spec.html"

if [ -n "$SCOPE" ]; then
    fetch tr-scope "/report/Transaction%20Report?start=$START&end=$END&accounts=$SCOPE"
    preview tr-scope-spec "$TMP/transaction-report-scope.json"
    [ -f "$TMP/tr-scope.html" ] && compare "Transaction Report (account scope)" \
        "$TMP/tr-scope.html" "$TMP/tr-scope-spec.html"

    fetch gl-scope "/report/General%20Ledger?start=$START&end=$END&accounts=$SCOPE"
    preview gl-scope-spec "$TMP/general-ledger-scope.json"
    [ -f "$TMP/gl-scope.html" ] && compare "General Ledger (account scope)" \
        "$TMP/gl-scope.html" "$TMP/gl-scope-spec.html"
fi

echo "== the settings with no splits"

fetch tr-empty "/report/Transaction%20Report?start=1970-01-01&end=1970-01-02&accounts=all"
preview tr-empty-spec "$TMP/empty-transaction-report.json"
[ -f "$TMP/tr-empty.html" ] && compare "Transaction Report (empty range)" \
    "$TMP/tr-empty.html" "$TMP/tr-empty-spec.html"

fetch gl-empty "/report/General%20Ledger?start=1970-01-01&end=1970-01-02&accounts=all"
preview gl-empty-spec "$TMP/empty-general-ledger.json"
[ -f "$TMP/gl-empty.html" ] && compare "General Ledger (empty range)" \
    "$TMP/gl-empty.html" "$TMP/gl-empty-spec.html"

if [ "$FAILS" -ne 0 ]; then
    echo "$FAILS check(s) failed"
    exit 1
fi
echo "split listing specs match the native listings"
