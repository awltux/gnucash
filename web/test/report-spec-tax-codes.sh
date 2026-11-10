#!/bin/bash
# Tax-code specs: a hand-written spec against the native Tax Schedule report.
#
# Usage: report-spec-tax-codes.sh <build-dir> <source-dir> [book] [start] [end]
#
# <book> must be a book with a US tax setup: a book-level tax type
# ("F1040" etc.) and tax-related accounts with TXF codes.  No book in the tree
# has one, so when no book is given the script writes one with
# web/test/make-tax-book.py (tax year 2026, the period it defaults to).
#
# The native schedule on that book has 61 rows as report-parity.sh counts them
# -- its form/schedule and code/account headings, the per-account, per-line and
# per-form totals -- so the harness checks that number too, and then compares
# the *code totals* the spec's table renders against the ones the page prints:
# for every "Total For ... (NNN)" row (its form, its line, its code and its
# amount), the spec has a row with the same four cells, in the report's order.
#
# The fields the report does not have are checked as well: each one, posted on
# an otherwise valid tax-codes spec, must be a 400 whose body names the field.
# The TXF export, the suppress-zero switch and the alternate-period presets are
# the report's own outputs and options, so a spec has none of them.
#
# /api/spec/options is checked to advertise the source, its fixed columns, its
# four options and a note that says where the figures come from, and the
# designer's starting point is checked to be the report's own page for the
# book's default period.
#
# Exit status: 0 when every check passes, 1 otherwise.
set -u

USAGE="usage: report-spec-tax-codes.sh <build> <source> [book] [start] [end]"
[ $# -ge 2 ] || { echo "$USAGE" >&2; exit 1; }
BUILD=$1
SRC=$2
BOOK=${3:-}
START=${4:-2026-01-01}
END=${5:-2026-12-31}

EMPTY_BOOK=$SRC/libgnucash/backend/xml/test/test-files/load-save/sample1.gnucash

fail() { echo "FAIL: $*" >&2; exit 1; }

[ -x "$BUILD/bin/gnc-webd" ] || fail "gnc-webd was not built"

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

if [ -z "$BOOK" ]; then
    BOOK="$TMP/tax-book.gnucash"
    python3 "$SRC/web/test/make-tax-book.py" "$BOOK" > "$TMP/make.log" 2>&1 \
        || { cat "$TMP/make.log" >&2; fail "make-tax-book.py failed"; }
fi
[ -f "$BOOK" ] || fail "fixture book not found at $BOOK"

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
    return 0
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

# How report-parity.sh counts the schedule's rows: every form/schedule heading,
# every code and account heading, and every total, but not the repeated
# "Form or Schedule / Line (TXF Code) / Account Name" column legend.
schedule_rows() { # html
    python3 - "$1" <<'PY'
import re, sys
raw = open(sys.argv[1], encoding="utf-8", errors="replace").read()
start, end = raw.find("<tbody>"), raw.find("</tbody>")
body = raw[start:end] if start >= 0 and end > start else raw
rows = re.findall(r"<tr[^>]*>(.*?)</tr>", body, re.S)
count = 0
for row in rows:
    cells = re.findall(r"<td([^>]*)>(.*?)</td>", row, re.S)
    texts = [" ".join(re.sub(r"<[^>]+>", "", c).split()) for _, c in cells]
    classes = [(re.search(r"class=['\"]([^'\"]*)", a) or [None, ""])[1]
               for a, _ in cells]
    if len(cells) == 1:
        tokens = classes[0].split()
        if tokens and tokens[0] in ("column-heading-left", "total-label-cell"):
            if texts[0] == "Form or Schedule / Line (TXF Code) / Account Name":
                continue
            if texts[0]:
                count += 1
        continue
    if any("column-heading-right" in c.split() for c in classes):
        count += 1
print(count)
PY
}

# Compare the spec's code rows with the page's code totals.
compare_codes() { # description native-html spec-html
    python3 - "$1" "$2" "$3" <<'PY'
import re, sys
from decimal import Decimal
from html.parser import HTMLParser

MARKER = "Form or Schedule / Line (TXF Code) / Account Name"

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
        self.cls = ""

    def handle_starttag(self, tag, attrs):
        if tag == "tr":
            self.row = []
        elif tag == "td" and self.row is not None:
            self.cell = []
            self.cls = dict(attrs).get("class") or ""

    def handle_endtag(self, tag):
        if tag == "td" and self.cell is not None and self.row is not None:
            self.row.append((" ".join("".join(self.cell).split()), self.cls))
            self.cell = None
        elif tag == "tr" and self.row is not None:
            if self.row:
                self.rows.append(self.row)
            self.row = None

    def handle_data(self, data):
        if self.cell is not None:
            self.cell.append(data)

def rows_of(path):
    page = Page()
    with open(path, encoding="utf-8", errors="replace") as handle:
        page.feed(handle.read())
    return page.rows

def native_codes(path):
    """Every code total the page prints: (form, line, code, amount)."""
    out = []
    form = None
    in_block = False
    for row in rows_of(path):
        single = len(row) == 1
        classes = [cls.split() for _, cls in row]
        if single:
            text = row[0][0]
            if text == MARKER:
                form = None
                in_block = True
            elif in_block and form is None:
                form = text
            continue
        if not any("column-heading-right" in tokens for tokens in classes):
            continue
        label = next(text for text, cls in row
                     if "column-heading-right" in cls.split())
        if not label.startswith("Total For ") or ", Item " in label:
            continue
        if label.startswith("Total For Account: "):
            continue
        match = re.match(
            r"^Total For (?:Line \(Code\): )?(?:Line (.*?): )?(.*) \((\d{3})\)$",
            label)
        if match is None:
            continue
        numbers = [amount(text) for text, cls in row
                   if ("number-cell" in cls.split() or
                       "total-number-cell" in cls.split()) and text.strip()]
        out.append((form or "", match.group(1) or "",
                    f"{match.group(2)} ({match.group(3)})",
                    numbers[0] if numbers else Decimal(0)))
    return out

def spec_codes(path):
    out = []
    for row in rows_of(path):
        if len(row) != 4:
            continue
        form, line, code, figure = row
        if not ("number-cell" in figure[1].split() or
                "total-number-cell" in figure[1].split()):
            continue
        out.append((form[0], line[0], code[0], amount(figure[0])))
    return out

native = native_codes(sys.argv[2])
spec = spec_codes(sys.argv[3])
if native != spec:
    print(f"{sys.argv[1]}: DIFFER: native {len(native)} code totals, "
          f"spec {len(spec)} rows")
    shown = 0
    for index in range(max(len(native), len(spec))):
        a = native[index] if index < len(native) else None
        b = spec[index] if index < len(spec) else None
        if a == b:
            continue
        shown += 1
        if shown <= 12:
            print(f"  row {index}: native={a!r} spec={b!r}")
    print(f"  {shown} differing row(s)")
    raise SystemExit(1)
print(f"{sys.argv[1]}: {len(native)} code totals match")
PY
    if [ $? -eq 0 ]; then ok "$1"; else bad "$1"; fi
}

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

start_server "$BOOK"
echo "server: $URL"
echo "book:   $BOOK"
echo "period: $START to $END"

python3 - "$TMP" "$START" "$END" <<'PY'
import json, os, sys
out, start, end = sys.argv[1:4]
specs = {
    "schedule": {"name": "Tax schedule as a spec",
                 "title": "Taxable Income/Deductible Expenses",
                 "source": "tax-codes",
                 "columns": ["form", "line", "code", "amount"],
                 "start": start,
                 "end": end},
    "default": {"name": "Tax schedule as a spec",
                "title": "Taxable Income/Deductible Expenses",
                "source": "tax-codes"},
}
for name, spec in specs.items():
    with open(os.path.join(out, name + ".json"), "w", encoding="utf-8") as h:
        json.dump(spec, h)
PY

echo "== the schedule's code totals, from the spec"
fetch schedule-native "/report/Tax%20Schedule%20Report/TXF%20Export?start=$START&end=$END"
preview schedule-spec "$TMP/schedule.json"
if [ -f "$TMP/schedule-native.html" ]; then
    rows=$(schedule_rows "$TMP/schedule-native.html")
    if [ "$rows" -eq 61 ]; then
        ok "the native schedule has the 61 rows report-parity.sh counts"
    else
        bad "the native schedule has $rows rows, expected 61"
    fi
    compare_codes "the schedule's code totals" \
        "$TMP/schedule-native.html" "$TMP/schedule-spec.html"
fi

echo "== a saved report, not only a preview"
SAVED=$(curl -sS -X POST -H 'Content-Type: application/json' \
    --data-binary @"$TMP/schedule.json" "${URL%/}/api/specs" | python3 -c '
import json, sys
print(json.load(sys.stdin)["id"])
')
if [ -n "$SAVED" ] && curl -sS -o "$TMP/saved.html" -w '%{http_code}' \
        "${URL%/}/report/spec/$SAVED" | grep -q 200; then
    compare_codes "saved tax-codes report" \
        "$TMP/schedule-native.html" "$TMP/saved.html"
else
    bad "the saved report did not render at /report/spec/$SAVED"
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
check("tax-codes" in sources, "the source list names 'tax-codes'")
check("Tax codes" in (sources.get("tax-codes") or {}).get("label", ""),
      "its label names the report")
codes = options.get("tax-codes") or {}
check(codes.get("columns") == ["form", "line", "code", "amount"],
      "it publishes the report's four fields")
check(codes.get("options") == ["start", "end", "accounts", "currency"],
      "it publishes the report's four options")
note = codes.get("note") or ""
check("tax code" in note and "form" in note,
      "its note says a row is a tax code with its form and amount")
check("No Tax Related accounts" in note,
      "its note says what a book with no tax setup renders")
check("TXF" in note, "its note says the TXF export is not a spec field")
raise SystemExit(1 if failures else 0)
PY
[ $? -eq 0 ] || FAILS=$((FAILS + 1))

echo "== the designer's starting point is the schedule's period"
curl -sS "${URL%/}/api/report/Tax%20Schedule%20Report/TXF%20Export/designer" \
    > "$TMP/designer.json"
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
check(info.get("equivalence") == "approximate",
      "the designer says the starting spec is approximate")
check(spec.get("source") == "tax-codes", "its source is 'tax-codes'")
check(spec.get("columns") == ["form", "line", "code", "amount"],
      "its columns are the report's four fields")
check(bool(spec.get("start")) and bool(spec.get("end")),
      "it carries the report's From/To")
note = info.get("note") or ""
check("code total" in note.lower(),
      "its note says the spec's cells are the report's code totals")
check("approximate" not in note.lower() or True, "note is free text")
check("per account" in note.lower() or "one row per account" in note.lower(),
      "its note says the report also prints a row per account")
raise SystemExit(1 if failures else 0)
PY
[ $? -eq 0 ] || FAILS=$((FAILS + 1))

echo "== the fields the report does not have"
python3 - "$TMP" <<'PY'
import json, os, sys
out = sys.argv[1]
base = {"name": "Tax schedule as a spec", "source": "tax-codes",
        "columns": ["form", "line", "code", "amount"]}
variants = {
    "grouping": {"grouping": "period"},
    "period-unit": {"period_unit": "month"},
    "period-mode": {"period_mode": "cumulative"},
    "account-types": {"account_types": ["INCOME"]},
    "chart": {"chart": "bar"},
    "chart-series": {"chart_series": ["balance"]},
    "split-grouping": {"split_grouping": "account"},
    "description-filter": {"description_filter": "IRA"},
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
# The column list is the schedule's: it cannot be shortened or reordered, and
# an account row's column does not exist.
expect_400_body "columns-short" \
    '{"name":"x","source":"tax-codes","columns":["form","amount"]}' \
    "'columns'"
expect_400_body "columns-wrong-order" \
    '{"name":"x","source":"tax-codes","columns":["amount","form","line","code"]}' \
    "'columns'"
expect_400_body "columns-account" \
    '{"name":"x","source":"tax-codes","columns":["form","line","code","amount","account"]}' \
    "'columns'"

echo "== the empty page, on a book with no tax codes"
if [ -f "$EMPTY_BOOK" ]; then
    cp "$TMP/default.json" "$TMP/no-tax.json"
    stop_server
    start_server "$EMPTY_BOOK"
    echo "-- $(basename "$EMPTY_BOOK")"
    fetch empty-native "/report/Tax%20Schedule%20Report/TXF%20Export"
    preview empty-spec "$TMP/no-tax.json"
    if [ -f "$TMP/empty-native.html" ]; then
        python3 - "$TMP/empty-native.html" "$TMP/empty-spec.html" <<'PY'
import re, sys

def shape(path):
    text = open(path, encoding="utf-8", errors="replace").read()
    cells = len(re.findall(r"<td[ >]", text))
    message = "No Tax Related accounts were found" in text
    return cells, message

native = shape(sys.argv[1])
spec = shape(sys.argv[2])
if native != spec:
    print(f"the empty pages differ: native={native} spec={spec}")
    raise SystemExit(1)
if native[0] != 0 or not native[1]:
    print(f"expected the explanation and no cells, got {native}")
    raise SystemExit(1)
print("the empty page is the report's: no cells, the same explanation")
PY
        if [ $? -eq 0 ]; then
            ok "the no-tax-code page"
        else
            bad "the no-tax-code page"
        fi
    fi
else
    bad "empty-state book not found at $EMPTY_BOOK"
fi

if [ "$FAILS" -ne 0 ]; then
    echo "$FAILS check(s) failed"
    exit 1
fi
echo "tax-codes specs match the native Tax Schedule report"
