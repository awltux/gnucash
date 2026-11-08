#!/bin/bash
# Report parity: the Scheme Trial Balance versus the native one.
#
# Usage: report-parity.sh <guile-build> <native-build> <source-dir> [book]
#
# <guile-build> must contain bin/gnucash-cli (a WITH_GNUCASH build); override
# the binary with GNC_CLI=/path/to/gnucash-cli.  <native-build> contains
# gnc-webd.  Both run against the same fixture book, and the extracted rows
# (account name, debit, credit) are compared as numbers, so currency symbols,
# thousands separators and the table markup itself do not count as a diff.
#
# Exit status: 0 when every row and both totals match, 1 otherwise.
set -u

GUILE_BUILD=${1:?usage: report-parity.sh <guile-build> <native-build> <source-dir> [book]}
NATIVE_BUILD=${2:?usage: report-parity.sh <guile-build> <native-build> <source-dir> [book]}
SRC=${3:?usage: report-parity.sh <guile-build> <native-build> <source-dir> [book]}
BOOK=${4:-$SRC/libgnucash/backend/xml/test/test-files/load-save/sample1.gnucash}

CLI=${GNC_CLI:-$GUILE_BUILD/bin/gnucash-cli}
WEBDBIN="$NATIVE_BUILD/bin/gnc-webd"
BACKEND="$NATIVE_BUILD/lib/gnucash/libgncmod-backend-xml.so"

fail() { echo "FAIL: $*" >&2; exit 1; }

[ -x "$CLI" ] || fail "no gnucash-cli at $CLI (need a WITH_GNUCASH Guile build, or set GNC_CLI)"
[ -x "$WEBDBIN" ] || fail "gnc-webd was not built at $WEBDBIN"
[ -f "$BOOK" ] || fail "fixture book not found at $BOOK"

TMP=$(mktemp -d)
SERVER_PID=
trap 'kill "$SERVER_PID" 2>/dev/null; rm -rf "$TMP"' EXIT

# ------------------------------------------------------------------ oracle
SCHEMAS="$GUILE_BUILD/share/glib-2.0/schemas"
[ -f "$SCHEMAS/gschemas.compiled" ] || SCHEMAS=""
CCACHE=$(ls -d "$GUILE_BUILD"/lib/*/guile/3.0/site-ccache 2>/dev/null | head -1)

echo "oracle: $CLI"
env GNC_UNINSTALLED=YES GNC_BUILDDIR="$GUILE_BUILD" \
    ${SCHEMAS:+GSETTINGS_SCHEMA_DIR="$SCHEMAS"} \
    LD_LIBRARY_PATH="$GUILE_BUILD/lib:$GUILE_BUILD/lib/gnucash" \
    GNC_MODULE_PATH="$GUILE_BUILD/lib/gnucash" \
    GUILE_LOAD_PATH="$GUILE_BUILD/share/guile/site/3.0" \
    GUILE_LOAD_COMPILED_PATH="${CCACHE:-}" \
    "$CLI" "$BOOK" -R run --name "Trial Balance" \
    --output-file "$TMP/scheme.html" > "$TMP/scheme.log" 2>&1 \
    || { cat "$TMP/scheme.log" >&2; fail "the Scheme report failed"; }
[ -s "$TMP/scheme.html" ] || fail "the Scheme report is empty"

# ------------------------------------------------------------------ native
NATIVE_SCHEMAS="$NATIVE_BUILD/share/glib-2.0/schemas"
if [ ! -f "$NATIVE_SCHEMAS/gschemas.compiled" ]; then
    # Reuse the oracle build's schemas so both reports resolve the same
    # preference defaults; the native build may not have compiled schemas.
    NATIVE_SCHEMAS="$GUILE_BUILD/share/glib-2.0/schemas"
fi
[ -f "$NATIVE_SCHEMAS/gschemas.compiled" ] || NATIVE_SCHEMAS=""
NATIVE_CCACHE=$(ls -d "$NATIVE_BUILD"/lib/*/guile/3.0/site-ccache 2>/dev/null | head -1)

BACKEND_ARGS=()
[ -f "$BACKEND" ] && BACKEND_ARGS=(--backend "$BACKEND")

echo "native: $WEBDBIN"
env GNC_UNINSTALLED=YES GNC_BUILDDIR="$NATIVE_BUILD" \
    ${NATIVE_SCHEMAS:+GSETTINGS_SCHEMA_DIR="$NATIVE_SCHEMAS"} \
    LD_LIBRARY_PATH="$NATIVE_BUILD/lib:$NATIVE_BUILD/lib/gnucash" \
    GNC_MODULE_PATH="$NATIVE_BUILD/lib/gnucash" \
    GUILE_LOAD_PATH="$NATIVE_BUILD/share/guile/site/3.0" \
    GUILE_LOAD_COMPILED_PATH="${NATIVE_CCACHE:-}" \
    GNC_WEBUI_DIR="$SRC/web/ui" \
    "$WEBDBIN" "${BACKEND_ARGS[@]}" "$BOOK" > "$TMP/server.log" 2>&1 &
SERVER_PID=$!

URL=""
for _ in $(seq 1 150); do
    URL=$(grep -oE 'http://127\.0\.0\.1:[0-9]+/[0-9a-f]+/' "$TMP/server.log" | head -1)
    [ -n "$URL" ] && break
    kill -0 "$SERVER_PID" 2>/dev/null || break
    sleep 0.2
done
[ -n "$URL" ] || { cat "$TMP/server.log" >&2; fail "gnc-webd did not start"; }

curl -fsS "${URL%/}/report/Trial%20Balance" -o "$TMP/native.html" \
    || { cat "$TMP/server.log" >&2; fail "the native report request failed"; }
[ -s "$TMP/native.html" ] || fail "the native report is empty"

# ----------------------------------------------------------------- compare
python3 - "$TMP/scheme.html" "$TMP/native.html" <<'PY'
import re
import sys
from decimal import Decimal, InvalidOperation
from html.parser import HTMLParser


class TableParser(HTMLParser):
    """Pull (label, column 1, column 2) out of the report table.

    Both renderers use the Scheme cell classes -- text-cell for the account
    name, number-cell / total-number-cell for amounts -- so one parser reads
    either document.
    """

    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.rows = []
        self.row = None
        self.cell = None
        self.cell_class = ""

    def handle_starttag(self, tag, attrs):
        attrs = dict(attrs)
        if tag == "tr":
            self.row = ([], [])
        elif tag in ("td", "th") and self.row is not None:
            self.cell = []
            self.cell_class = attrs.get("class", "")

    def handle_endtag(self, tag):
        if tag in ("td", "th") and self.row is not None and self.cell is not None:
            text = " ".join("".join(self.cell).split())
            self.row[0].append(text)
            self.row[1].append(self.cell_class)
            self.cell = None
            self.cell_class = ""
        elif tag == "tr" and self.row is not None:
            cells, classes = self.row
            if cells:
                self.rows.append((cells, classes))
            self.row = None

    def handle_data(self, data):
        if self.cell is not None:
            self.cell.append(data)


def parse(path):
    parser = TableParser()
    with open(path, encoding="utf-8", errors="replace") as handle:
        parser.feed(handle.read())

    out = []
    for cells, classes in parser.rows:
        if "total-label-cell" in classes[0] or "total-number-cell" in classes:
            numbers = [text for text, klass in zip(cells, classes)
                       if "total-number-cell" in klass]
            out.append(("__TOTAL__",
                        numbers[0] if len(numbers) > 0 else "",
                        numbers[1] if len(numbers) > 1 else ""))
            continue
        if "text-cell" not in classes[0]:
            continue
        debit = cells[1] if len(cells) > 1 else ""
        credit = cells[2] if len(cells) > 2 else ""
        out.append((cells[0], debit, credit))
    return out


def amount(text):
    """A comparable number, independent of currency formatting."""
    if not text:
        return Decimal(0)
    text = text.replace("\u00a0", " ")
    negative = text.strip().startswith("(") and text.strip().endswith(")")
    text = re.sub(r"[^0-9.,\-]", "", text)
    if text in ("", "-"):
        return Decimal(0)
    try:
        value = Decimal(text.replace(",", ""))
    except InvalidOperation:
        raise SystemExit(f"cannot parse amount {text!r}")
    return -value if negative else value


def key(row):
    return (row[0], amount(row[1]), amount(row[2]))


scheme = [key(row) for row in parse(sys.argv[1])]
native = [key(row) for row in parse(sys.argv[2])]

print(f"scheme rows: {len(scheme)}")
print(f"native rows: {len(native)}")

if scheme == native:
    print(f"MATCH: {len(scheme)} rows (account, debit, credit)")
    sys.exit(0)

print(f"DIFFER: scheme {len(scheme)} rows, native {len(native)} rows")
differing = 0
for index in range(max(len(scheme), len(native))):
    left = scheme[index] if index < len(scheme) else None
    right = native[index] if index < len(native) else None
    if left == right:
        continue
    differing += 1
    if differing <= 20:
        print(f"  row {index}: scheme={left!r} native={right!r}")
print(f"{differing} differing row(s)")
sys.exit(1)
PY
status=$?
[ "$status" = 0 ] || fail "the native report does not match the Scheme report"
echo "report parity passed"
