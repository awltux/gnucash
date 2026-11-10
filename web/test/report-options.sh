#!/bin/bash
# The report option surface: descriptors, validation and application.
#
# Usage: report-options.sh <build-dir> <source-dir> [book]
#
# Starts gnc-webd against the book and checks the stage-3 contract:
#   * /api/report/<name>/options describes the options a native report takes,
#     with type, label and default;
#   * /api/reports?options=1 lists the native reports with their options and
#     the plain /api/reports list keeps its array-of-names shape;
#   * an unknown option, a bad date, an unknown GUID, an unknown currency and
#     a conflicting period/date are 4xx responses whose message names the
#     problem;
#   * an end date changes an as-of report's figures, a period preset resolves,
#     and an account GUID selects that account's subtree.
#
# Exit status: 0 when every check passes, 1 otherwise.
set -u

BUILD=${1:?usage: report-options.sh <build-dir> <source-dir> [book]}
SRC=${2:?usage: report-options.sh <build-dir> <source-dir> [book]}
BOOK=${3:-$SRC/libgnucash/backend/xml/test/test-files/load-save/sample1.gnucash}

BACKEND="$BUILD/lib/gnucash/libgncmod-backend-xml.so"
SCHEMAS="$BUILD/share/glib-2.0/schemas"
CCACHE=$(ls -d "$BUILD"/lib/*/guile/3.0/site-ccache 2>/dev/null | head -1)

fail() { echo "FAIL: $*" >&2; exit 1; }

[ -x "$BUILD/bin/gnc-webd" ] || fail "gnc-webd was not built"
[ -f "$BOOK" ] || fail "fixture book not found at $BOOK"
[ -f "$SCHEMAS/gschemas.compiled" ] || SCHEMAS=""

# The Transaction Breakdown Report's account option needs one account with
# transactions before any of its other options can change the page.  Read the
# fixture's "Checking Account" GUID from the book so the test works for any
# book that carries that account (sample1 and the generated books do).
BREAKDOWN_ACCOUNT=$(python3 - "$BOOK" <<'PY'
import re
import sys

raw = open(sys.argv[1], encoding="utf-8", errors="replace").read()
for block in re.findall(r'<gnc:account[^>]*>(.*?)</gnc:account>', raw, re.S):
    if re.search(r'<act:name>Checking Account</act:name>', block):
        match = re.search(r'<act:id[^>]*>([0-9a-fA-F]{32})</act:id>', block)
        if match:
            print(match.group(1))
            break
PY
)

TMP=$(mktemp -d)
BODY="$TMP/body"
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
    curl -sS -o "$BODY" -w '%{http_code}' "${URL%/}$1"
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
    if grep -qF -- "$2" "$BODY"; then
        ok "body names '$2' ($1)"
    else
        bad "body does not name '$2' ($1): $(head -c 300 "$BODY")"
    fi
}

expect_not_body() { # description needle
    if grep -qF -- "$2" "$BODY"; then
        bad "body unexpectedly contains '$2' ($1)"
    else
        ok "body has no '$2' ($1)"
    fi
}

echo "== descriptors"
expect_status 200 "Trial Balance options" "/api/report/Trial%20Balance/options"
expect_body "Trial Balance options" '"report":"Trial Balance"'
expect_body "Trial Balance options" '"name":"start"'
expect_body "Trial Balance options" '"name":"end"'
expect_body "Trial Balance options" '"label":"Date of Report"'
expect_body "Trial Balance options" '"type":"date"'
expect_body "Trial Balance options" '"default":"'
expect_body "Trial Balance options" '"name":"currency"'
expect_body "Trial Balance options" '"name":"period"'
expect_body "Trial Balance options" '"choices":["this-year","last-year","all"]'

expect_status 200 "Transaction Report options" "/api/report/Transaction%20Report/options"
expect_body "Transaction Report options" '"name":"start"'
expect_body "Transaction Report options" '"name":"end"'
expect_body "Transaction Report options" '"name":"accounts"'
expect_body "Transaction Report options" '"type":"account-list"'
expect_body "Transaction Report options" '"default":[]'

expect_status 200 "General Ledger options" "/api/report/General%20Ledger/options"
expect_body "General Ledger options" '"name":"accounts"'

# The entry-listing cluster.  The General Journal has only the register
# display switches (no account or date option: its query is every non-void
# transaction in the book).  The Transaction Breakdown Report has one account,
# a date range, a description filter and a numeric column limit.  The
# Reconciliation Report has an account list, a date range and the reconciled
# date switch.
expect_status 200 "General Journal options" "/api/report/General%20Journal/options"
expect_body "General Journal options" '"name":"show_date"'
expect_body "General Journal options" '"name":"show_running_balance"'
expect_body "General Journal options" '"type":"bool"'
expect_not_body "General Journal has no account selection" '"name":"accounts"'
expect_not_body "General Journal has no date option" '"name":"start"'

expect_status 200 "Transaction Breakdown options" \
    "/api/report/Transaction%20Breakdown%20Report/options"
expect_body "Transaction Breakdown options" '"name":"account"'
expect_body "Transaction Breakdown options" '"type":"account-list"'
expect_body "Transaction Breakdown options" '"name":"description-filter"'
expect_body "Transaction Breakdown options" '"type":"text"'
expect_body "Transaction Breakdown options" '"name":"max-columns"'
expect_body "Transaction Breakdown options" '"type":"number"'
expect_body "Transaction Breakdown options" '"default":10'
expect_not_body "Transaction Breakdown has no account list" '"name":"accounts"'

expect_status 200 "Reconciliation options" \
    "/api/report/Reconciliation%20Report/options"
expect_body "Reconciliation options" '"name":"accounts"'
expect_body "Reconciliation options" '"type":"account-list"'
expect_body "Reconciliation options" '"name":"show_reconciled_date"'
expect_body "Reconciliation options" '"name":"start"'
expect_body "Reconciliation options" '"name":"end"'

expect_status 400 "bad column limit" \
    "/report/Transaction%20Breakdown%20Report?max-columns=zero"
expect_body "bad column limit" "invalid number 'zero' for option 'max-columns'"
expect_status 400 "out-of-range column limit" \
    "/report/Transaction%20Breakdown%20Report?max-columns=101"
expect_body "out-of-range column limit" "option 'max-columns' must be between 1 and 100"
expect_status 400 "unknown account on the breakdown report" \
    "/report/Transaction%20Breakdown%20Report?account=00000000000000000000000000000000"
expect_body "unknown account on the breakdown report" "unknown account GUID"
expect_status 400 "unknown option on the General Journal" \
    "/report/General%20Journal?bogus=1"
expect_body "unknown option on the General Journal" "unknown option 'bogus'"

# The aging reports take one date and no account selection: they always age
# every A/Receivable (or A/Payable) account in the book.
for aging in "Receivable%20Aging" "Payable%20Aging"; do
    expect_status 200 "$aging options" "/api/report/$aging/options"
    expect_body "$aging options" '"label":"To"'
    expect_body "$aging options" '"type":"date"'
    expect_not_body "$aging options has no account selection" '"name":"accounts"'
    expect_not_body "$aging options has no currency" '"name":"currency"'
    expect_not_body "$aging options has no period preset" '"name":"period"'
done

expect_status 400 "accounts on an aging report" \
    "/report/Receivable%20Aging?accounts=all"
expect_body "accounts on an aging report" "does not accept an 'accounts' option"
for aging in "Receivable%20Aging" "Payable%20Aging"; do
    expect_status 400 "period on an aging report" "/report/$aging?period=all"
    expect_body "period on an aging report" "does not accept a 'period' option"
done

# The Investment Lots report takes a start and end date and a currency, and
# covers every stock account in the book, so it has no account selection.  It
# has no period preset either: its end date is also the valuation date, so
# "all dates" cannot mean an unbounded end.
expect_status 200 "Investment Lots options" "/api/report/Investment%20Lots/options"
expect_body "Investment Lots options" '"name":"start"'
expect_body "Investment Lots options" '"label":"Start date"'
expect_body "Investment Lots options" '"label":"End date"'
expect_body "Investment Lots options" '"name":"end"'
expect_body "Investment Lots options" '"type":"date"'
expect_body "Investment Lots options" '"name":"currency"'
expect_not_body "Investment Lots options has no account selection" \
    '"name":"accounts"'
expect_not_body "Investment Lots options has no period preset" \
    '"name":"period"'
expect_status 400 "accounts on the lots report" \
    "/report/Investment%20Lots?accounts=all"
expect_body "accounts on the lots report" \
    "does not accept an 'accounts' option"
expect_status 400 "period on the lots report" \
    "/report/Investment%20Lots?period=all"
expect_body "period on the lots report" "does not accept a 'period' option"

# The two portfolio listings publish the same price source, account selection
# and zero-share switch; the Advanced Portfolio adds the basis method, the
# brokerage-fee policy and its four display switches, and has no start date
# because its one date is the valuation date.
for pf in "Investment Portfolio:1" "Advanced Portfolio:2"; do
    pfname=${pf%:*}
    pflevel=${pf##*:}
    pfenc=$(printf '%s' "$pfname" | sed 's/ /%20/g')
    expect_status 200 "$pfname options" "/api/report/$pfenc/options"
    expect_body "$pfname options" "\"report\":\"$pfname\""
    expect_body "$pfname options" '"name":"end"'
    expect_body "$pfname options" '"label":"Date"'
    expect_body "$pfname options" '"name":"currency"'
    expect_body "$pfname options" '"name":"price-source"'
    expect_body "$pfname options" '"type":"choice"'
    expect_body "$pfname options" \
        '"choices":["pricedb-latest","pricedb-before","pricedb-nearest"]'
    expect_body "$pfname options" '"name":"accounts"'
    expect_body "$pfname options" '"name":"include-zero"'
    expect_not_body "$pfname options has no period preset" '"name":"period"'
    if [ "$pflevel" = "1" ]; then
        # The Investment Portfolio accepts a start date for range symmetry.
        expect_body "$pfname options" '"name":"start"'
        expect_not_body "$pfname options has no basis method" \
            '"name":"basis-method"'
    else
        expect_not_body "$pfname has no start date" '"name":"start"'
        expect_body "$pfname options" '"name":"basis-method"'
        expect_body "$pfname options" \
            '"choices":["average-basis","fifo-basis","filo-basis"]'
        expect_body "$pfname options" '"name":"brokerage-fees"'
        expect_body "$pfname options" \
            '"choices":["include-in-basis","include-in-gain","ignore-brokerage"]'
        expect_body "$pfname options" '"name":"show-symbol"'
    fi
    expect_status 400 "bad price source on the $pfname" \
        "/report/$pfenc?price-source=weighted-average"
    expect_body "bad price source on the $pfname" \
        "invalid price-source 'weighted-average'"
    expect_status 400 "period on the $pfname" "/report/$pfenc?period=all"
    expect_body "period on the $pfname" "does not accept a 'period' option"
    if [ "$pflevel" = "2" ]; then
        expect_status 400 "start on the $pfname" \
            "/report/$pfenc?start=2026-01-01"
        expect_body "start on the $pfname" "does not accept a 'start' option"
        expect_status 400 "bad basis method on the $pfname" \
            "/report/$pfenc?basis-method=average"
        expect_body "bad basis method on the $pfname" \
            "invalid basis-method 'average'"
    fi
done

# The Lot Viewer takes one account, a period and a description filter.  It has
# no currency and no period preset.
expect_status 200 "Lot Viewer options" "/api/report/Lot%20Viewer/options"
expect_body "Lot Viewer options" '"name":"start"'
expect_body "Lot Viewer options" '"name":"end"'
expect_body "Lot Viewer options" '"name":"account"'
expect_body "Lot Viewer options" '"label":"Account"'
expect_body "Lot Viewer options" '"name":"description-filter"'
expect_body "Lot Viewer options" '"type":"text"'
expect_not_body "Lot Viewer options has no 'accounts' list" '"name":"accounts"'
expect_not_body "Lot Viewer options has no currency" '"name":"currency"'
expect_not_body "Lot Viewer options has no period preset" '"name":"period"'
expect_status 400 "accounts on the lot viewer" "/report/Lot%20Viewer?accounts=all"
expect_body "accounts on the lot viewer" "does not accept an 'accounts' option"
expect_status 400 "currency on the lot viewer" "/report/Lot%20Viewer?currency=USD"
expect_body "currency on the lot viewer" "does not accept a 'currency' option"

# The IFRS cost basis report has five account selections: the stock account is
# the route's "accounts", and the other four are their own names.
expect_status 200 "IFRS options" \
    "/api/report/IFRS%20weighted-average%20cost%20basis%20report/options"
expect_body "IFRS options" '"name":"start"'
expect_body "IFRS options" '"name":"end"'
expect_body "IFRS options" '"name":"currency"'
expect_body "IFRS options" '"name":"accounts"'
expect_body "IFRS options" '"label":"Stock Account"'
expect_body "IFRS options" '"name":"proceeds-account"'
expect_body "IFRS options" '"name":"dividend-account"'
expect_body "IFRS options" '"name":"capgains-account"'
expect_body "IFRS options" '"name":"fees-account"'
expect_body "IFRS options" '"name":"fee-action"'
expect_body "IFRS options" '"name":"format-cells"'
expect_not_body "IFRS options has no period preset" '"name":"period"'
expect_status 400 "period on the IFRS report" \
    "/report/IFRS%20weighted-average%20cost%20basis%20report?period=all"
expect_body "period on the IFRS report" "does not accept a 'period' option"
expect_status 400 "unknown option on the IFRS report" \
    "/report/IFRS%20weighted-average%20cost%20basis%20report?basis-method=fifo-basis"
expect_body "unknown option on the IFRS report" \
    "unknown option 'basis-method' for report"

# The cluster's default pages.  Lot Viewer's Scheme account option has no
# default, and a null account makes its split query add no account term at
# all: the default page is the whole book, one column per lot it can reach, it
# is not the "specify report options" warning.  The page is a table when the
# book has transactions in the default period and the engine's "No data"
# warning when it does not; both are the report's own default output.
lv_status=$(fetch "/report/Lot%20Viewer")
if [ "$lv_status" = 200 ] && grep -qE '<table|>No data<' "$BODY"; then
    ok "Lot Viewer defaults render the whole book, or say there is no data"
else
    bad "Lot Viewer defaults: got $lv_status: $(head -c 200 "$BODY")"
fi
expect_status 200 "Lot Viewer defaults (again)" "/report/Lot%20Viewer"
expect_not_body "Lot Viewer defaults do not ask for options" \
    "specify certain report options"

# The IFRS report's five account selections fall back to the engine's "first
# account of the allowed types", so with no options at all it renders the same
# ledger the Scheme report renders on a book that has a stock account, and the
# engine's "specify report options" warning on a book that has none.  Both are
# correct default pages, so the check here is that the default is one of them
# (never a blank page); the stock-account case is compared with the Scheme
# report by report-parity.sh on the generated investment book.
if_status=$(fetch "/report/IFRS%20weighted-average%20cost%20basis%20report")
if [ "$if_status" = 200 ] && \
   grep -qE '<table|specify certain report options' "$BODY"; then
    ok "IFRS defaults render the ledger, or say why this book has no stock account"
else
    bad "IFRS defaults: got $if_status: $(head -c 200 "$BODY")"
fi

# The Budget Report reads a budget object rather than a query over splits, so
# its only honest option is the account selection: its Scheme "dates" are
# budget-period selectors, not calendar dates.  It always uses the book's
# default budget.
expect_status 200 "Budget Report options" "/api/report/Budget%20Report/options"
expect_body "Budget Report options" '"name":"accounts"'
expect_body "Budget Report options" '"type":"account-list"'
expect_not_body "Budget Report options has no date" '"name":"start"'
expect_not_body "Budget Report options has no end date" '"name":"end"'
expect_not_body "Budget Report options has no currency" '"name":"currency"'
expect_not_body "Budget Report options has no period preset" '"name":"period"'
expect_status 400 "period on the budget report" \
    "/report/Budget%20Report?period=all"
expect_body "period on the budget report" "does not accept a 'period' option"
expect_status 400 "currency on the budget report" \
    "/report/Budget%20Report?currency=USD"
expect_body "currency on the budget report" \
    "does not accept a 'currency' option"

# The statement, balance-sheet and flow layouts read the same budget object.
# They honour the account selection and the report currency (the statements
# convert into a report currency), but their Scheme date/period options are
# budget-period selectors, not dates, so the route must reject them.
for entry in \
    "Budget%20Income%20Statement|Budget Income Statement" \
    "Budget%20Profit%20%26%20Loss|Budget Profit & Loss" \
    "Budget%20Balance%20Sheet|Budget Balance Sheet" \
    "Budget%20Flow|Budget Flow"
do
    enc=${entry%%|*}
    name=${entry#*|}
    expect_status 200 "$name options" "/api/report/$enc/options"
    expect_body "$name options" "\"report\":\"$name\""
    expect_body "$name options" '"name":"accounts"'
    expect_body "$name options" '"type":"account-list"'
    expect_body "$name options" '"name":"currency"'
    expect_not_body "$name options has no date" '"name":"start"'
    expect_not_body "$name options has no end date" '"name":"end"'
    expect_not_body "$name options has no period preset" '"name":"period"'
    expect_status 400 "start on the $name" "/report/$enc?start=2026-01-01"
    expect_body "start on the $name" "does not accept a 'start' option"
    expect_status 400 "end on the $name" "/report/$enc?end=2026-12-31"
    expect_body "end on the $name" "does not accept a 'end' option"
    expect_status 400 "period on the $name" "/report/$enc?period=all"
    expect_body "period on the $name" "does not accept a 'period' option"
done

expect_status 404 "options for an unknown report" "/api/report/No%20Such%20Report/options"

# The invoice report's selector is a document id, not a date or an account.
# It advertises the invoice itself, a custom title and the document's display
# switches.  The fixture book has one invoice, "yoyo1" (a vendor bill), which
# is enough to prove the selector, the title and every switch change the
# document.
echo "== the invoice document"
expect_status 200 "Printable Invoice options" \
    "/api/report/Printable%20Invoice/options"
expect_body "Printable Invoice options" '"report":"Printable Invoice"'
expect_body "Printable Invoice options" '"name":"invoice"'
expect_body "Printable Invoice options" '"type":"invoice"'
expect_body "Printable Invoice options" '"name":"title"'
expect_body "Printable Invoice options" '"type":"text"'
expect_body "Printable Invoice options" '"name":"show_date"'
expect_body "Printable Invoice options" '"type":"bool"'
expect_body "Printable Invoice options" '"name":"show_tax_amount"'
expect_body "Printable Invoice options" '"default":false'
expect_not_body "Printable Invoice options has no date range" '"name":"start"'
expect_not_body "Printable Invoice options has no account selection" \
    '"name":"accounts"'
expect_not_body "Printable Invoice options has no period preset" \
    '"name":"period"'

fetch "/report/Printable%20Invoice" > /dev/null
expect_body "an unselected invoice warns" "No valid invoice selected"
fetch "/report/Printable%20Invoice?invoice=yoyo1" > /dev/null
expect_body "the selected invoice renders its owner" "Yoyodyne, Inc."
expect_body "the selected invoice renders its entries table" "entries-table"
expect_body "the selected invoice renders a total" "Total Price"
fetch "/report/Printable%20Invoice?invoice=yoyo1&title=Bill%20of%20Sale" \
    > /dev/null
expect_body "a custom title replaces the document kind" "Bill of Sale #yoyo1"
expect_status 400 "unknown invoice" \
    "/report/Printable%20Invoice?invoice=NO-SUCH-INVOICE"
expect_body "unknown invoice" "unknown invoice 'NO-SUCH-INVOICE'"
expect_status 400 "title on another report" \
    "/report/Trial%20Balance?title=Statement"
expect_body "title on another report" "unknown option 'title'"

# The Receipt and the two Tax Invoice variants are documents too.  They all
# take the invoice selector; the tax invoices publish the Elements switches
# they actually use, and the receipt publishes none (its Scheme layout has no
# display boolean).  The generated invoice book is where a customer document
# can be rendered; sample1's single vendor bill is enough to prove the
# selector, the vendor-side Tax Invoice and the receipt's refusal of a bill.
echo "== the other invoice documents"
expect_status 200 "Receipt options" "/api/report/Receipt/options"
expect_body "Receipt options" '"report":"Receipt"'
expect_body "Receipt options" '"name":"invoice"'
expect_body "Receipt options" '"type":"invoice"'
expect_not_body "Receipt options has no title" '"name":"title"'
expect_not_body "Receipt options has no date range" '"name":"start"'
expect_not_body "Receipt options has no account selection" '"name":"accounts"'
expect_not_body "Receipt options has no period preset" '"name":"period"'

for entry in "Tax%20Invoice|Tax Invoice|Invoice" \
             "Australian%20Tax%20Invoice|Australian Tax Invoice|Tax Invoice"
do
    enc=${entry%%|*}
    rest=${entry#*|}
    name=${rest%%|*}
    default_title=${rest#*|}
    expect_status 200 "$name options" "/api/report/$enc/options"
    expect_body "$name options" "\"report\":\"$name\""
    expect_body "$name options" '"name":"invoice"'
    expect_body "$name options" '"name":"title"'
    expect_body "$name options" "\"default\":\"$default_title\""
    for switch in show_date show_tax_rate show_units show_contact \
                  show_address show_invoice_number show_company_name \
                  invoice_number_next_to_title show_job_name \
                  show_job_number show_net_price border_collapse
    do
        expect_body "$name options" "\"name\":\"$switch\""
    done
    expect_not_body "$name options has no date range" '"name":"start"'
    expect_not_body "$name options has no account selection" \
        '"name":"accounts"'
    expect_not_body "$name options has no period preset" '"name":"period"'
    expect_not_body "$name options has no currency" '"name":"currency"'
    expect_status 400 "start on the $name" "/report/$enc?start=2026-01-01"
    expect_body "start on the $name" "does not accept a 'start' option"
    expect_status 400 "accounts on the $name" "/report/$enc?accounts=all"
    expect_body "accounts on the $name" "does not accept an 'accounts' option"
    expect_status 400 "period on the $name" "/report/$enc?period=all"
    expect_body "period on the $name" "does not accept a 'period' option"
    expect_status 400 "unknown option on the $name" "/report/$enc?bogus=1"
    expect_body "unknown option on the $name" "unknown option 'bogus'"
    expect_status 400 "bad boolean on the $name" \
        "/report/$enc?show_date=maybe"
    expect_body "bad boolean on the $name" "invalid boolean 'maybe'"
    expect_status 400 "empty invoice on the $name" "/report/$enc?invoice="
    expect_body "empty invoice on the $name" "empty 'invoice' value"
    expect_status 400 "unknown invoice on the $name" \
        "/report/$enc?invoice=NO-SUCH-INVOICE"
    expect_body "unknown invoice on the $name" \
        "unknown invoice 'NO-SUCH-INVOICE'"
    fetch "/report/$enc" > /dev/null
    expect_body "$name unselected warning" "No invoice has been selected"
    fetch "/report/$enc?invoice=yoyo1" > /dev/null
    expect_body "$name vendor bill renders" "entries"
    expect_body "$name vendor bill renders its owner" "Yoyodyne, Inc."
done

expect_status 400 "title on the Receipt" "/report/Receipt?title=Receipt"
expect_body "title on the Receipt" "unknown option 'title'"
fetch "/report/Receipt?invoice=yoyo1" > /dev/null
expect_body "the Receipt refuses a bill" \
    "designed for customer (sales) invoices only"
fetch "/report/Receipt" > /dev/null
expect_body "an unselected Receipt warns" "No invoice has been selected"

# The Tax Invoice switches that are observable on the fixture book: the
# custom title, the date column, the number-next-to-title row and the border
# switch.  The ones whose data is absent from this book are documented no-ops
# and are covered by the generic loop below.
fetch "/report/Tax%20Invoice?invoice=yoyo1" > /dev/null
cp "$BODY" "$TMP/tax-baseline.html"
fetch "/report/Tax%20Invoice?invoice=yoyo1&title=Statement" > /dev/null
if cmp -s "$TMP/tax-baseline.html" "$BODY"; then
    bad "the Tax Invoice custom title does not change the document"
else
    ok "the Tax Invoice custom title changes the document"
fi
fetch "/report/Tax%20Invoice?invoice=yoyo1&show_date=false" > /dev/null
if grep -q "<th align='center'>Date</th>" "$BODY"; then
    bad "the Tax Invoice show_date=false kept the date column"
else
    ok "the Tax Invoice show_date=false removes the date column"
fi
fetch "/report/Tax%20Invoice?invoice=yoyo1&invoice_number_next_to_title=true" \
    > /dev/null
expect_body "the Tax Invoice number next to the title" "Invoice&nbsp;yoyo1"

# The US tax schedule's own option surface: From/To, the Scheme "Alternate
# Period" choice, the account selection, the two row-changing switches and
# the TXF/report output choice.  sample1 has no tax-related accounts, so the
# figures are exercised on the generated book by report-parity.sh; this block
# pins the descriptors and the 400s.
TAX="Tax%20Schedule%20Report/TXF%20Export"
expect_status 200 "tax schedule options" "/api/report/$TAX/options"
expect_body "tax schedule options" '"report":"Tax Schedule Report/TXF Export"'
expect_body "tax schedule options" '"name":"start"'
expect_body "tax schedule options" '"label":"From"'
expect_body "tax schedule options" '"name":"end"'
expect_body "tax schedule options" '"label":"To"'
expect_body "tax schedule options" '"name":"alternate-period"'
expect_body "tax schedule options" '"type":"choice"'
expect_body "tax schedule options" '"default":"from-to"'
expect_body "tax schedule options" '"choices":["from-to","1st-est","2nd-est","3rd-est","4th-est","last-year","1st-last","2nd-last","3rd-last","4th-last"]'
expect_body "tax schedule options" '"name":"accounts"'
expect_body "tax schedule options" '"type":"account-list"'
expect_body "tax schedule options" '"name":"suppress-zero"'
expect_body "tax schedule options" '"name":"format"'
expect_body "tax schedule options" '"choices":["report","txf"]'
expect_not_body "tax schedule has no period preset" '"name":"period"'
expect_not_body "tax schedule has no currency option" '"name":"currency"'
fetch "/report/$TAX" > /dev/null
expect_body "tax schedule on a book without tax accounts" \
    "No Tax Related accounts"
fetch "/report/$TAX?format=txf" > /dev/null
expect_body "tax schedule TXF header" "V042"

echo "== the report list"
expect_status 200 "plain list" "/api/reports"
expect_body "plain list" '"Trial Balance"'
expect_not_body "plain list stays an array of names" '"report":'
expect_status 200 "list with options" "/api/reports?options=1"
expect_body "list with options" '"report":"Trial Balance"'
expect_body "list with options" '"name":"accounts"'

echo "== rejections name the problem"
expect_status 400 "unknown option" "/report/Trial%20Balance?bogus=1"
expect_body "unknown option" "unknown option 'bogus'"
expect_status 400 "short date" "/report/Income%20Statement?start=2018-1-1"
expect_body "short date" "invalid start date"
expect_status 400 "bad month" "/report/Income%20Statement?start=2018-13-01"
expect_body "bad month" "invalid start date"
expect_status 400 "bad day" "/report/Income%20Statement?end=2018-02-30"
expect_body "bad day" "invalid end date"
# An inverted range must be empty, not a crash: the engine's split iterator
# was undefined behaviour when first > last.  It is not a 400 because the
# options form can send the descriptor's start default with an earlier end,
# and the as-of reports accept a start they ignore.
expect_status 200 "end before the default start" \
    "/report/Income%20Statement?end=2009-12-31"
expect_body "end before the default start" "Income Statement"
expect_status 200 "explicit inverted range" \
    "/report/Income%20Statement?start=2018-01-01&end=2009-12-31"
expect_body "explicit inverted range" "Income Statement"
expect_status 400 "accounts on an as-of report" "/report/Trial%20Balance?accounts=all"
expect_body "accounts on an as-of report" "does not accept an 'accounts' option"
expect_status 400 "malformed GUID" "/report/Transaction%20Report?accounts=deadbeef"
expect_body "malformed GUID" "invalid account GUID 'deadbeef'"
expect_status 400 "unknown GUID" \
    "/report/Transaction%20Report?accounts=00000000000000000000000000000000"
expect_body "unknown GUID" "unknown account GUID '00000000000000000000000000000000'"
expect_status 400 "unknown currency" "/report/Trial%20Balance?currency=XYZ"
expect_body "unknown currency" "unknown currency 'XYZ'"
expect_status 400 "unknown period" "/report/Income%20Statement?period=last-week"
expect_body "unknown period" "unknown period 'last-week'"
expect_status 400 "period and dates conflict" \
    "/report/Income%20Statement?period=all&start=2018-01-01"
expect_body "period and dates conflict" "cannot be combined"
expect_status 400 "accounts=all with GUIDs" \
    "/report/Transaction%20Report?accounts=all&accounts=00000000000000000000000000000000"
expect_body "accounts=all with GUIDs" "cannot be combined with account GUIDs"

# The tax schedule publishes no "period" preset (its Scheme option is the
# different "Alternate Period" choice) and no currency option.
expect_status 400 "period on the tax schedule" "/report/$TAX?period=all"
expect_body "period on the tax schedule" "does not accept a 'period' option"
expect_status 400 "currency on the tax schedule" "/report/$TAX?currency=USD"
expect_body "currency on the tax schedule" "does not accept a 'currency' option"
expect_status 400 "unknown alternate period" \
    "/report/$TAX?alternate-period=bogus"
expect_body "unknown alternate period" "invalid alternate-period 'bogus'"
expect_status 400 "unknown tax output format" "/report/$TAX?format=xml"
expect_body "unknown tax output format" "invalid format 'xml'"
expect_status 400 "bad tax boolean" "/report/$TAX?suppress-zero=maybe"
expect_body "bad tax boolean" "invalid boolean 'maybe'"
expect_status 400 "unknown tax option" "/report/$TAX?bogus=1"
expect_body "unknown tax option" "unknown option 'bogus'"
expect_status 400 "bad GUID on the tax schedule" "/report/$TAX?accounts=deadbeef"
expect_body "bad GUID on the tax schedule" "invalid account GUID 'deadbeef'"

# The owner reports: Customer/Vendor/Employee/Job Report are one option set
# with an owner selector and the history display switches, and Customer
# Summary has its own General/Display page.  The fixture book may carry an
# owner of each kind; when it does, the selector's positive path is checked
# too, otherwise only the descriptors and the 400s are.
echo "== the owner reports"
OWNER_IDS=$(python3 - "$BOOK" <<'PY'
import re
import sys

raw = open(sys.argv[1], encoding="utf-8", errors="replace").read()
for kind, prefix, tag in (("customer", "cust", "GncCustomer"),
                          ("vendor", "vendor", "GncVendor"),
                          ("employee", "employee", "GncEmployee"),
                          ("job", "job", "GncJob")):
    match = re.search(
        r'<gnc:' + tag + r' version="2.0.0">\s*'
        r'<' + prefix + r':guid type="guid">([0-9a-fA-F]{32})</' + prefix +
        r':guid>', raw)
    if match:
        print(f"{kind}={match.group(1)}")
PY
)
owner_id() { printf '%s\n' "$OWNER_IDS" | sed -n "s/^$1=//p"; }
CUSTOMER_GUID=$(owner_id customer)
VENDOR_GUID=$(owner_id vendor)
EMPLOYEE_GUID=$(owner_id employee)
JOB_GUID=$(owner_id job)

for entry in "Customer%20Report|Customer Report|Customer|$CUSTOMER_GUID" \
             "Vendor%20Report|Vendor Report|Vendor|$VENDOR_GUID" \
             "Employee%20Report|Employee Report|Employee|$EMPLOYEE_GUID" \
             "Job%20Report|Job Report|Job|$JOB_GUID"
do
    enc=${entry%%|*}
    rest=${entry#*|}
    name=${rest%%|*}
    rest=${rest#*|}
    page=${rest%%|*}
    guid=${rest#*|}
    expect_status 200 "$name options" "/api/report/$enc/options"
    expect_body "$name options" "\"report\":\"$name\""
    expect_body "$name options" '"name":"owner"'
    expect_body "$name options" '"type":"owner"'
    expect_body "$name options" "\"label\":\"$page\""
    expect_body "$name options" '"name":"start"'
    expect_body "$name options" '"label":"From"'
    expect_body "$name options" '"name":"end"'
    expect_body "$name options" '"label":"To"'
    expect_body "$name options" '"name":"show_balance"'
    expect_body "$name options" '"name":"period"'
    expect_not_body "$name options has no account selection" '"name":"accounts"'
    expect_not_body "$name options has no currency" '"name":"currency"'
    expect_status 400 "empty owner on the $name" "/report/$enc?owner="
    expect_body "empty owner on the $name" "empty 'owner' value"
    expect_status 400 "unknown owner on the $name" \
        "/report/$enc?owner=00000000000000000000000000000000"
    expect_body "unknown owner on the $name" "unknown owner"
    expect_status 400 "accounts on the $name" "/report/$enc?accounts=all"
    expect_body "accounts on the $name" "does not accept an 'accounts' option"
    expect_status 400 "bad boolean on the $name" \
        "/report/$enc?show_balance=maybe"
    expect_body "bad boolean on the $name" "invalid boolean 'maybe'"
    expect_status 400 "unknown option on the $name" "/report/$enc?bogus=1"
    expect_body "unknown option on the $name" "unknown option 'bogus'"
done

# The Job Report has no Job column because its owner is the job.
fetch "/api/report/Job%20Report/options" > /dev/null
expect_not_body "Job Report options has no job column" '"name":"show_job"'
fetch "/api/report/Customer%20Report/options" > /dev/null
expect_body "Customer Report options has a job column" '"name":"show_job"'

if [ -n "$CUSTOMER_GUID" ]; then
    fetch "/report/Customer%20Report?owner=$CUSTOMER_GUID" > /dev/null
    expect_body "the selected customer renders" "Acme Widgets Ltd"
    expect_body "the selected customer renders its history" "Debits"
    expect_body "the selected customer renders the aging buckets" "91+ days"
    fetch "/report/Customer%20Report?owner=$CUSTOMER_GUID&show_date=false" \
        > /dev/null
    expect_not_body "show_date=false removes the date column" "<th>Date</th>"
    fetch "/report/Customer%20Report?owner=$CUSTOMER_GUID&show_balance=false" \
        > /dev/null
    expect_not_body "show_balance=false removes the balance column" \
        "<th>Balance</th>"
fi
for pair in "Vendor%20Report|$VENDOR_GUID" "Employee%20Report|$EMPLOYEE_GUID" \
            "Job%20Report|$JOB_GUID"
do
    enc=${pair%%|*}
    guid=${pair#*|}
    if [ -n "$guid" ]; then
        fetch "/report/$enc?owner=$guid" > /dev/null
        expect_body "the selected ${enc%%%20*} renders a history table" \
            "Period Totals"
    fi
done

# Customer Summary: its own dates and display switches, and no owner.
echo "== the customer summary"
expect_status 200 "Customer Summary options" "/api/report/Customer%20Summary/options"
expect_body "Customer Summary options" '"report":"Customer Summary"'
expect_body "Customer Summary options" '"name":"start"'
expect_body "Customer Summary options" '"label":"From"'
expect_body "Customer Summary options" '"name":"end"'
expect_body "Customer Summary options" '"label":"To"'
expect_body "Customer Summary options" '"name":"show_expense"'
expect_body "Customer Summary options" '"name":"show_zero_lines"'
expect_body "Customer Summary options" '"name":"show_inactive"'
expect_body "Customer Summary options" '"name":"period"'
expect_not_body "Customer Summary has no owner" '"name":"owner"'
expect_not_body "Customer Summary has no account selection" '"name":"accounts"'
expect_not_body "Customer Summary has no currency" '"name":"currency"'
expect_status 400 "owner on the Customer Summary" \
    "/report/Customer%20Summary?owner=00000000000000000000000000000000"
expect_body "owner on the Customer Summary" "unknown option 'owner'"
expect_status 400 "accounts on the Customer Summary" \
    "/report/Customer%20Summary?accounts=all"
expect_body "accounts on the Customer Summary" \
    "does not accept an 'accounts' option"
expect_status 400 "bad boolean on the Customer Summary" \
    "/report/Customer%20Summary?show_expense=maybe"
expect_body "bad boolean on the Customer Summary" "invalid boolean 'maybe'"
# sample1 has no customer on its owner list, and customer-summary.scm's page
# for that case is the sentence -- not a table with headings and a total row.
# The generated owner book and report-parity.sh cover the table's headings,
# rows and total rows.
fetch "/report/Customer%20Summary" > /dev/null
expect_body "the Customer Summary says this book has no customer" \
    "No valid customer found."
expect_not_body "the Customer Summary does not print a table for it" \
    "<table"

echo "== the options apply"
fetch "/report/Trial%20Balance?end=2018-12-31" > /dev/null
TB_2018=$(mktemp)
cp "$BODY" "$TB_2018"
fetch "/report/Trial%20Balance?end=2009-12-31" > /dev/null
if cmp -s "$TB_2018" "$BODY"; then
    bad "Trial Balance end date does not change the document"
else
    ok "Trial Balance end date changes the figures"
fi
rm -f "$TB_2018"

# A start date is accepted on the as-of reports, but only the end date moves
# their figures: the balance sheet with a range equals the balance sheet as
# of the same end date.
fetch "/report/Balance%20Sheet?end=2018-12-31" > /dev/null
BS_END=$(mktemp)
cp "$BODY" "$BS_END"
BS_RANGE_CODE=$(fetch "/report/Balance%20Sheet?start=2009-01-01&end=2018-12-31")
if [ "$BS_RANGE_CODE" = "200" ] && cmp -s "$BS_END" "$BODY"; then
    ok "a start date on the balance sheet is accepted and the end date decides"
else
    bad "balance sheet start+end answered $BS_RANGE_CODE or changed the figures"
fi
rm -f "$BS_END"

fetch "/report/Income%20Statement?period=this-year" > /dev/null
expect_body "period=this-year resolves to a period" "For Period Covering"

fetch "/report/Transaction%20Report?period=all&accounts=all" > /dev/null
expect_body "period=all with accounts=all renders rows" "date-cell"

# The default options of the transaction report are an empty selection, which
# is the Scheme "No accounts selected" warning -- not the whole book.
fetch "/report/Transaction%20Report?start=2000-01-01&end=2030-12-31" > /dev/null
expect_body "empty default selection warns" "No accounts selected"

# A GUID means that account and its subtree.  Pick a parent account from
# /api/accounts: it has no splits of its own on the fixture book, so rows can
# only appear if the subtree was selected.
fetch "/api/accounts" > /dev/null
PARENT_GUID=$(python3 - "$BODY" <<'PY'
import json, sys
with open(sys.argv[1], encoding="utf-8") as handle:
    root = json.load(handle)

def walk(node):
    yield node
    for child in node.get("children", []):
        yield from walk(child)

parents = [a for a in walk(root) if a.get("children")]
print((parents[0] if parents else {}).get("guid", ""))
PY
)
[ -n "$PARENT_GUID" ] || fail "could not find a parent account GUID in the book"

fetch "/report/Transaction%20Report?start=2000-01-01&end=2030-12-31&accounts=$PARENT_GUID" > /dev/null
expect_body "an account GUID selects its subtree" "date-cell"
expect_not_body "a subtree selection is not the empty default" "No accounts selected"

DEFAULT_CURRENCY=$(fetch "/api/report/Trial%20Balance/options" > /dev/null; python3 - "$BODY" <<'PY'
import json, sys
with open(sys.argv[1], encoding="utf-8") as handle:
    data = json.load(handle)
print(next(o["default"] for o in data["options"] if o["name"] == "currency"))
PY
)
CURRENCY_CODE=$(fetch "/report/Trial%20Balance?end=2018-12-31&currency=$DEFAULT_CURRENCY")
if [ "$CURRENCY_CODE" = "200" ]; then
    ok "200 report currency $DEFAULT_CURRENCY"
else
    bad "the report currency $DEFAULT_CURRENCY answered $CURRENCY_CODE"
fi

# The multicolumn statements and the equity statement.  Their descriptors, the
# 400s that name the problem, and the period-column form, which is the part of
# their option surface that is not a plain date or boolean.
for entry in \
    "Balance%20Sheet%20(Multicolumn)|Balance Sheet (Multicolumn)" \
    "Income%20Statement%20(Multicolumn)|Income Statement (Multicolumn)"
do
    enc=${entry%%|*}
    name=${entry#*|}
    fetch "/api/report/$enc/options" > /dev/null
    expect_body "$name publishes its period duration" '"name":"period-duration"'
    expect_body "$name publishes the column order" '"name":"reverse-chrono"'
    expect_body "$name publishes the section switches" '"name":"label-sections"'
    expect_body "$name publishes the zero-account switch" '"name":"show-zb-accts"'
    expect_body "$name publishes the full-name switch" '"name":"account-full-name"'

    expect_status 400 "an unknown period duration on $name" \
        "/report/$enc?period-duration=fortnight"
    expect_body "an unknown period duration on $name" \
        "invalid period-duration 'fortnight'"
    expect_status 400 "accounts on $name" "/report/$enc?accounts=all"
    expect_body "accounts on $name" "does not accept an 'accounts' option"
    expect_status 400 "a bad boolean on $name" "/report/$enc?reverse-chrono=maybe"
    expect_body "a bad boolean on $name" "invalid boolean 'maybe'"

    # The default page is the single-column form: it has no column heading row.
    fetch "/report/$enc" > /dev/null
    expect_not_body "$name default is single-column" ">Date<"
    expect_not_body "$name default is single-column" ">Period<"
    fetch "/report/$enc?period-duration=month&start=2018-01-01&end=2018-12-31" > /dev/null
    expect_body "$name heads its period columns" "12/31/18"
    expect_body "$name prints the 2018 columns" "01/01/18"
done

# The profit & loss's overall-period switch needs more than one period column.
fetch "/report/Income%20Statement%20(Multicolumn)?period-duration=month&start=2018-01-01&end=2018-12-31" > /dev/null
MC_MONTH=$(mktemp)
cp "$BODY" "$MC_MONTH"
fetch "/report/Income%20Statement%20(Multicolumn)?period-duration=month&start=2018-01-01&end=2018-12-31&include-overall-period=true" > /dev/null
if cmp -s "$MC_MONTH" "$BODY"; then
    bad "the overall-period column does not change the profit & loss"
else
    ok "the overall-period column changes the profit & loss"
fi
rm -f "$MC_MONTH"

fetch "/api/report/Equity%20Statement/options" > /dev/null
expect_body "the equity statement publishes its title" '"name":"report-title"'
expect_body "the equity statement publishes a start date" '"name":"start"'
fetch "/report/Equity%20Statement?start=2018-01-01&end=2018-12-31" > /dev/null
expect_body "the equity statement prints its opening capital" "Capital, 01/01/18"
expect_body "the equity statement prints its closing capital" "Capital, 12/31/18"
expect_status 400 "an unknown option on the equity statement" \
    "/report/Equity%20Statement?window=2018"
expect_body "an unknown option on the equity statement" "unknown option 'window'"
fetch "/report/Equity%20Statement?report-title=Statement" > /dev/null
expect_body "the equity statement takes a title" "<h3>Statement</h3>"

# The Cash Flow report classifies the counterpart splits of the selected
# accounts into money in and money out; it is not a balance table, so it has
# no debit/credit columns and no period preset.  The Scheme "Always show
# sub-accounts" switch is not published: the route's account GUIDs already
# select each account's subtree, which is all that switch adds.
expect_status 200 "Cash Flow options" "/api/report/Cash%20Flow/options"
expect_body "Cash Flow options" '"report":"Cash Flow"'
expect_body "Cash Flow options" '"name":"start"'
expect_body "Cash Flow options" '"name":"end"'
expect_body "Cash Flow options" '"name":"currency"'
expect_body "Cash Flow options" '"name":"accounts"'
expect_body "Cash Flow options" '"type":"account-list"'
expect_body "Cash Flow options" '"name":"show-full-names"'
expect_body "Cash Flow options" '"name":"show-selected-accts"'
expect_body "Cash Flow options" '"name":"include-trading"'
expect_body "Cash Flow options" '"name":"display-depth"'
expect_body "Cash Flow options" '"type":"number"'
expect_body "Cash Flow options" '"default":2'
expect_not_body "Cash Flow has no period preset" '"name":"period"'
expect_not_body "Cash Flow does not publish the sub-account switch" \
    '"name":"show-subaccounts"'
expect_status 400 "period on the Cash Flow report" "/report/Cash%20Flow?period=all"
expect_body "period on the Cash Flow report" "does not accept a 'period' option"
expect_status 400 "bad depth on the Cash Flow report" \
    "/report/Cash%20Flow?display-depth=deep"
expect_body "bad depth on the Cash Flow report" \
    "invalid number 'deep' for option 'display-depth'"

# Its date options default to the accounting period, so this book's 2018
# activity is outside the default page: that page is the two section headings
# with zero totals.  With the 2018 period the sample book's single transaction
# has its counterpart in the "money in" section, and each display switch
# changes the page around it.
fetch "/report/Cash%20Flow" > /dev/null
expect_body "Cash Flow default page" "Money into selected accounts comes from"
expect_body "Cash Flow default page" "Money out of selected accounts goes to"
expect_body "Cash Flow default page" "Difference"
fetch "/report/Cash%20Flow?start=2018-01-01&end=2018-12-31" > /dev/null
expect_body "Cash Flow 2018" "Opening Balances"
expect_body "Cash Flow 2018" "Money In"
CF_PAGE=$(mktemp)
cp "$BODY" "$CF_PAGE"
fetch "/report/Cash%20Flow?start=2018-01-01&end=2018-12-31&show-full-names=false" > /dev/null
if cmp -s "$CF_PAGE" "$BODY"; then
    bad "Cash Flow show-full-names=false does not change the page"
else
    ok "Cash Flow show-full-names=false changes the account labels"
fi
fetch "/report/Cash%20Flow?start=2018-01-01&end=2018-12-31&show-selected-accts=false" > /dev/null
if cmp -s "$CF_PAGE" "$BODY"; then
    bad "Cash Flow show-selected-accts=false does not change the page"
else
    ok "Cash Flow show-selected-accts=false drops the account list"
fi
fetch "/report/Cash%20Flow?start=2018-01-01&end=2018-12-31&display-depth=1" > /dev/null
if cmp -s "$CF_PAGE" "$BODY"; then
    bad "Cash Flow display-depth=1 does not change the account list"
else
    ok "Cash Flow display-depth=1 shortens the account list"
fi
rm -f "$CF_PAGE"
# Accepted, and a no-op only because this book has no trading account.
expect_status 200 "include-trading on the Cash Flow report" \
    "/report/Cash%20Flow?start=2018-01-01&end=2018-12-31&include-trading=true"

# The Income and GST Statement is a trep-engine listing with calculated
# columns: one row per transaction and one column per heading.  Its period
# defaults to the previous calendar quarter, not the accounting period.
expect_status 200 "Income and GST Statement options" \
    "/api/report/Income%20and%20GST%20Statement/options"
expect_body "Income and GST Statement options" '"name":"start"'
expect_body "Income and GST Statement options" '"name":"end"'
expect_body "Income and GST Statement options" '"name":"currency"'
expect_body "Income and GST Statement options" '"name":"accounts"'
expect_body "Income and GST Statement options" '"label":"Sales"'
expect_body "Income and GST Statement options" '"name":"purchases"'
expect_body "Income and GST Statement options" '"name":"tax-accounts"'
expect_body "Income and GST Statement options" '"name":"report-format"'
expect_body "Income and GST Statement options" \
    '"choices":["default","au-bas","uk-vat"]'
expect_body "Income and GST Statement options" '"name":"individual-sales"'
expect_body "Income and GST Statement options" '"name":"individual-tax"'
expect_body "Income and GST Statement options" '"name":"gross-balance"'
expect_body "Income and GST Statement options" '"name":"net-balance"'
expect_body "Income and GST Statement options" '"name":"tax-payable"'
expect_not_body "the GST statement has no period preset" '"name":"period"'
expect_status 400 "period on the GST statement" \
    "/report/Income%20and%20GST%20Statement?period=all"
expect_body "period on the GST statement" "does not accept a 'period' option"
expect_status 400 "unknown format on the GST statement" \
    "/report/Income%20and%20GST%20Statement?report-format=bas"
expect_body "unknown format on the GST statement" \
    "invalid report-format 'bas'"

# This book's activity is outside the default quarter, so the default page is
# the engine's "No matching transactions found" warning.  With the 2018 period
# the default format prints the six calculated columns around the book's one
# purchase, and the format and column switches change the page.
fetch "/report/Income%20and%20GST%20Statement" > /dev/null
expect_body "GST default page" "No matching transactions found"
GST_QUERY="start=2018-01-01&end=2018-12-31"
fetch "/report/Income%20and%20GST%20Statement?$GST_QUERY" > /dev/null
expect_body "GST 2018 headings" "Gross Sales"
expect_body "GST 2018 headings" "Net Purchases"
expect_body "GST 2018 row" "Yoyodyne, Inc."
expect_body "GST 2018 total" "Grand Total"
expect_not_body "GST default columns" "Tax payable"
fetch "/report/Income%20and%20GST%20Statement?$GST_QUERY&gross-balance=true&net-balance=true&tax-payable=true" > /dev/null
expect_body "GST optional columns" "Gross Balance"
expect_body "GST optional columns" "Net Balance"
expect_body "GST optional columns" "Tax payable"
fetch "/report/Income%20and%20GST%20Statement?$GST_QUERY&report-format=au-bas" > /dev/null
expect_body "GST AU BAS format" "1A GST on Sales"
expect_body "GST AU BAS format" "1B GST on Purchases"
expect_not_body "GST AU BAS format" "Gross Sales"
fetch "/report/Income%20and%20GST%20Statement?$GST_QUERY&report-format=uk-vat" > /dev/null
expect_body "GST UK VAT format" "Box 1 VAT Sales"
expect_body "GST UK VAT format" "Box 5 VAT Difference"
fetch "/report/Income%20and%20GST%20Statement?$GST_QUERY&individual-tax=true" > /dev/null
expect_not_body "GST individual tax columns" "Tax on Sales"

echo "== every advertised option is effective or rejected"
# The descriptors are the contract.  For every native report and every option
# its descriptor publishes, sending the option must either change the rendered
# document or be refused with a 400 that names it.  The two documented
# exceptions are the as-of start dates, which the descriptors explain.
#
# Currency is not probed for effect: sample1 has only USD, so there is no
# second currency to convert into.  The default currency is accepted above and
# an unknown currency is a 400 below; a second-currency fixture is not part of
# this test, so currency effectiveness is the one gap this loop leaves.
OPTDIR=$(mktemp -d)
fetch "/api/reports?options=1" > /dev/null
cp "$BODY" "$OPTDIR/reports.json"
python3 - "$OPTDIR/reports.json" <<'PY' > "$OPTDIR/reports"
import json, sys
with open(sys.argv[1], encoding="utf-8") as handle:
    for item in json.load(handle):
        print(item["report"])
PY

while IFS= read -r report; do
    enc=$(python3 -c 'import sys,urllib.parse; print(urllib.parse.quote(sys.argv[1]))' "$report")
    code=$(fetch "/api/report/$enc/options")
    if [ "$code" != "200" ]; then
        bad "$report options answered $code"
        continue
    fi
    cp "$BODY" "$OPTDIR/options.json"
    python3 - "$OPTDIR/options.json" <<'PY' > "$OPTDIR/options"
import json, sys
with open(sys.argv[1], encoding="utf-8") as handle:
    for option in json.load(handle)["options"]:
        print("\t".join((option["name"], option["type"],
                         json.dumps(option["default"]))))
PY

    # A real baseline for the reports that need a selection: their date
    # options do nothing while the report is the empty-selection warning.
    # The invoice report is the same case with an invoice instead of accounts:
    # the document needs one selected before any display switch is visible.
    base=""
    if grep -q '"name":"accounts"' "$OPTDIR/options.json"; then
        base="accounts=all"
    elif grep -q '"name":"invoice"' "$OPTDIR/options.json"; then
        base="invoice=yoyo1"
    elif [ "$report" = "Transaction Breakdown Report" ]; then
        # The breakdown report needs one account and a period that contains
        # data before any of its switches can be seen; sample1's Checking
        # Account over 2018 is that selection.
        base="account=$BREAKDOWN_ACCOUNT&start=2018-01-01&end=2018-12-31"
    fi
    case "$report" in
        "Balance Sheet (Multicolumn)"|"Income Statement (Multicolumn)")
            # Their switches act on the column axis, so the baseline is a
            # monthly run over 2018: it has columns to reorder, to hide and to
            # add an overall period to.  The default (single-column) page has
            # one column and would make the column switches look inert.
            base="period-duration=month&start=2018-01-01&end=2018-12-31" ;;
    esac
    fetch "/report/$enc" > /dev/null
    cp "$BODY" "$OPTDIR/no-options.html"
    fetch "/report/$enc${base:+?$base}" > /dev/null
    cp "$BODY" "$OPTDIR/baseline.html"

    # A report whose data is simply absent from this book cannot demonstrate
    # that its options are effective.  The budget reports are that case:
    # sample1 has no budget at all, so both the default and accounts=all
    # render the Scheme engine's "No budgets exist" warning.  The budget data
    # path, including the account selection, is covered by report-parity.sh on
    # the book make-budget-book.py generates.
    case "$report" in
        "Budget Report"|"Budget Income Statement"|"Budget Profit & Loss"|\
        "Budget Balance Sheet"|"Budget Flow")
            if grep -qF "No budgets exist" "$OPTDIR/no-options.html"; then
                ok "$report has no budget in this book; the generated-book parity test covers it"
                continue
            fi ;;
        "Tax Schedule Report/TXF Export")
            # sample1 has no tax-related accounts, so every switch renders the
            # same "No Tax Related accounts" explanation.  The descriptor and
            # 400 checks above cover the option surface; the generated tax book
            # in report-parity.sh covers the effect of the dates and switches.
            if grep -qF "No Tax Related accounts" "$OPTDIR/no-options.html"; then
                ok "$report has no tax accounts in this book; the generated-book parity test covers it"
                continue
            fi ;;
        "Customer Report"|"Vendor Report"|"Employee Report"|"Job Report")
            # The generic probe has no owner to select (sample1 has no
            # customer at all), so every option would be compared against the
            # "No valid owner selected" warning.  The descriptors and 400s are
            # checked above; the owner fixture and report-parity.sh cover the
            # selected-owner effects.
            ok "$report needs an owner selection; the owner fixture and parity test cover it"
            continue ;;
        "Customer Summary")
            # With no customers and (on the default no-schema run) a one-day
            # period, the display switches have nothing to change.  The
            # generated owner book and report-parity.sh cover them.
            ok "$report has no customer activity in this book; the generated owner book covers it"
            continue ;;
        "Cash Flow")
            # Its period defaults to the accounting period, so this book's
            # 2018 activity is outside every generic probe's comparison.  The
            # report's own section above checks the switches on 2018, and
            # report-parity.sh compares the page with the Scheme report.
            ok "$report's period defaults away from this book's activity; its switches are checked above"
            continue ;;
        "Income and GST Statement")
            # Its period defaults to the previous calendar quarter, so the
            # generic probes would all compare the same "no matching
            # transactions" page.  The report's own section above checks the
            # format and column switches on 2018, and report-parity.sh compares
            # every column with the Scheme report.
            ok "$report's period defaults to the previous quarter; its switches are checked above"
            continue ;;
        "Investment Portfolio"|"Advanced Portfolio"|"Lot Viewer"|\
        "IFRS weighted-average cost basis report")
            # sample1.gnucash has no stock account, no price database and no
            # lots, so every option of the valuation cluster renders the same
            # empty report on it.  The descriptors and the 400s are checked
            # above; web/test/make-investment-book.py and report-parity.sh
            # cover what the options do.
            ok "$report needs a book with lots and prices; the generated investment book covers it"
            continue ;;
    esac

    while IFS=$'\t' read -r opt otype odefault; do
        [ "$opt" = "currency" ] && continue
        case "$otype" in
            date)
                case "$opt" in
                    start) probe="start=2018-12-31" ;;
                    end) probe="end=2030-12-31" ;;
                    *) bad "$report advertises date option '$opt' with an unknown name"
                       continue ;;
                esac ;;
            account-list)
                case "$opt" in
                    accounts) probe="accounts=all" ;;
                    account) probe="account=$BREAKDOWN_ACCOUNT&start=2018-01-01&end=2018-12-31" ;;
                    *) bad "$report advertises account option '$opt' with an unknown name"
                       continue ;;
                esac ;;
            choice)
                case "$opt" in
                    period) probe="period=all" ;;
                    alternate-period) probe="alternate-period=last-year" ;;
                    format) probe="format=txf" ;;
                    period-duration) probe="period-duration=year" ;;
                    *) bad "$report advertises choice option '$opt' with an unknown name"
                       continue ;;
                esac ;;
            text)
                case "$opt" in
                    title) probe="title=Statement" ;;
                    report-title) probe="report-title=Statement" ;;
                    description-filter) probe="description-filter=zzz" ;;
                    *) bad "$report advertises text option '$opt' with no probe"
                       continue ;;
                esac ;;
            number)
                # Lower the limit below the number of accounts the selection
                # links, so the report switches to its "Too many accounts"
                # warning.  The descriptor's own default is the upper case.
                probe="$opt=1" ;;
            bool)
                # Flip the descriptor's default, so the probe asks for the
                # opposite of what the report already renders.
                if [ "$odefault" = "true" ]; then
                    probe="$opt=false"
                else
                    probe="$opt=true"
                fi ;;
            invoice)
                # sample1's single document.  The generated invoice book has
                # the same option with a customer invoice; this probe only has
                # to show that selecting one changes the document.
                case "$opt" in
                    invoice) probe="invoice=yoyo1" ;;
                    *) bad "$report advertises invoice option '$opt' with an unknown name"
                       continue ;;
                esac ;;
            *)
                bad "$report advertises option '$opt' of unsupported type '$otype'"
                continue ;;
        esac

        if [ "$opt" = "accounts" ] || [ "$opt" = "invoice" ] || \
           [ "$opt" = "account" ]; then
            code=$(fetch "/report/$enc?$probe")
            before="$OPTDIR/no-options.html"
        else
            code=$(fetch "/report/$enc?${base:+$base&}$probe")
            before="$OPTDIR/baseline.html"
        fi

        if [ "$code" = "400" ]; then
            if grep -qF "does not accept a '$opt' option" "$BODY"; then
                ok "$report rejects $opt (400 names the option)"
            else
                bad "$report rejected $opt without naming it: $(head -c 200 "$BODY")"
            fi
        elif [ "$code" != "200" ]; then
            bad "$report $opt answered $code"
        elif ! cmp -s "$before" "$BODY"; then
            ok "$report $opt changes the report"
        else
            case "$report:$opt" in
                "Trial Balance:start")
                    if grep -qF 'accepted but has no effect' "$OPTDIR/options.json"; then
                        ok "$report $opt is the documented as-of no-op"
                    else
                        bad "$report $opt is a no-op but the descriptor does not say so"
                    fi ;;
                "Balance Sheet:start")
                    if grep -qF 'does not use a start date' "$OPTDIR/options.json"; then
                        ok "$report $opt is the documented as-of no-op"
                    else
                        bad "$report $opt is a no-op but the descriptor does not say so"
                    fi ;;
                "Printable Invoice:show_notes"|"Printable Invoice:show_terms"|\
                "Printable Invoice:show_payments")
                    # sample1's bill has no notes, no terms and no payments, so
                    # these switches have nothing to show on it.  The generated
                    # invoice book exercises them (report-parity.sh).
                    if grep -qF 'has no effect' "$OPTDIR/options.json"; then
                        ok "$report $opt has nothing to show on this book; the descriptor says so"
                    else
                        bad "$report $opt is a no-op but the descriptor does not say so"
                    fi ;;
                "Tax Invoice:show_tax_rate"|"Tax Invoice:show_contact"|\
                "Tax Invoice:show_address"|"Tax Invoice:show_job_name"|\
                "Tax Invoice:show_job_number"|"Tax Invoice:show_net_price"|\
                "Australian Tax Invoice:show_tax_rate"|\
                "Australian Tax Invoice:show_contact"|\
                "Australian Tax Invoice:show_address"|\
                "Australian Tax Invoice:show_job_name"|\
                "Australian Tax Invoice:show_job_number"|\
                "Australian Tax Invoice:show_net_price")
                    # sample1's only document is an untaxed vendor bill with
                    # no tax table, no job, no company contact person and the
                    # same net and gross price, so those switches have
                    # nothing to show.  The customer-invoice side, where they
                    # do change the document, is covered by report-parity.sh
                    # on the generated invoice book.
                    if grep -qF 'has no effect' "$OPTDIR/options.json"; then
                        ok "$report $opt has nothing to show on this book; the descriptor says so"
                    else
                        bad "$report $opt is a no-op but the descriptor does not say so"
                    fi ;;
                *)
                    bad "$report advertises $opt but the report is unchanged" ;;
            esac
        fi
    done < "$OPTDIR/options"
done < "$OPTDIR/reports"
rm -rf "$OPTDIR"

if [ "$FAILS" -ne 0 ]; then
    echo "$FAILS check(s) failed"
    exit 1
fi
echo "report option surface passed"
