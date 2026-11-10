#!/bin/bash
# Document specs: a saved document against the /report/<document> route.
#
# Usage: report-spec-documents.sh <build-dir> <source-dir> [book]
#
# <book> is a book with invoices: web/test/make-invoice-book.py writes one
# (INV-1001 for a customer, BILL-2001 for a vendor, a credit note and an
# unposted document) when no book is given.
#
# A document is not a table, so a document spec is not a table spec: it holds
# the document report's name ('document'), the invoice to print ('invoice') and
# that report's own print options ('document_options').  This harness proves
# the claim the design makes -- that the spec renders through the document
# renderer and not through a table -- by comparing the preview's bytes with the
# route's bytes for the same document, invoice and options:
#
#   * every document layout (Printable, Easy, Fancy, Receipt, Tax Invoice and
#     its Australian variant) on the same invoice;
#   * a document spec that sets print options (custom title, no date column,
#     a different picture slot) against the same options on the route;
#   * a spec with no invoice against the route with no invoice, which is the
#     report's "no invoice selected" page.
#
# The fields a document does not have are checked as well: each one, posted on
# an otherwise valid document spec, must be a 400 whose body names the field.
# A print option the report does not publish, and an option of the wrong type,
# are refused too, since the document's option surface is the report's.
#
# Exit status: 0 when every check passes, 1 otherwise.
set -u

USAGE="usage: report-spec-documents.sh <build> <source> [book]"
[ $# -ge 2 ] || { echo "$USAGE" >&2; exit 1; }
BUILD=$1
SRC=$2
BOOK=${3:-}

EMPTY_BOOK=$SRC/libgnucash/backend/xml/test/test-files/load-save/sample1.gnucash
INVOICE=INV-1001

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
    BOOK="$TMP/invoice-book.gnucash"
    python3 "$SRC/web/test/make-invoice-book.py" "$BOOK" > "$TMP/make.log" 2>&1 \
        || { cat "$TMP/make.log" >&2; fail "make-invoice-book.py failed"; }
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

# The claim: the spec's document is the route's document, byte for byte.
identical() { # description native-html spec-html
    if cmp -s "$2" "$3"; then
        ok "$1: byte-identical ($(wc -c < "$2") bytes)"
    else
        bad "$1: the documents differ"
        diff <(sed 's/></>\n</g' "$2") <(sed 's/></>\n</g' "$3") | head -8
    fi
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

python3 - "$TMP" "$INVOICE" <<'PY'
import json, os, sys
out, invoice = sys.argv[1:3]
specs = {}
for report in ["Printable Invoice", "Easy Invoice", "Fancy Invoice", "Receipt",
               "Tax Invoice", "Australian Tax Invoice"]:
    key = report.lower().replace(" ", "-")
    specs[key] = {"name": report + " as a document", "title": report,
                  "source": "document", "document": report,
                  "invoice": invoice}
specs["options"] = {
    "name": "Printable Invoice with options", "title": "Printable Invoice",
    "source": "document", "document": "Printable Invoice",
    "invoice": invoice,
    "document_options": {"title": "Statement of Account",
                         "show_date": "false",
                         "show_company_contact": "true"},
}
specs["no-invoice"] = {"name": "No invoice selected", "title": "Receipt",
                       "source": "document", "document": "Receipt"}
for name, spec in specs.items():
    with open(os.path.join(out, name + ".json"), "w", encoding="utf-8") as h:
        json.dump(spec, h)
PY

echo "== every layout prints the route's document"
for report in "Printable Invoice" "Easy Invoice" "Fancy Invoice" "Receipt" \
              "Tax Invoice" "Australian Tax Invoice"
do
    key=$(echo "$report" | tr 'A-Z ' 'a-z-')
    encoded=$(python3 -c 'import sys, urllib.parse; print(urllib.parse.quote(sys.argv[1]))' "$report")
    fetch "$key-native" "/report/$encoded?invoice=$INVOICE"
    preview "$key-spec" "$TMP/$key.json"
    [ -f "$TMP/$key-native.html" ] && \
        identical "$report" "$TMP/$key-native.html" "$TMP/$key-spec.html"
done

echo "== the print options are the report's own"
fetch options-native "/report/Printable%20Invoice?invoice=$INVOICE&title=Statement%20of%20Account&show_date=false&show_company_contact=true"
preview options-spec "$TMP/options.json"
[ -f "$TMP/options-native.html" ] && identical "Printable Invoice with options" \
    "$TMP/options-native.html" "$TMP/options-spec.html"

echo "== no invoice selected is the report's warning page"
fetch none-native "/report/Receipt"
preview none-spec "$TMP/no-invoice.json"
[ -f "$TMP/none-native.html" ] && identical "no invoice selected" \
    "$TMP/none-native.html" "$TMP/none-spec.html"

echo "== a saved document, not only a preview"
SAVED=$(curl -sS -X POST -H 'Content-Type: application/json' \
    --data-binary @"$TMP/printable-invoice.json" "${URL%/}/api/specs" | python3 -c '
import json, sys
print(json.load(sys.stdin)["id"])
')
if [ -n "$SAVED" ] && curl -sS -o "$TMP/saved.html" -w '%{http_code}' \
        "${URL%/}/report/spec/$SAVED" | grep -q 200; then
    identical "saved document" "$TMP/printable-invoice-native.html" \
        "$TMP/saved.html"
else
    bad "the saved report did not render at /report/spec/$SAVED"
fi
curl -sS "${URL%/}/api/specs/$SAVED" > "$TMP/saved.json"
python3 - "$TMP/saved.json" "$INVOICE" <<'PY'
import json, sys
with open(sys.argv[1], encoding="utf-8") as handle:
    spec = json.load(handle)
failures = 0
def check(condition, message):
    global failures
    if condition:
        print("ok   " + message)
    else:
        failures += 1
        print("FAIL " + message)
check(spec.get("source") == "document", "the store keeps source='document'")
check(spec.get("document") == "Printable Invoice",
      "the store keeps the document's name")
check(spec.get("invoice") == sys.argv[2], "the store keeps the invoice")
check(spec.get("columns") == [], "the store keeps no columns")
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
check("document" in sources, "the source list names 'document'")
document = options.get("document") or {}
names = [entry.get("name") for entry in document.get("documents") or []]
check(names == ["Printable Invoice", "Easy Invoice", "Fancy Invoice",
                "Receipt", "Tax Invoice", "Australian Tax Invoice"],
      "it publishes the six document reports")
check(document.get("options") == ["invoice", "document_options"],
      "it publishes the document's two fields")
printable = next((entry for entry in document.get("documents") or []
                  if entry.get("name") == "Printable Invoice"), {})
published = [option.get("name") for option in printable.get("options") or []]
check("invoice" in published and "show_date" in published,
      "it publishes the document's own print options")
note = document.get("note") or ""
check("not a table" in note, "its note says a document is not a table")
check("document renderer" in note,
      "its note says the spec renders through the document renderer")
raise SystemExit(1 if failures else 0)
PY
[ $? -eq 0 ] || FAILS=$((FAILS + 1))

echo "== the designer's starting point is the document"
curl -sS "${URL%/}/api/report/Printable%20Invoice/designer" > "$TMP/designer.json"
python3 - "$TMP/designer.json" <<'PY'
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
spec = info.get("spec") or {}
check(info.get("equivalence") == "exact",
      "the designer says the starting point is exact")
check(spec.get("source") == "document", "its source is 'document'")
check(spec.get("document") == "Printable Invoice",
      "its document is the report itself")
check(not spec.get("invoice"), "it selects no invoice, which is the warning page")
note = info.get("note") or ""
check("not a table" in note, "its note says a document is not a table")
check("document renderer" in note,
      "its note says it renders through the document renderer")
raise SystemExit(1 if failures else 0)
PY
[ $? -eq 0 ] || FAILS=$((FAILS + 1))

echo "== the fields a document does not have"
python3 - "$TMP" <<'PY'
import json, os, sys
out = sys.argv[1]
base = {"name": "Printable Invoice as a document", "source": "document",
        "document": "Printable Invoice"}
variants = {
    "columns": {"columns": ["account-name", "balance"]},
    "grouping": {"grouping": "period"},
    "period-unit": {"period_unit": "month"},
    "period-mode": {"period_mode": "cumulative"},
    "start": {"start": "2026-01-01"},
    "end": {"end": "2026-12-31"},
    "date-preset": {"date_preset": "this-year"},
    "currency": {"currency": "USD"},
    "accounts": {"accounts": ["00000000000000000000000000000000"]},
    "account-types": {"account_types": ["ASSET"]},
    "chart": {"chart": "bar"},
    "chart-series": {"chart_series": ["balance"]},
    "split-grouping": {"split_grouping": "account"},
    "description-filter": {"description_filter": "x"},
    "invoice-owner": {"invoice_owner": "payable"},
    "invoice-grouping": {"invoice_grouping": "document"},
    "bucket-basis": {"bucket_basis": "posted"},
}
for name, extra in variants.items():
    spec = dict(base)
    spec.update(extra)
    with open(os.path.join(out, "bad-" + name + ".json"), "w",
              encoding="utf-8") as h:
        json.dump(spec, h)
PY
for pair in \
    "columns:'columns'" "grouping:'grouping'" "period-unit:'period_unit'" \
    "period-mode:'period_mode'" "start:'start'" "end:'end'" \
    "date-preset:'date_preset'" "currency:'currency'" "accounts:'accounts'" \
    "account-types:'account_types'" "chart:'chart'" \
    "chart-series:'chart_series'" "split-grouping:'split_grouping'" \
    "description-filter:'description_filter'" \
    "invoice-owner:'invoice_owner'" "invoice-grouping:'invoice_grouping'" \
    "bucket-basis:'bucket_basis'"
do
    slug=${pair%%:*}
    field=${pair#*:}
    expect_400 "$slug" "$TMP/bad-$slug.json" "$field"
done

echo "== the document's own option surface is the report's"
expect_400_body "an option the report does not publish" \
    '{"name":"x","source":"document","document":"Printable Invoice","document_options":{"show_unicorns":"true"}}' \
    "show_unicorns"
expect_400_body "a boolean option given a word" \
    '{"name":"x","source":"document","document":"Printable Invoice","document_options":{"show_date":"sometimes"}}' \
    "show_date"
expect_400_body "an unknown document" \
    '{"name":"x","source":"document","document":"Profit & Loss"}' \
    "unknown document"
expect_400_body "a document spec with no document" \
    '{"name":"x","source":"document"}' \
    "'document'"
expect_400_body "an unknown invoice" \
    '{"name":"x","source":"document","document":"Receipt","invoice":"NO-SUCH"}' \
    "unknown invoice"
expect_400_body "the invoice as a print option" \
    '{"name":"x","source":"document","document":"Receipt","document_options":{"invoice":"INV-1001"}}' \
    "'invoice' field"

stop_server

if [ "$FAILS" -ne 0 ]; then
    echo "$FAILS check(s) failed"
    exit 1
fi
echo "document specs print the document routes' own documents"
