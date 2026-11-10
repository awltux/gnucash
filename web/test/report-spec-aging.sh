#!/bin/bash
# Invoices specs: a hand-written spec against the native aging reports.
#
# Usage: report-spec-aging.sh <build-dir> <source-dir> <book> [as-of]
#                             [empty-book...]
#
# Starts gnc-webd against the book, then for both aging reports -- the
# Receivable Aging and the Payable Aging -- renders the native report and a
# hand-written spec through /api/spec/preview and compares the two documents
# cell for cell:
#
#   * the given as-of date (default 2026-06-30), the report's "To" option;
#   * 91 days earlier, so a due date that was 91+ days past falls in "Current";
#   * 30 days earlier, one bucket further in.
#
# Three dates, because the claim is the bucket arithmetic, not one page: the
# same documents must land in the bucket the report puts them in, at any date.
#
# The comparison reads each document's body rows and its column headings, and
# treats an amount as a number, so currency symbols and markup do not count as
# a difference.  The document title is deliberately not compared: a spec is a
# saved report with its own name and title, which is the one thing the copy
# does not take from the report (the route's title carries the report date).
#
# The empty state is checked on the books given after the date (default: the
# tree's sample1 and taxreport, which have no invoices).  Each book gets its
# own server, because the page depends on the book.  The native page and the
# spec page must both print the report's explanation and neither may print a
# table row; the two explanations the report has (no aging account at all, and
# aging accounts with nothing to age) are both exercised by the two fixtures.
# On sample1 the Payable Aging is not empty (it has a vendor bill), so that page
# is compared as a table instead and says so.
#
# The fields the report does not have are checked too: each one, posted on an
# otherwise valid invoices spec, must be a 400 whose body names the field.  So
# are a wrong owner kind, a wrong row set, a column from the other row set and
# an invoice field on a spec of another source.  /api/spec/options is checked
# to advertise the source, its two column lists and its options, and both aging
# reports' designer descriptions are checked to be exact -- including that the
# starting spec renders the report's own default page.
#
# Finally the document grouping is checked against the owner rows: the
# documents' bucket amounts, summed per owner, must be the report's owner
# buckets.  That is the report's own arithmetic seen at document level, and it
# is what makes a partially paid invoice show its remaining balance rather than
# its total.
#
# Exit status: 0 when every check passes, 1 otherwise.
set -u

USAGE="usage: report-spec-aging.sh <build> <source> <book> [as-of] [empty-book...]"
[ $# -ge 3 ] || { echo "$USAGE" >&2; exit 1; }
BUILD=$1
SRC=$2
BOOK=$3
shift 3

AS_OF=2026-06-30
if [ $# -gt 0 ] && [[ $1 =~ ^[0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9]$ ]]; then
    AS_OF=$1
    shift
fi
EARLY_CURRENT=$(date -d "$AS_OF -91 days" +%F)
EARLY_61_90=$(date -d "$AS_OF -30 days" +%F)

fail() { echo "FAIL: $*" >&2; exit 1; }

[ -x "$BUILD/bin/gnc-webd" ] || fail "gnc-webd was not built"
[ -f "$BOOK" ] || fail "fixture book not found at $BOOK"
case "$AS_OF" in
[0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9]) ;;
*) fail "as-of date must be YYYY-MM-DD, got '$AS_OF'" ;;
esac

# The books whose empty page is checked.  The two tree fixtures have no
# invoice at all: taxreport has no A/Receivable or A/Payable account (the first
# explanation) and sample1 has an A/Receivable account with nothing to age (the
# second one).  sample1's A/Payable account does have a bill, so its Payable
# Aging is a table, and the same comparison checks it.
EMPTY_BOOKS=("$@")
if [ ${#EMPTY_BOOKS[@]} -eq 0 ]; then
    EMPTY_BOOKS=("$SRC/libgnucash/backend/xml/test/test-files/load-save/sample1.gnucash"
                 "$SRC/doc/examples/taxreport.gnucash")
fi

BACKEND="$BUILD/lib/gnucash/libgncmod-backend-xml.so"
SCHEMAS="$BUILD/share/glib-2.0/schemas"
CCACHE=$(ls -d "$BUILD"/lib/*/guile/3.0/site-ccache 2>/dev/null | head -1)
[ -f "$SCHEMAS/gschemas.compiled" ] || SCHEMAS=""

TMP=$(mktemp -d)
SERVER_PID=
BOOK_NOW=
cleanup() {
    [ -n "$SERVER_PID" ] && kill "$SERVER_PID" 2>/dev/null
    rm -rf "$TMP"
}
trap cleanup EXIT

BACKEND_ARGS=()
[ -f "$BACKEND" ] && BACKEND_ARGS=(--backend "$BACKEND")

start_server() { # book
    BOOK_NOW=$1
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
        "$BUILD/bin/gnc-webd" "${BACKEND_ARGS[@]}" "$BOOK_NOW" > "$log" 2>&1 &
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

# The spec documents, written by python so the dates are JSON-escaped, not
# shell-quoted.  The owner columns are the report's own; the document spec
# lists the documents the buckets are made of.
python3 - "$TMP" "$AS_OF" "$EARLY_CURRENT" "$EARLY_61_90" "${#EMPTY_BOOKS[@]}" <<'PY'
import json, os, sys
out, as_of, early_current, early_61_90, empty_count = sys.argv[1:6]

def write(name, spec):
    with open(os.path.join(out, name + ".json"), "w", encoding="utf-8") as h:
        json.dump(spec, h)

OWNER_COLUMNS = ["owner", "pre-payment", "current", "0-30-days", "31-60-days",
                 "61-90-days", "91-plus-days", "total"]
DOCUMENT_COLUMNS = ["document-type", "number", "date", "due-date", "owner",
                    "total", "tax", "balance", "pre-payment", "current",
                    "0-30-days", "31-60-days", "61-90-days", "91-plus-days"]

for name, columns in (("owner-columns", OWNER_COLUMNS),
                      ("document-columns", DOCUMENT_COLUMNS)):
    write(name, {"name": "columns", "columns": columns})

def base(report, end, **extra):
    spec = {"name": report + " as a spec",
            "title": report + " as a spec",
            "source": "invoices",
            "invoice_owner": "receivable" if "Receivable" in report else "payable",
            "columns": OWNER_COLUMNS}
    if end is not None:
        spec["end"] = end
    spec.update(extra)
    return spec

for report, key in (("Receivable Aging", "receivable"),
                    ("Payable Aging", "payable")):
    write("owner-%s-%s" % (key, as_of), base(report, as_of))
    write("owner-%s-current" % key, base(report, early_current))
    write("owner-%s-61-90" % key, base(report, early_61_90))

# The document listing: the same amounts, one row per document.
for report, key in (("Receivable Aging", "receivable"),
                    ("Payable Aging", "payable")):
    write("document-%s" % key,
          base(report, as_of, columns=DOCUMENT_COLUMNS,
               invoice_grouping="document"))

# The report's other bucket basis, which a spec may ask for even though the
# route fixes it at the default.
write("posted-receivable", base("Receivable Aging", as_of,
                                bucket_basis="posted"))

# An empty page, at the same date, for each fixture book.
for index in range(int(empty_count)):
    for report, key in (("Receivable Aging", "receivable"),
                        ("Payable Aging", "payable")):
        write("empty-%d-%s" % (index, key), base(report, as_of))

# One mutation per field the report does not have.  Each is otherwise a valid
# invoices spec, so a 400 can only be about the field under test.
variants = {
    "start": {"start": "2020-01-01"},
    "date-preset": {"date_preset": "this-year"},
    "currency": {"currency": "USD"},
    "grouping": {"grouping": "period"},
    "period-unit": {"period_unit": "month"},
    "period-mode": {"period_mode": "cumulative"},
    "chart": {"chart": "bar"},
    "chart-series": {"chart_series": ["balance"]},
    "account-types": {"account_types": ["RECEIVABLE"]},
    "owner-kind": {"invoice_owner": "customer"},
    "row-set": {"invoice_grouping": "period"},
    "bucket-basis": {"bucket_basis": "invoice-date"},
    "wrong-source-column": {"columns": ["number"]},
    "wrong-vocabulary": {"columns": ["account-name"]},
}
for name, extra in variants.items():
    write("bad-" + name, base("Receivable Aging", as_of, **extra))
# An invoice field on a spec of another source is refused, not ignored.
write("bad-owner-on-accounts",
      {"name": "accounts with an invoice field", "source": "accounts",
       "columns": ["account-name", "balance"], "date_preset": "all",
       "invoice_owner": "payable"})
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
# report-spec-balance-sheet.sh uses -- every body row becomes the sequence of
# its non-empty cells, an amount cell as a number and every other cell as text
# -- plus the report's explanation paragraph, because the aging reports print
# one instead of a table when there is nothing to age.  That is the report's
# claim: the same rows, in the same order, with the same values, headings and
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
else:
    empty = " (the empty page)" if not native_rows else ""
    print(f"{description}: {len(native_rows)} rows match{empty}")

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

# Which shape a native page has: the explanation and no rows, or a table.
page_shape() { # html expected(empty|table) description
    python3 - "$1" "$2" "$3" <<'PY'
import re, sys
from html.parser import HTMLParser

class Page(HTMLParser):
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.tds, self.paras, self.cell, self.para = 0, [], None, False
    def handle_starttag(self, tag, attrs):
        if tag == "td":
            self.tds += 1
        elif tag == "p":
            self.cell, self.para = [], True
    def handle_endtag(self, tag):
        if tag == "p" and self.cell is not None:
            self.paras.append(" ".join("".join(self.cell).split()))
            self.cell, self.para = None, False
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

start_server "$BOOK"
echo "server: $URL"
echo "book:   $BOOK"
echo "as of:  $AS_OF (also $EARLY_CURRENT and $EARLY_61_90)"

echo "== the report's own page, at three dates"
for report_slug in receivable payable; do
    if [ "$report_slug" = receivable ]; then
        REPORT="Receivable%20Aging"
        LABEL="Receivable Aging"
    else
        REPORT="Payable%20Aging"
        LABEL="Payable Aging"
    fi
    fetch "$report_slug-native" "/report/$REPORT?end=$AS_OF"
    preview "$report_slug-spec" "$TMP/owner-$report_slug-$AS_OF.json"
    [ -f "$TMP/$report_slug-native.html" ] && compare "$LABEL as of $AS_OF" \
        "$TMP/$report_slug-native.html" "$TMP/$report_slug-spec.html"

    fetch "$report_slug-current-native" "/report/$REPORT?end=$EARLY_CURRENT"
    preview "$report_slug-current-spec" "$TMP/owner-$report_slug-current.json"
    [ -f "$TMP/$report_slug-current-native.html" ] && compare \
        "$LABEL as of $EARLY_CURRENT" \
        "$TMP/$report_slug-current-native.html" \
        "$TMP/$report_slug-current-spec.html"

    fetch "$report_slug-61-90-native" "/report/$REPORT?end=$EARLY_61_90"
    preview "$report_slug-61-90-spec" "$TMP/owner-$report_slug-61-90.json"
    [ -f "$TMP/$report_slug-61-90-native.html" ] && compare \
        "$LABEL as of $EARLY_61_90" \
        "$TMP/$report_slug-61-90-native.html" \
        "$TMP/$report_slug-61-90-spec.html"
done

echo "== the report's other bucket basis"
# The route has no option for it, so the check is that the spec renders and
# that the posted date really moves a bucket relative to the due date.
preview posted-spec "$TMP/posted-receivable.json"
fetch posted-native "/report/Receivable%20Aging?end=$AS_OF"
python3 - "$TMP/posted-native.html" "$TMP/posted-spec.html" <<'PY'
import re, sys
from html.parser import HTMLParser

class Rows(HTMLParser):
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.rows, self.row, self.cell = [], None, None
    def handle_starttag(self, tag, attrs):
        if tag == "tr":
            self.row = []
        elif tag == "td" and self.row is not None:
            self.cell = []
    def handle_endtag(self, tag):
        if tag == "td" and self.cell is not None:
            self.row.append(" ".join("".join(self.cell).split()))
            self.cell = None
        elif tag == "tr" and self.row is not None:
            if self.row:
                self.rows.append(self.row)
            self.row = None
    def handle_data(self, data):
        if self.cell is not None:
            self.cell.append(data)

def numbers(path):
    parser = Rows()
    with open(path, encoding="utf-8") as handle:
        parser.feed(handle.read())
    return [[re.sub(r"[^0-9.\-]", "", cell) for cell in row]
            for row in parser.rows if row and row[0] != "Company"]

due, posted = numbers(sys.argv[1]), numbers(sys.argv[2])
if not posted:
    print("  FAIL the posted-date spec printed no rows")
    raise SystemExit(1)
if due == posted:
    print("  FAIL 'posted' produced the same buckets as 'due'")
    raise SystemExit(1)
print(f"  ok   'posted' moved the buckets: due={due[0]!r} posted={posted[0]!r}")
PY
[ $? -eq 0 ] && ok "the posted bucket basis" || bad "the posted bucket basis"

echo "== the document rows are the owner rows' own amounts"
preview document-receivable "$TMP/document-receivable.json"
preview document-payable "$TMP/document-payable.json"
python3 - "$TMP/receivable-spec.html" "$TMP/document-receivable.html" \
          "$TMP/payable-spec.html" "$TMP/document-payable.html" <<'PY'
import sys
from decimal import Decimal
from html.parser import HTMLParser

def amount(text):
    text = text.replace("\u00a0", " ").replace("\u2066", "").replace("\u2069", "")
    text = text.strip()
    if text.startswith("(") and text.endswith(")"):
        text = "-" + text[1:-1]
    text = text.replace("$", "").replace(",", "")
    try:
        return Decimal(text)
    except Exception:
        return Decimal(0)

class Table(HTMLParser):
    """Rows as {heading: value}; a blank cell is None."""
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.headings, self.rows, self.row, self.cell = [], [], None, None
        self.head = False
    def handle_starttag(self, tag, attrs):
        if tag == "tr":
            self.row = []
        elif tag in ("td", "th") and self.row is not None:
            self.cell, self.head = [], tag == "th"
    def handle_endtag(self, tag):
        if tag in ("td", "th") and self.cell is not None:
            text = " ".join("".join(self.cell).split())
            if self.head:
                self.headings.append(text)
            else:
                self.row.append(text)
            self.cell = None
        elif tag == "tr" and self.row is not None:
            if self.row:
                if not self.headings:
                    self.headings = list(self.row)
                else:
                    self.rows.append(dict(zip(self.headings, self.row)))
            self.row = None
    def handle_data(self, data):
        if self.cell is not None:
            self.cell.append(data)

def parse(path):
    parser = Table()
    with open(path, encoding="utf-8") as handle:
        parser.feed(handle.read())
    return parser

BUCKETS = ["Pre-Payment", "Current", "0-30 days", "31-60 days", "61-90 days",
           "91+ days"]

failed = False
for owner_page, document_page, label in (
        (sys.argv[1], sys.argv[2], "Receivable Aging"),
        (sys.argv[3], sys.argv[4], "Payable Aging")):
    owner = parse(owner_page)
    documents = parse(document_page)
    expected = {}
    for row in owner.rows:
        name = row.get("Company", "")
        if not name or name == "Total":
            continue
        expected[name] = {bucket: amount(row.get(bucket) or "0")
                          for bucket in BUCKETS}
    if not expected:
        print(f"  FAIL {label}: the owner page has no owner row")
        failed = True
        continue
    seen = {}
    listed = []
    for row in documents.rows:
        if row.get("Type") == "Total" or not row.get("Type"):
            continue
        name = row.get("Company", "")
        listed.append((row.get("Type"), row.get("Number"),
                       row.get("Due Date"), row.get("Company"),
                       amount(row.get("Total") or "0"),
                       amount(row.get("Balance") or "0")))
        for bucket in BUCKETS:
            seen.setdefault(name, dict.fromkeys(BUCKETS, Decimal(0)))
            seen[name][bucket] += amount(row.get(bucket) or "0")
    if set(seen) != set(expected):
        print(f"  FAIL {label}: document owners {sorted(seen)} vs {sorted(expected)}")
        failed = True
        continue
    for name in expected:
        for bucket in BUCKETS:
            if seen[name][bucket] != expected[name][bucket]:
                print(f"  FAIL {label}: {name} {bucket}: documents "
                      f"{seen[name][bucket]} vs owner row {expected[name][bucket]}")
                failed = True
    if not failed:
        print(f"  ok   {label}: {len(listed)} document row(s) sum to the "
              f"{len(expected)} owner row(s)")
        for entry in listed:
            print(f"       type={entry[0]!r} number={entry[1]!r} "
                  f"due={entry[2]!r} owner={entry[3]!r} "
                  f"total={entry[4]} balance={entry[5]}")
raise SystemExit(1 if failed else 0)
PY
[ $? -eq 0 ] && ok "the document rows" || bad "the document rows"

echo "== a saved report, not only a preview"
SAVED=$(curl -sS -X POST -H 'Content-Type: application/json' \
    --data-binary @"$TMP/owner-receivable-$AS_OF.json" "${URL%/}/api/specs" \
    | python3 -c '
import json, sys
print(json.load(sys.stdin)["id"])
')
if [ -n "$SAVED" ] && curl -sS -o "$TMP/saved.html" -w '%{http_code}' \
        "${URL%/}/report/spec/$SAVED" | grep -q 200; then
    compare "saved invoices report" "$TMP/receivable-native.html" "$TMP/saved.html"
else
    bad "the saved report did not render at /report/spec/$SAVED"
fi
curl -sS "${URL%/}/api/specs/$SAVED" > "$TMP/saved.json"
python3 - "$TMP/saved.json" <<'PY'
import json, sys
with open(sys.argv[1], encoding="utf-8") as handle:
    spec = json.load(handle)
failures = 0
for field, expected in (("source", "invoices"), ("invoice_owner", "receivable"),
                        ("invoice_grouping", "owner"), ("bucket_basis", "due")):
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
python3 - "$TMP/options.json" "$TMP/owner-columns.json" \
          "$TMP/document-columns.json" <<'PY'
import json, sys
with open(sys.argv[1], encoding="utf-8") as handle:
    options = json.load(handle)
owner_columns = json.load(open(sys.argv[2], encoding="utf-8"))["columns"]
document_columns = json.load(open(sys.argv[3], encoding="utf-8"))["columns"]

failures = 0
def check(condition, message):
    global failures
    if condition:
        print("ok   " + message)
    else:
        failures += 1
        print("FAIL " + message)

sources = {entry["name"] for entry in options.get("sources") or []}
check("invoices" in sources, "the source list names 'invoices'")
invoices = options.get("invoices") or {}
check(invoices.get("owner_columns") == owner_columns,
      "it publishes the owner row's columns")
check(invoices.get("document_columns") == document_columns,
      "it publishes the document row's columns")
check(sorted(entry["name"] for entry in (invoices.get("owners") or []))
      == ["payable", "receivable"],
      "it publishes the two owner sides")
check([entry["name"] for entry in (invoices.get("groupings") or [])]
      == ["owner", "document"],
      "it publishes the two row sets")
check([entry["name"] for entry in (invoices.get("bucket_basis") or [])]
      == ["due", "posted"],
      "it publishes the two bucket bases")
check(invoices.get("options") == ["end"],
      "it publishes the source's only option")
check("remaining balance" in (invoices.get("note") or ""),
      "its note says how a partially paid document contributes")
raise SystemExit(1 if failures else 0)
PY
[ $? -eq 0 ] || FAILS=$((FAILS + 1))

echo "== the designer's starting point is the report"
for pair in "Receivable%20Aging:ReceivableAging" \
            "Payable%20Aging:PayableAging"
do
    report=${pair%%:*}
    slug=${pair##*:}
    curl -sS "${URL%/}/api/report/$report/designer" > "$TMP/designer-$slug.json"
    curl -sS "${URL%/}/report/$report" > "$TMP/designer-$slug-native.html"
done
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

for slug, owner in (("ReceivableAging", "receivable"),
                    ("PayableAging", "payable")):
    with open(os.path.join(tmp, "designer-" + slug + ".json"),
              encoding="utf-8") as handle:
        info = json.load(handle)
    spec = info.get("spec") or {}
    check(info.get("equivalence") == "exact",
          f"{slug}: the designer says the starting spec is exact")
    check(spec.get("source") == "invoices",
          f"{slug}: its source is 'invoices'")
    check(spec.get("invoice_owner") == owner,
          f"{slug}: its owner side is {owner!r}")
    check(spec.get("columns") == ["owner", "pre-payment", "current", "0-30-days",
                                  "31-60-days", "61-90-days", "91-plus-days",
                                  "total"],
          f"{slug}: its columns are the report's own")
    check(isinstance(spec.get("end"), str) and len(spec["end"]) == 10,
          f"{slug}: it carries the report's default date")
    note = info.get("note") or ""
    check("bucket" in note and "remaining balance" in note,
          f"{slug}: its note explains the buckets and the remaining balance")
    # The starting spec must render the report's default page.  The copy has
    # its own name and title; keep them out of the comparison.
    spec["name"] = spec["title"] = slug
    with open(os.path.join(tmp, "designer-spec-" + slug + ".json"), "w",
              encoding="utf-8") as handle:
        json.dump(spec, handle)
raise SystemExit(1 if failures else 0)
PY
[ $? -eq 0 ] || FAILS=$((FAILS + 1))
for slug in ReceivableAging PayableAging; do
    preview "designer-$slug" "$TMP/designer-spec-$slug.json"
    [ -f "$TMP/designer-$slug-native.html" ] && compare \
        "the report's default page ($slug)" \
        "$TMP/designer-$slug-native.html" "$TMP/designer-$slug.html"
done

echo "== the fields the report does not have"
for pair in \
    "start:'start'" "date-preset:'date_preset'" "currency:'currency'" \
    "grouping:'grouping'" "period-unit:'period_unit'" \
    "period-mode:'period_mode'" "chart:'chart'" \
    "chart-series:'chart_series'" "account-types:'account_types'" \
    "owner-kind:invoice_owner" "row-set:invoice_grouping" \
    "bucket-basis:bucket_basis"
do
    slug=${pair%%:*}
    field=${pair#*:}
    expect_400 "$slug" "$TMP/bad-$slug.json" "$field"
done
expect_400 "wrong-source-column" "$TMP/bad-wrong-source-column.json" \
    "document column"
expect_400 "wrong-vocabulary" "$TMP/bad-wrong-vocabulary.json" \
    "unknown invoice column"
expect_400 "an invoice field on an accounts spec" \
    "$TMP/bad-owner-on-accounts.json" "'invoice_owner'"

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
    python3 - "$TMP" "$AS_OF" "$GUID" <<'PY'
import json, os, sys
out, end, guid = sys.argv[1:4]
spec = {"name": "invoices with a scope",
        "source": "invoices", "end": end,
        "columns": ["owner", "total"], "accounts": [guid]}
with open(os.path.join(out, "bad-accounts.json"), "w", encoding="utf-8") as h:
    json.dump(spec, h)
PY
    expect_400 "accounts" "$TMP/bad-accounts.json" "'accounts'"
else
    bad "no account GUID from /api/accounts, cannot test the 'accounts' refusal"
fi

stop_server

echo "== the empty page, one server per book"
index=0
for empty_book in "${EMPTY_BOOKS[@]}"; do
    case "$empty_book" in
    sample1) empty_book="$SRC/libgnucash/backend/xml/test/test-files/load-save/sample1.gnucash" ;;
    taxreport) empty_book="$SRC/doc/examples/taxreport.gnucash" ;;
    esac
    if [ ! -f "$empty_book" ]; then
        bad "empty-state book not found at $empty_book"
        index=$((index + 1))
        continue
    fi
    echo "-- $(basename "$empty_book")"
    start_server "$empty_book"
    for report_slug in receivable payable; do
        if [ "$report_slug" = receivable ]; then
            REPORT="Receivable%20Aging"
            LABEL="Receivable Aging"
        else
            REPORT="Payable%20Aging"
            LABEL="Payable Aging"
        fi
        fetch "empty-$index-$report_slug-native" "/report/$REPORT?end=$AS_OF"
        preview "empty-$index-$report_slug-spec" \
            "$TMP/empty-$index-$report_slug.json"
        [ -f "$TMP/empty-$index-$report_slug-native.html" ] && compare \
            "$LABEL on $(basename "$empty_book")" \
            "$TMP/empty-$index-$report_slug-native.html" \
            "$TMP/empty-$index-$report_slug-spec.html"
    done
    # sample1 has a vendor bill, so its Payable Aging is a table; every other
    # fixture/report pair here is the explanation instead.
    case "$(basename "$empty_book")" in
    sample1.gnucash) page_shape "$TMP/empty-$index-receivable-native.html" \
                         empty "sample1 Receivable Aging is the empty page" ;;
    *) for report_slug in receivable payable; do
           page_shape "$TMP/empty-$index-$report_slug-native.html" empty \
               "$(basename "$empty_book") $report_slug Aging is the empty page"
       done ;;
    esac
    if [ "$(basename "$empty_book")" = sample1.gnucash ]; then
        page_shape "$TMP/empty-$index-payable-native.html" table \
            "sample1 Payable Aging has the vendor bill's row"
    fi
    stop_server
    index=$((index + 1))
done

if [ "$FAILS" -ne 0 ]; then
    echo "$FAILS check(s) failed"
    exit 1
fi
echo "invoices specs match the native aging reports"
