#!/bin/bash
# Report parity: the Scheme report versus the native one, report by report.
#
# Usage: report-parity.sh <guile-build> <native-build> <source-dir>
#                         [book] [report] [start] [end] [accounts] [invoice]
#                         [options]
#
# <guile-build> must contain bin/gnucash-cli (a WITH_GNUCASH build); override
# the binary with GNC_CLI=/path/to/gnucash-cli.  <native-build> contains
# gnc-webd.  Both run against the same book.
#
# [report] defaults to "Trial Balance"; "Balance Sheet", "Income Statement",
# "Profit & Loss", "Transaction Report", "General Ledger", "Receivable Aging",
# "Payable Aging", "Investment Lots", "Budget Report",
# "Budget Income Statement", "Budget Profit & Loss", "Budget Balance Sheet",
# "Budget Flow", "Printable Invoice", "Receipt", "Tax Invoice",
# "Australian Tax Invoice", "Tax Schedule Report/TXF Export", "Customer
# Report", "Vendor Report", "Employee Report", "Job Report" and "Customer
# Summary" are supported.
# Each report has its own extractor (see extractor_for below), because the
# reports do not agree on what a row is: the Trial Balance is (account, debit,
# credit), the balance sheet and P&L are (label, amount) with section headings
# and totals, and the aging reports are (owner, six buckets, total) with a
# Total row per account.  The invoice document is compared as its line items
# and its money totals, not cell by cell: one extractor reads (description,
# quantity, unit price, amount) from the entries table, then the subtotal, the
# tax, the total, any payment row and the amount due.  Both engines put those
# in a div.entries-table with the same cell classes, so the same parser reads
# the Scheme document and the native one.
#
# The Receipt and the two Tax Invoice variants have their own extractors
# because their rows are different documents, not cell-class variants: the
# Receipt prints one row per line with a tax marker, an item count, and tax /
# payment / amount-due rows only when the document has them; the Tax Invoice
# prints the tax rate and the tax-inclusive total per line.  The extractors
# keep the cells both engines agree on, including the Receipt's marker and the
# Tax Invoice's "n/a" tax rate, rather than reducing both to a grand total.
#
# [start] and [end] are optional YYYY-MM-DD dates.  [accounts] is optional:
# "all", or a comma-separated list of account GUIDs.  Each GUID means that
# account and its whole subtree (which is what the native route does), so the
# oracle is asked for the same expanded list.  When any option is given the
# native report is asked with the report route's query parameters and the
# Scheme report is asked for the same options through a saved-report variant
# written into the oracle build's user data directory (gnucash-cli has no
# command line for report options).  Without options both engines use the
# report's own defaults, which is the real parity claim; the dates exist
# because the fixture books predate the current accounting period, so a period
# report on its defaults renders zeros and proves only the shape.
#
# The Trial Balance and Balance Sheet are as-of reports: the Scheme Trial
# Balance's current variant ignores its start date and the balance sheet has a
# single date option.  The native route accepts a start date for them (so a
# range can be handed to any report) but only the end date changes the figures;
# the harness sends the start date to the oracle where the report has that
# option.
#
# Numbers are compared as numbers, so currency symbols, thousands separators
# and the table markup itself do not count as a diff.
#
# Exit status: 0 when every row and total matches, 1 otherwise.
#
# The aging reports need a book with invoices and payments in it; the fixture
# book has a single vendor bill, so generate one with the companion script and
# pass it as [book]:
#
#   python3 web/test/make-aging-book.py /tmp/aging-book.gnucash
#   web/test/report-parity.sh <guile-build> <native-build> <source-dir> \
#       /tmp/aging-book.gnucash "Receivable Aging" "" 2026-06-30
#
# (add --two-receivable to the generator to put one customer's invoices in a
# second A/Receivable account, which exercises the per-account headings).
#
# The invoice report prints one document, selected by its "Invoice Number"
# option.  No fixture book in the tree has a customer invoice, so generate
# one and pass its id as [invoice]:
#
#   python3 web/test/make-invoice-book.py /tmp/invoice-book.gnucash
#   web/test/report-parity.sh <guile-build> <native-build> <source-dir> \
#       /tmp/invoice-book.gnucash "Printable Invoice" "" "" "" INV-1001
#
# The harness resolves the id to the invoice GUID the Scheme saved report
# needs; the native route takes the id itself.  Its dates are the posted and
# due dates on the invoice, not report options, so start/end are refused.
#
# [options] is an optional "name=value&..." string for the document report's
# display switches (show_date, show_tax_rate, show_units, show_contact,
# show_address, show_invoice_number, show_company_name,
# invoice_number_next_to_title, show_job_name, show_job_number,
# show_net_price, border_collapse) and its title.  The harness sets the same
# Scheme options through the saved report, so a flipped switch is compared
# with the oracle rather than only checked for changing the page.
#
# The owner reports (Customer Report, Vendor Report, Employee Report, Job
# Report) take their owner in that same [options] string as owner=<guid>; the
# harness sets the Scheme owner option as the (type . guid) pair the option
# system deserialises, using the report's own owner page.  Customer Summary
# takes no owner.  Both use From/To dates and the owner report's other
# switches (show_date, show_due_date, show_reference, show_type,
# show_description, show_job, show_sale, show_tax, show_debits, show_credits,
# show_balance; Customer Summary: show_expense, show_zero_lines,
# show_inactive).  Their extractors compare the history rows and totals plus
# the aging row, and the summary's customer/profit/markup/sales/expense rows.
#
# The owner fixture is generated by the same script as the invoice fixture,
# extended with a second customer, an employee, a job and their documents:
#
#   python3 web/test/make-invoice-book.py /tmp/owner-book.gnucash
#   web/test/report-parity.sh <guile-build> <native-build> <source-dir> \
#       /tmp/owner-book.gnucash "Customer Report" 2026-01-01 2026-12-31 "" "" \
#       "owner=<customer guid>"
#
# The book's ids and GUIDs are appended to <book>.ids, one "kind name guid"
# row per owner and document.
#
# The Investment Lots report needs a book with a stock account, lots and a
# price database; no fixture book has one, so generate it the same way and
# pass it as [book]:
#
#   python3 web/test/make-investment-book.py /tmp/investment-book.gnucash
#   web/test/report-parity.sh <guile-build> <native-build> <source-dir> \
#       /tmp/investment-book.gnucash "Investment Lots" 2026-01-01 2026-06-30
#
# The Tax Schedule report needs tax-related accounts with US TXF codes; no
# book in the tree has any, so generate one and run it for its tax year:
#
#   python3 web/test/make-tax-book.py /tmp/tax-book.gnucash
#   web/test/report-parity.sh <guile-build> <native-build> <source-dir> \
#       /tmp/tax-book.gnucash "Tax Schedule Report/TXF Export" \
#       2026-01-01 2026-12-31
#
# Its extractor compares the schedule's form/schedule headings, line and
# account rows, and every account/line/form total with its sign.  The
# transaction detail rows are deliberately not compared: the native report
# does not print them, and the claim this harness makes is the schedule table,
# not the split listing.  web/test/tax-txf-parity.sh compares the report's TXF
# export against the native ?format=txf output.
#
# Its account selection is not part of the native route: the report always
# covers every stock and mutual fund account, which is the Scheme report's
# default selection.
#
# The budget reports need a book with a budget; no fixture book has budget
# values, so generate one and run the reports with their own defaults (the
# whole budget and the default account selection):
#
#   python3 web/test/make-budget-book.py /tmp/budget-book.gnucash
#   web/test/report-parity.sh <guile-build> <native-build> <source-dir> \
#       /tmp/budget-book.gnucash "Budget Report"
#   web/test/report-parity.sh <guile-build> <native-build> <source-dir> \
#       /tmp/budget-book.gnucash "Budget Income Statement"
#   web/test/report-parity.sh <guile-build> <native-build> <source-dir> \
#       /tmp/budget-book.gnucash "Budget Profit & Loss"
#   web/test/report-parity.sh <guile-build> <native-build> <source-dir> \
#       /tmp/budget-book.gnucash "Budget Balance Sheet"
#   web/test/report-parity.sh <guile-build> <native-build> <source-dir> \
#       /tmp/budget-book.gnucash "Budget Flow"
#
# Their period ranges are ranges of budget periods, not calendar dates, so
# this harness refuses start/end for them.  A scoped run passes an account
# GUID, e.g. "Expenses", in the [accounts] position.  The Budget Chart is not
# in the native layer: it draws one chart per account and has no table to
# extract, which the single-chart Table model cannot represent.
set -u

GUILE_BUILD=${1:?usage: report-parity.sh <guile-build> <native-build> <source-dir> [book] [report] [start] [end] [accounts]}
NATIVE_BUILD=${2:?usage: report-parity.sh <guile-build> <native-build> <source-dir> [book] [report] [start] [end] [accounts]}
SRC=${3:?usage: report-parity.sh <guile-build> <native-build> <source-dir> [book] [report] [start] [end] [accounts]}
BOOK=${4:-$SRC/libgnucash/backend/xml/test/test-files/load-save/sample1.gnucash}
REPORT=${5:-Trial Balance}
START=${6:-}
END=${7:-}
ACCOUNTS=${8:-}
INVOICE=${9:-}
EXTRA_OPTIONS=${10:-}

CLI=${GNC_CLI:-$GUILE_BUILD/bin/gnucash-cli}
WEBDBIN="$NATIVE_BUILD/bin/gnc-webd"
BACKEND="$NATIVE_BUILD/lib/gnucash/libgncmod-backend-xml.so"
SAVED_REPORTS="$GUILE_BUILD/gnc_data_home/saved-reports-2.8"
VARIANT_NAME="Native parity report"

fail() { echo "FAIL: $*" >&2; exit 1; }

# The Scheme report and the native report must exist, and the extractor must
# know how to read their tables.
extractor_for() {
    case "$1" in
        "Trial Balance")    echo trial-balance ;;
        "Balance Sheet")    echo balance-sheet ;;
        "Income Statement"|"Profit & Loss") echo balance-sheet ;;
        "Transaction Report") echo transaction-report ;;
        "General Ledger")   echo general-ledger ;;
        "Receivable Aging"|"Payable Aging") echo aging ;;
        "Investment Lots") echo investment-lots ;;
        "Budget Report") echo budget-report ;;
        "Budget Income Statement"|"Budget Profit & Loss"|\
        "Budget Balance Sheet"|"Budget Flow") echo budget-statement ;;
        "Printable Invoice") echo invoice ;;
        "Receipt") echo receipt ;;
        "Tax Invoice"|"Australian Tax Invoice") echo tax-invoice ;;
        "Tax Schedule Report/TXF Export") echo tax-schedule ;;
        "Customer Report"|"Vendor Report"|"Employee Report"|\
        "Job Report") echo owner-report ;;
        "Customer Summary") echo customer-summary ;;
        *) return 1 ;;
    esac
}

report_guid() {
    case "$1" in
        "Trial Balance")    echo "216cd0cf6931453ebcce85415aba7082" ;;
        "Balance Sheet")    echo "c4173ac99b2b448289bf4d11c731af13" ;;
        "Income Statement") echo "0b81a3bdfd504aff849ec2e8630524bc" ;;
        "Profit & Loss")    echo "8758ba23984c40dea5527f5f0ca2779e" ;;
        "Transaction Report") echo "2fe3b9833af044abb929a88d5a59620f" ;;
        "General Ledger")   echo "2e22929e5c5b4b769f615a815ef0c20f" ;;
        "Receivable Aging") echo "9cf76bed17f14401b8e3e22d0079cb98" ;;
        "Payable Aging")    echo "e57770f2dbca46619d6dac4ac5469b50" ;;
        "Investment Lots")  echo "ab2acc24afd14630a551f98f1a35fa81" ;;
        "Budget Report")    echo "810ed4b25ef0486ea43bbd3dddb32b11" ;;
        "Budget Income Statement") echo "583c313fcc484efc974c4c844404f454" ;;
        "Budget Profit & Loss")    echo "e5fa5ce805e840ecbeca4dba3fa4ead9" ;;
        "Budget Balance Sheet")    echo "ecc35ea9dbfa4e20ba389fc85d59cb69" ;;
        "Budget Flow")             echo "e6e34fa3b6e748debde3cb3bc76d3e53" ;;
        "Printable Invoice")       echo "5123a759ceb9483abf2182d01c140e8d" ;;
        "Receipt")                 echo "7eb3df21073d4c33920a0257da15fba5" ;;
        "Tax Invoice")             echo "0769e242be474010b4acf264a5512e6e" ;;
        "Australian Tax Invoice")  echo "3dbbc2584da64e7a8674355bc3fbfe3d" ;;
        "Tax Schedule Report/TXF Export") echo "f8921f4e5c284d7caca81e239f468a68" ;;
        "Customer Report") echo "c146317be32e4948a561ec7fc89d15c1" ;;
        "Vendor Report") echo "d7d1e53505ee4b1b82efad9eacedaea0" ;;
        "Employee Report") echo "08ae9c2e884b4f9787144f47eacd7f44" ;;
        "Job Report") echo "5518ac227e474f47a34439f2d4d049de" ;;
        "Customer Summary") echo "4166a20981985fd2b07ff8cb3b7d384e" ;;
        *) return 1 ;;
    esac
}

# Reports whose Scheme table is produced by the transaction engine, and which
# therefore need an explicit account selection as well as a period.
needs_accounts() {
    case "$1" in
        "Transaction Report"|"General Ledger") return 0 ;;
        *) return 1 ;;
    esac
}

# The Scheme option names that carry the period.  They differ per report: the
# Trial Balance calls its end date "Date of Report" and its start date "Start
# of Adjusting/Closing", the balance sheet has one date, and the P&L and the
# transaction reports use "Start Date"/"End Date".
scheme_start_option() {
    case "$1" in
        "Trial Balance") echo "Start of Adjusting/Closing" ;;
        # The balance sheet has a single date option and no start.
        "Balance Sheet") echo "" ;;
        # The aging reports have a single "To" date as well.
        "Receivable Aging"|"Payable Aging") echo "" ;;
        # The lots report calls its dates "Start date"/"End date".
        "Investment Lots") echo "Start date" ;;
        # The tax schedule's period options are the Scheme names "From"/"To".
        "Tax Schedule Report/TXF Export") echo "From" ;;
        # The budget reports' periods are budget periods, not calendar dates;
        # Budget Report has no date option at all.
        "Budget Report"|"Budget Income Statement"|"Budget Profit & Loss"|\
        "Budget Balance Sheet"|"Budget Flow") echo "" ;;
        # The invoice documents' dates come from the invoice itself.
        "Printable Invoice"|"Receipt"|"Tax Invoice"|\
        "Australian Tax Invoice") echo "" ;;
        # The owner reports and the customer summary call their range From/To.
        "Customer Report"|"Vendor Report"|"Employee Report"|"Job Report"|\
        "Customer Summary") echo "From" ;;
        *) echo "Start Date" ;;
    esac
}

scheme_end_option() {
    case "$1" in
        "Trial Balance") echo "Date of Report" ;;
        "Balance Sheet") echo "Balance Sheet Date" ;;
        "Receivable Aging"|"Payable Aging") echo "To" ;;
        "Investment Lots") echo "End date" ;;
        "Tax Schedule Report/TXF Export") echo "To" ;;
        "Budget Report"|"Budget Income Statement"|"Budget Profit & Loss"|\
        "Budget Balance Sheet"|"Budget Flow") echo "" ;;
        "Printable Invoice"|"Receipt"|"Tax Invoice"|\
        "Australian Tax Invoice") echo "" ;;
        "Customer Report"|"Vendor Report"|"Employee Report"|"Job Report"|\
        "Customer Summary") echo "To" ;;
        *) echo "End Date" ;;
    esac
}

# The native route's own option surface.  The transaction engine reports and
# the budget reports have an account selection.
native_takes_accounts() {
    case "$1" in
        "Transaction Report"|"General Ledger"|"Budget Report"|\
        "Budget Income Statement"|"Budget Profit & Loss"|\
        "Budget Balance Sheet"|"Budget Flow"|\
        "Tax Schedule Report/TXF Export") return 0 ;;
        *) return 1 ;;
    esac
}

# The Scheme option name under the "Accounts" page.  The transaction reports
# and the budget balance sheet / budget statements call it "Accounts"; the
# Budget Report and Budget Flow call it "Account".
scheme_account_option() {
    case "$1" in
        "Budget Report"|"Budget Flow") echo "Account" ;;
        "Tax Schedule Report/TXF Export") echo "Select Accounts (none = all)" ;;
        *) echo "Accounts" ;;
    esac
}

EXTRACTOR=$(extractor_for "$REPORT") \
    || fail "no extractor for report '$REPORT' (supported: Trial Balance, Balance Sheet, Income Statement, Profit & Loss, Transaction Report, General Ledger, Receivable Aging, Payable Aging, Investment Lots, Budget Report, Budget Income Statement, Budget Profit & Loss, Budget Balance Sheet, Budget Flow, Printable Invoice, Receipt, Tax Invoice, Australian Tax Invoice, Tax Schedule Report/TXF Export, Customer Report, Vendor Report, Employee Report, Job Report, Customer Summary)"

case "$REPORT" in
    "Budget Report"|"Budget Income Statement"|"Budget Profit & Loss"|\
    "Budget Balance Sheet"|"Budget Flow")
        if [ -n "$START" ] || [ -n "$END" ]; then
            fail "the $REPORT period range is a range of budget periods, not dates"
        fi ;;
    "Printable Invoice"|"Receipt"|"Tax Invoice"|\
    "Australian Tax Invoice")
        if [ -n "$START" ] || [ -n "$END" ]; then
            fail "the $REPORT dates come from the invoice, not from report options"
        fi
        [ -n "$INVOICE" ] \
            || fail "the $REPORT needs an invoice id in the [invoice] position" ;;
esac

[ -x "$CLI" ] || fail "no gnucash-cli at $CLI (need a WITH_GNUCASH Guile build, or set GNC_CLI)"
[ -x "$WEBDBIN" ] || fail "gnc-webd was not built at $WEBDBIN"
[ -f "$BOOK" ] || fail "fixture book not found at $BOOK"

TMP=$(mktemp -d)
SERVER_PID=
SAVED_REPORTS_BACKUP=
SAVED_REPORTS_WRITTEN=
cleanup() {
    kill "$SERVER_PID" 2>/dev/null
    # Put the oracle build's saved reports back exactly as they were.  When
    # the file did not exist before this run, remove the one we wrote.
    if [ -n "$SAVED_REPORTS_WRITTEN" ]; then
        if [ -f "$SAVED_REPORTS_BACKUP" ]; then
            mv -f "$SAVED_REPORTS_BACKUP" "$SAVED_REPORTS"
        else
            rm -f "$SAVED_REPORTS"
        fi
    fi
    rm -rf "$TMP"
}
trap cleanup EXIT

# ------------------------------------------------------------------ oracle
#
# gnucash-cli can only run a report with its default options.  To ask the
# Scheme report for a period (and, for the transaction reports, an account
# selection), add a saved report to the build's user data directory that
# inherits the report's renderer and sets the options.  This is exactly the
# file GnuCash writes when a user saves a report configuration, and the
# oracle build is a test build, so writing it is fair game; it is restored
# on exit.
ORACLE_NAME="$REPORT"

if [ -n "$START" ] && [ -z "$END" ]; then
    fail "a start date needs an end date too (got start=$START)"
fi
if [ -n "$ACCOUNTS" ] && ! native_takes_accounts "$REPORT"; then
    fail "$REPORT has no account selection, so [accounts] cannot be used"
fi

# The four owner reports are one report each only once an owner is selected;
# the harness passes it through the [options] string, as the route does.
case "$REPORT" in
    "Customer Report"|"Vendor Report"|"Employee Report"|"Job Report")
        case "&$EXTRA_OPTIONS&" in
            *"&owner="*) ;;
            *) fail "$REPORT needs owner=<owner GUID> in [options]" ;;
        esac ;;
    "Customer Summary")
        case "&$EXTRA_OPTIONS&" in
            *"&owner="*) fail "Customer Summary has no owner option" ;;
        esac ;;
esac

if [ -n "$START" ] || [ -n "$END" ] || [ -n "$ACCOUNTS" ] || \
   [ -n "$INVOICE" ] || [ -n "$EXTRA_OPTIONS" ]; then
    GUID=$(report_guid "$REPORT") || fail "unknown report guid for '$REPORT'"

    START_T64=
    END_T64=
    if [ -n "$START" ]; then
        START_T64=$(date -u -d "$START 12:00:00" +%s) \
            || fail "cannot parse start date '$START' (want YYYY-MM-DD)"
    fi
    if [ -n "$END" ]; then
        END_T64=$(date -u -d "$END 12:00:00" +%s) \
            || fail "cannot parse end date '$END' (want YYYY-MM-DD)"
    fi

    DATE_OPTIONS=""
    START_OPTION=$(scheme_start_option "$REPORT")
    if [ -n "$START" ] && [ -n "$START_OPTION" ]; then
        DATE_OPTIONS="$DATE_OPTIONS
      (gnc-set-option (gnc:optiondb options) \"General\" \"$START_OPTION\"
                      (cons 'absolute $START_T64))"
    fi
    if [ -n "$END" ]; then
        DATE_OPTIONS="$DATE_OPTIONS
      (gnc-set-option (gnc:optiondb options) \"General\" \"$(scheme_end_option "$REPORT")\"
                      (cons 'absolute $END_T64))"
    fi

    ACCOUNT_OPTION=""
    if [ -n "$ACCOUNTS" ]; then
        if [ "$ACCOUNTS" = "all" ]; then
            ACCOUNT_EXPR='(gnc-account-get-descendants-sorted (gnc-get-current-root-account))'
        else
            # The native route expands each GUID to that account's subtree, so
            # give the oracle the same list rather than the bare GUIDs.
            GUID_LIST=""
            OLDIFS=$IFS
            IFS=,
            for guid in $ACCOUNTS; do
                [ -n "$guid" ] || continue
                GUID_LIST="$GUID_LIST \"$guid\""
            done
            IFS=$OLDIFS
            ACCOUNT_EXPR="(let* ((root (gnc-get-current-root-account))
                           (all (cons root (gnc-account-get-descendants-sorted root)))
                           (wanted (list$GUID_LIST)))
                      (apply append
                             (map (lambda (a)
                                    (cons a (gnc-account-get-descendants-sorted a)))
                                  (filter (lambda (a)
                                            (member (gncAccountGetGUID a) wanted))
                                          all))))"
        fi
        ACCOUNT_OPTION="(gnc-set-option (gnc:optiondb options) \"Accounts\" \"$(scheme_account_option "$REPORT")\"
                        $ACCOUNT_EXPR)"
    fi

    INVOICE_OPTION=""
    if [ -n "$INVOICE" ]; then
        # The Scheme option's value is the invoice object, which the saved
        # report selects by GUID (GncOptionQofInstanceValue::deserialize).
        # Resolve the document id to that GUID from the book itself, so the
        # harness takes the same id the native route does.
        INVOICE_GUID=$(python3 - "$BOOK" "$INVOICE" <<'PY'
import re
import sys

raw = open(sys.argv[1], encoding="utf-8", errors="replace").read()
match = re.search(
    r'<invoice:guid type="guid">([0-9a-fA-F]{32})</invoice:guid>\s*'
    r'<invoice:id>' + re.escape(sys.argv[2]) + r'</invoice:id>', raw)
if match is None:
    sys.exit(1)
print(match.group(1))
PY
        ) || fail "no invoice '$INVOICE' in $BOOK"
        INVOICE_OPTION="(gnc-set-option (gnc:optiondb options) \"General\" \"Invoice Number\" \"$INVOICE_GUID\")"
    fi

    # The document display switches are Scheme booleans on the "Elements"
    # page (the Report Title is on "Headings 1").  The owner reports keep
    # their display switches on "Display Columns" and the Customer Summary on
    # "Display".  Translate the route's names and true/false spelling so the
    # oracle is asked for exactly the same page the native route renders.
    EXTRA_SCHEME=""
    if [ -n "$EXTRA_OPTIONS" ]; then
        OLDIFS=$IFS
        IFS='&'
        for pair in $EXTRA_OPTIONS; do
            key=${pair%%=*}
            value=${pair#*=}
            case "$REPORT" in
                "Customer Report"|"Vendor Report"|"Employee Report"|\
                "Job Report")
                    case "$key" in
                        owner)
                            case "$REPORT" in
                                "Customer Report")
                                    OWNER_PAGE=Customer
                                    OWNER_TYPE=GNC-OWNER-CUSTOMER ;;
                                "Vendor Report")
                                    OWNER_PAGE=Vendor
                                    OWNER_TYPE=GNC-OWNER-VENDOR ;;
                                "Employee Report")
                                    OWNER_PAGE=Employee
                                    OWNER_TYPE=GNC-OWNER-EMPLOYEE ;;
                                "Job Report")
                                    OWNER_PAGE=Job
                                    OWNER_TYPE=GNC-OWNER-JOB ;;
                            esac
                            # GncOptionGncOwnerValue takes the owner type and
                            # GUID as the pair gnc-set-option deserialises.
                            EXTRA_SCHEME="$EXTRA_SCHEME
      (gnc-set-option (gnc:optiondb options) \"General\" \"$OWNER_PAGE\"
                      (cons $OWNER_TYPE \"$value\"))"
                            continue ;;
                        show_date)        page="Display Columns"; name="Date" ;;
                        show_due_date)    page="Display Columns"; name="Due Date" ;;
                        show_reference)   page="Display Columns"; name="Reference" ;;
                        show_type)        page="Display Columns"; name="Type" ;;
                        show_description) page="Display Columns"; name="Description" ;;
                        show_job)         page="Display Columns"; name="Job" ;;
                        show_sale)        page="Display Columns"; name="Sale" ;;
                        show_tax)         page="Display Columns"; name="Tax" ;;
                        show_debits)      page="Display Columns"; name="Debits" ;;
                        show_credits)     page="Display Columns"; name="Credits" ;;
                        show_balance)     page="Display Columns"; name="Balance" ;;
                        *) fail "no Scheme mapping for option '$key' on $REPORT" ;;
                    esac ;;
                "Customer Summary")
                    case "$key" in
                        show_expense)
                            page="Display"; name="Show Expense Column" ;;
                        show_zero_lines)
                            page="Display"; name="Show Lines with All Zeros" ;;
                        show_inactive)
                            page="Display"; name="Show Inactive Customers" ;;
                        *) fail "no Scheme mapping for option '$key' on $REPORT" ;;
                    esac ;;
                *)
                    case "$key" in
                        title)                 page="Headings 1"; name="Report Title" ;;
                        show_date)             page="Elements"; name="column: Date" ;;
                        show_tax_rate)         page="Elements"; name="column: Tax Rate" ;;
                        show_units)            page="Elements"; name="column: Units" ;;
                        show_contact)          page="Elements"; name="row: Contact" ;;
                        show_address)          page="Elements"; name="row: Address" ;;
                        show_invoice_number)   page="Elements"; name="row: Invoice Number" ;;
                        show_company_name)     page="Elements"; name="row: Company Name" ;;
                        invoice_number_next_to_title)
                            page="Elements"; name="Invoice Number next to title" ;;
                        show_job_name)         page="Elements"; name="Show Job name" ;;
                        show_job_number)       page="Elements"; name="Show Job number" ;;
                        show_net_price)        page="Elements"; name="Show net price" ;;
                        border_collapse)       page="Display"; name="table-border-collapse" ;;
                        *) fail "no Scheme mapping for option '$key'" ;;
                    esac ;;
            esac
            case "$value" in
                true)  scheme_value="#t" ;;
                false) scheme_value="#f" ;;
                *)     scheme_value="\"$value\"" ;;
            esac
            EXTRA_SCHEME="$EXTRA_SCHEME
      (gnc-set-option (gnc:optiondb options) \"$page\" \"$name\" $scheme_value)"
        done
        IFS=$OLDIFS
    fi

    mkdir -p "$(dirname "$SAVED_REPORTS")"
    if [ -f "$SAVED_REPORTS" ]; then
        SAVED_REPORTS_BACKUP="$TMP/saved-reports.orig"
        cp "$SAVED_REPORTS" "$SAVED_REPORTS_BACKUP"
    fi
    SAVED_REPORTS_WRITTEN=1
    cat > "$SAVED_REPORTS" <<EOF
;; Written by web/test/report-parity.sh; removed when the run ends.
(let ()
  (define (options-gen)
    (let ((options (gnc:report-template-new-options/report-guid "$GUID" "$REPORT"))
          (new-embedded-report-ids '()))
      $DATE_OPTIONS
      $ACCOUNT_OPTION
      $INVOICE_OPTION
      $EXTRA_SCHEME
      options))
  (gnc:define-report
    'version 1
    'name "$VARIANT_NAME"
    'report-guid "9e9e9e9e9e9e9e9e9e9e9e9e9e9e9e9e"
    'parent-type "$GUID"
    'options-generator options-gen
    'menu-path (list gnc:menuname-custom)
    'renderer (gnc:report-template-renderer/report-guid "$GUID" "$REPORT")))
EOF
    ORACLE_NAME="$VARIANT_NAME"
fi

SCHEMAS="$GUILE_BUILD/share/glib-2.0/schemas"
[ -f "$SCHEMAS/gschemas.compiled" ] || SCHEMAS=""
CCACHE=$(ls -d "$GUILE_BUILD"/lib/*/guile/3.0/site-ccache 2>/dev/null | head -1)

echo "oracle: $CLI ($ORACLE_NAME)"
env GNC_UNINSTALLED=YES GNC_BUILDDIR="$GUILE_BUILD" \
    ${SCHEMAS:+GSETTINGS_SCHEMA_DIR="$SCHEMAS"} \
    LD_LIBRARY_PATH="$GUILE_BUILD/lib:$GUILE_BUILD/lib/gnucash" \
    GNC_MODULE_PATH="$GUILE_BUILD/lib/gnucash" \
    GUILE_LOAD_PATH="$GUILE_BUILD/share/guile/site/3.0" \
    GUILE_LOAD_COMPILED_PATH="${CCACHE:-}" \
    "$CLI" "$BOOK" -R run --name "$ORACLE_NAME" \
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
for _ in $(seq 1 300); do
    URL=$(grep -oE 'http://127\.0\.0\.1:[0-9]+/[0-9a-f]+/' "$TMP/server.log" | head -1)
    [ -n "$URL" ] && break
    kill -0 "$SERVER_PID" 2>/dev/null || break
    sleep 0.2
done
[ -n "$URL" ] || { cat "$TMP/server.log" >&2; fail "gnc-webd did not start"; }

REQUEST_NAME=$(printf '%s' "$REPORT" | sed 's/ /%20/g')
REQUEST_QUERY=""
add_param() { REQUEST_QUERY="${REQUEST_QUERY}${REQUEST_QUERY:+&}$1"; }
if [ -n "$START" ]; then
    add_param "start=$START"
fi
if [ -n "$END" ]; then
    add_param "end=$END"
fi
if [ -n "$ACCOUNTS" ]; then
    add_param "accounts=$ACCOUNTS"
elif needs_accounts "$REPORT" && { [ -n "$START" ] || [ -n "$END" ]; }; then
    # Backwards compatibility: a period on the transaction reports used to
    # imply "every account" in this harness.  The route still has to be asked
    # for it explicitly, and that is what this is.
    add_param "accounts=all"
fi
if [ -n "$INVOICE" ]; then
    ENCODED_INVOICE=$(python3 -c 'import sys,urllib.parse; print(urllib.parse.quote(sys.argv[1]))' "$INVOICE")
    add_param "invoice=$ENCODED_INVOICE"
fi
if [ -n "$EXTRA_OPTIONS" ]; then
    add_param "$EXTRA_OPTIONS"
fi
[ -n "$REQUEST_QUERY" ] && REQUEST_QUERY="?$REQUEST_QUERY"
curl -fsS "${URL%/}/report/$REQUEST_NAME$REQUEST_QUERY" -o "$TMP/native.html" \
    || { cat "$TMP/server.log" >&2; fail "the native report request failed"; }
[ -s "$TMP/native.html" ] || fail "the native report is empty"

# ----------------------------------------------------------------- compare
python3 - "$TMP/scheme.html" "$TMP/native.html" "$EXTRACTOR" "$REPORT" <<'PY'
import re
import os
import sys
from decimal import Decimal, InvalidOperation
from html.parser import HTMLParser


class TableParser(HTMLParser):
    """Pull rows of (text, class) cells out of a report table.

    Both renderers use the Scheme cell classes -- text-cell / anchor-cell for
    the label, number-cell / total-number-cell for amounts -- so one parser
    reads either document.
    """

    def __init__(self, scope=None, top_level=False):
        super().__init__(convert_charrefs=True)
        self.rows = []
        self.row = None
        self.cell = None
        self.cell_class = ""
        # A document has header tables around the one table it compares;
        # scope limits collection to the element that carries the entries, so
        # the client and company tables cannot leak into the rows.  The
        # printable invoice scopes by div class, the receipt by div id, and
        # the tax invoices by table class.
        self.scope = scope
        self.in_scope = scope is None
        self.scope_tag = None
        self.scope_depth = 0
        # The tax schedule's total cells contain a nested one-cell table
        # (the Scheme renderer builds them that way).  top_level skips rows
        # and cells inside a cell, so the outer row is not reset by it.
        self.top_level = top_level
        self.table_depth = 0
        self.cell_depth = 0

    def _matches(self, tag, attrs):
        if self.scope is None:
            return False
        if tag == "div":
            return (self.scope in attrs.get("class", "").split()
                    or attrs.get("id", "") == self.scope)
        if tag == "table":
            return self.scope in attrs.get("class", "").split()
        return False

    def handle_starttag(self, tag, attrs):
        attrs = dict(attrs)
        if self.scope is not None:
            if not self.in_scope:
                if self._matches(tag, attrs):
                    self.in_scope = True
                    self.scope_tag = tag
                    self.scope_depth = 1
                return
            if tag == self.scope_tag:
                self.scope_depth += 1
        if self.top_level:
            if tag == "table":
                self.table_depth += 1
            elif tag == "tr":
                if self.table_depth == 1 and self.cell_depth == 0:
                    self.row = []
            elif tag in ("td", "th"):
                self.cell_depth += 1
                if self.cell_depth == 1 and self.row is not None:
                    self.cell = []
                    self.cell_class = attrs.get("class", "")
            return
        if tag == "tr":
            self.row = []
        elif tag in ("td", "th") and self.row is not None:
            self.cell = []
            self.cell_class = attrs.get("class", "")

    def handle_endtag(self, tag):
        if self.scope is not None:
            if not self.in_scope:
                return
            if tag == self.scope_tag:
                self.scope_depth -= 1
                if self.scope_depth == 0:
                    self.in_scope = False
                return
        if self.top_level:
            if tag in ("td", "th") and self.cell_depth > 0:
                self.cell_depth -= 1
                if (self.cell_depth == 0 and self.cell is not None
                        and self.row is not None):
                    text = " ".join("".join(self.cell).split())
                    self.row.append((text, self.cell_class))
                    self.cell = None
                    self.cell_class = ""
            elif tag == "tr":
                if (self.table_depth == 1 and self.row is not None
                        and self.cell_depth == 0):
                    if self.row:
                        self.rows.append(self.row)
                    self.row = None
            elif tag == "table":
                self.table_depth -= 1
            return
        if tag in ("td", "th") and self.row is not None and self.cell is not None:
            text = " ".join("".join(self.cell).split())
            self.row.append((text, self.cell_class))
            self.cell = None
            self.cell_class = ""
        elif tag == "tr" and self.row is not None:
            if self.row:
                self.rows.append(self.row)
            self.row = None

    def handle_data(self, data):
        if self.cell is not None:
            self.cell.append(data)


def parse(path, scope=None, top_level=False):
    parser = TableParser(scope, top_level)
    with open(path, encoding="utf-8", errors="replace") as handle:
        raw = handle.read()
    parser.feed(raw)
    return raw, parser.rows


NO_ACCOUNTS = "No accounts selected"
NO_BUDGET = "No budgets exist"


def extract(extractor, path, scope=None, top_level=False):
    """Run a report's extractor, and notice its warning page.

    The transaction report and the general ledger have no default account
    selection; both engines then render a warning instead of a table.  The
    budget report renders its own warning when the book has no budget.  Those
    warnings are the reports' correct default output, so compare their wording
    rather than treating "no rows" as a match by accident.
    """
    raw, rows = parse(path, scope, top_level)
    extracted = extractor(rows)
    if not extracted and NO_ACCOUNTS in raw:
        return [("PLACEHOLDER", NO_ACCOUNTS)]
    if not extracted and NO_BUDGET in raw:
        return [("PLACEHOLDER", NO_BUDGET)]
    return extracted


def amount(text):
    """A comparable number, independent of currency formatting."""
    if not text:
        return Decimal(0)
    text = text.replace("\u00a0", " ")
    negative = text.strip().startswith("(") and text.strip().endswith(")")
    text = re.sub(r"[^0-9.,\-]", "", text)
    if text in ("", "-", "."):
        return Decimal(0)
    try:
        value = Decimal(text.replace(",", ""))
    except InvalidOperation:
        raise SystemExit(f"cannot parse amount {text!r}")
    return -value if negative else value


NUMERIC = ("number-cell", "total-number-cell")
LABEL = ("anchor-cell", "text-cell", "total-label-cell", "centered-label-cell")


def is_number(cell_class):
    """Whether a cell class list marks an amount.

    The renderer appends "neg" to a negative amount's class ("number-cell
    neg"), so the class has to be read as a list of tokens rather than
    compared with a single string.
    """
    tokens = cell_class.split()
    return "number-cell" in tokens or "total-number-cell" in tokens


def is_total(cell_class):
    return "total-number-cell" in cell_class.split()


def trial_balance(rows):
    """(account, debit, credit), plus the totals row."""
    out = []
    for cells in rows:
        classes = [c for _, c in cells]
        if not classes:
            continue
        if "total-number-cell" in classes:
            numbers = [t for t, c in cells if c == "total-number-cell"]
            out.append(("__TOTAL__",
                        numbers[0] if len(numbers) > 0 else "",
                        numbers[1] if len(numbers) > 1 else ""))
            continue
        if classes[0] not in LABEL:
            continue
        debit = cells[1][0] if len(cells) > 1 else ""
        credit = cells[2][0] if len(cells) > 2 else ""
        out.append((cells[0][0], debit, credit))
    return [tuple([row[0]] + [amount(v) for v in row[1:]]) for row in out]


def balance_sheet(rows):
    """(label, [amounts]): section headings, account rows and totals.

    The amount column moves with the account's depth in the Scheme layout,
    so the extractor collects the row's amounts wherever they sit and drops
    the blank cells.  A section heading has no amount at all; a zero amount
    prints as "$0.00" and is a different thing.
    """
    out = []
    for cells in rows:
        if not cells or cells[0][1] not in LABEL:
            continue
        label = cells[0][0]
        if not label:
            continue
        values = [amount(t) for t, c in cells[1:] if c in NUMERIC and t.strip()]
        out.append((label, tuple(values)))
    return out


def transaction_report(rows):
    """Rows of the split listing: group headings, split lines, totals.

    The Scheme layout puts the group heading in the first cell and the split
    date in the second; a total row has its label in the second cell.  The
    number of cells differs between the two renderers (they use colspan for
    the label), so only the cell sequence up to the amounts is compared, and
    trailing blank cells are dropped.
    """
    out = []
    for cells in rows:
        classes = [c for _, c in cells]
        if len(cells) >= 2 and classes[1] == "date-cell":
            values = [t for t, c in cells[1:]]
            key = tuple(values[:6])
            amounts = tuple(amount(v) for v in values[6:])
            out.append(("SPLIT",) + key + amounts)
            continue
        if "total-number-cell" in classes:
            # "Total For X" and "Grand Total".  The Scheme subtotal rows have
            # an empty indent cell before the label and the grand total does
            # not; the amounts are whatever the total cells hold.
            label = next(t for t, c in cells if c == "total-label-cell")
            amounts = tuple(amount(t) for t, c in cells
                            if c == "total-number-cell")
            out.append(("TOTAL", label) + amounts)
            continue
        if classes and classes[0] == "total-label-cell":
            out.append(("HEADING", cells[0][0]))
            continue
    return out


def general_ledger(rows):
    """The register-style listing: "Balance b/f" headings, split lines with a
    running balance, and "Total For" lines."""
    out = []
    for cells in rows:
        classes = [c for _, c in cells]
        if len(cells) >= 4 and classes[0] == "total-label-cell" \
                and cells[0][0].endswith("Balance b/f"):
            amounts = tuple(amount(t) for t, c in cells[1:]
                            if c == "number-cell")
            out.append(("HEADING", cells[0][0]) + amounts)
            continue
        if len(cells) >= 2 and classes[1] == "total-label-cell":
            amounts = tuple(amount(t) for t, c in cells[2:] if c in NUMERIC)
            out.append(("TOTAL", cells[1][0]) + amounts)
            continue
        if len(cells) >= 6 and classes[1] == "date-cell":
            out.append(("SPLIT", cells[1][0], cells[2][0],
                        amount(cells[3][0]), amount(cells[4][0]),
                        amount(cells[5][0])))
    return out


def aging(rows):
    """The aging reports: one row per owner, one Total row per account.

    The owner label cell carries no class in either renderer (it is a plain
    cell holding an anchor in the Scheme document), so the label is the first
    non-numeric cell that has text.  The six buckets and the row total are
    number cells; the account's Total row prints seven total-number-cells.
    With more than one account of the type, the Scheme report puts the
    account name on a row of its own, which is compared as a heading so the
    account grouping is part of the parity claim too.
    """
    out = []
    for cells in rows:
        if not cells:
            continue
        numbers = tuple(amount(t) for t, c in cells if is_number(c))
        labels = [t for t, c in cells if not is_number(c) and t.strip()]
        if len(cells) == 1:
            out.append(("HEADING", cells[0][0]))
        elif not numbers:
            continue
        elif any(is_total(c) for _, c in cells):
            out.append(("TOTAL", labels[0] if labels else "", numbers))
        else:
            out.append(("ROW", labels[0] if labels else "", numbers))
    return out


def investment_lots(rows):
    """The lots report: one section per account, one row per lot.

    Every cell of a lot row counts: the opened/closed dates sit in number
    cells ("03/10/25"), the share amounts carry their commodity ("50. ACME"),
    the money columns are currency amounts and the two CAGR columns are
    percentages ("85.1%").  A zero amount that the report blanks on a
    non-total row is a None, which is a different thing from a printed zero.
    The account heading, its end price line, the column heading rows and the
    grand total are compared too, so the report's layout is part of the claim.
    """
    out = []
    for cells in rows:
        classes = [c for _, c in cells]
        texts = [t for t, _ in cells]
        if len(cells) == 1:
            text = texts[0].strip()
            if not text:
                continue          # padding rows and the rulers between sections
            if classes[0].startswith("column-heading-left"):
                out.append(("ACCOUNT", text))
            elif text.startswith("End price:") or text == "No price found":
                out.append(("PRICE", text))
            continue
        if all(c.startswith("column-heading-center") for c in classes):
            out.append(("HEADINGS", tuple(texts)))
            continue
        # The first cell is the row label (lot title, "Account Lots Total",
        # "Grand Total"); it carries the total-number-cell class on total
        # rows, so it must be read as text, not as an amount.
        values = [texts[0].strip()]
        values.extend(lot_value(text, cell_class)
                      for text, cell_class in cells[1:])
        out.append(("ROW", tuple(values)))
    return out


def lot_value(text, cell_class):
    """One cell of a lots-table row, as a comparable value."""
    stripped = text.strip()
    if not stripped:
        return None
    if re.fullmatch(r"\d{1,2}/\d{1,2}/\d{2,4}", stripped):
        return stripped          # a date in a number cell
    if stripped.endswith("%"):
        return Decimal(stripped[:-1])
    if is_number(cell_class):
        return amount(stripped)
    return stripped


def budget_statement(rows):
    """A budget statement/flow row: (label, all amounts in the row).

    The balance-sheet extractor cannot be reused because it matches the cell
    class exactly and the Scheme renderer appends "neg" to negative amounts
    ("number-cell neg"), which would silently drop exactly the sign-reversed
    income and liability rows this comparison is about.  Section headings
    legitimately have no amount, so they stay in the comparison as an empty
    tuple; rule rows have no label cell and are skipped.
    """
    out = []
    for cells in rows:
        if not cells or cells[0][1] not in LABEL:
            continue
        label = " ".join(cells[0][0].replace("\u00a0", " ").split())
        if not label:
            continue
        values = [amount(t) for t, c in cells[1:] if is_number(c) and t.strip()]
        out.append((label, tuple(values)))
    return out


def budget_report(rows):
    """The budget-vs-actual table: (account, budget, actual, ...) per period.

    The Scheme document has two header rows (the period date with a colspan,
    then the Bgt/Act labels) and the native document one (``Bgt <date>`` /
    ``Act <date>``); neither header row has a label in the first cell, so the
    extractor keeps every row that has a text label and at least one amount.
    A zero budget prints "." in both engines, which amount() reads as zero; a
    zero actual prints a monetary zero.  The account labels are indented with
    non-breaking spaces in the Scheme document, so those are stripped before
    comparison.
    """
    out = []
    for cells in rows:
        if not cells or cells[0][1] not in LABEL:
            continue
        label = " ".join(cells[0][0].replace("\u00a0", " ").split())
        if not label:
            continue
        numbers = tuple(amount(t) for t, c in cells[1:]
                        if is_number(c) and t.strip())
        if not numbers:
            continue
        out.append((label,) + numbers)
    return out


def invoice(rows):
    """The invoice document, as numbers: line items and money totals.

    This is deliberately not a cell-by-cell comparison.  A document's layout
    is allowed to differ; its numbers are not.  From the entries table
    (div.entries-table, which the parser has already scoped to) this pulls:

      * one ("ITEM", description, quantity, unit price, amount) per entry.
        The description is the text cell after the date; the three numbers
        are the quantity, unit-price and net-amount cells, in that order.
      * ("PAYMENT", (amount,)) for each applied payment: the Scheme document
        prints it as date + "Payment, thank you!" + one spanning amount.
      * ("TOTAL", label, (amount,)) for the subtotal ("Net Price"), the tax
        ("Tax"), the total ("Total Price") and the amount due ("Amount Due").

    Both engines use number-cell/total-number-cell for the amounts, so the
    cell classes carry the meaning even though the markup around them does
    not match.
    """
    out = []
    for cells in rows:
        if not cells:
            continue
        classes = [c for _, c in cells]
        texts = [t for t, _ in cells]
        tokens = [c.split() for c in classes]

        label_index = next(
            (i for i, toks in enumerate(tokens) if "total-label-cell" in toks),
            None)
        if label_index is not None:
            numbers = tuple(amount(t) for t, c in cells
                            if is_number(c) and t.strip())
            out.append(("TOTAL", texts[label_index].strip(), numbers))
            continue

        if any("total-number-cell" in toks for toks in tokens):
            numbers = tuple(amount(t) for t, c in cells
                            if is_number(c) and t.strip())
            out.append(("PAYMENT", numbers))
            continue

        numbers = [(i, texts[i]) for i, toks in enumerate(tokens)
                   if is_number(classes[i]) and texts[i].strip()]
        if (numbers and len(cells) >= 4 and not is_number(classes[0])
                and len(texts) >= 2):
            quantity = amount(numbers[0][1])
            unit_price = amount(numbers[1][1]) if len(numbers) > 1 \
                else Decimal(0)
            value = amount(numbers[-1][1])
            out.append(("ITEM", texts[1].strip(), quantity, unit_price,
                        value))
    return out


DATE = re.compile(r"\d{1,2}/\d{1,2}/\d{2,4}")


def receipt(rows):
    """The Receipt document: its columns, lines, item count and totals.

    The receipt is a different document, not an invoice with fewer cells.
    It always prints the five product columns, then one row per line with the
    tax marker in the amount cell, then the item count, then (only when the
    document has them) the Net Price / Tax Amount / Total Price subtotals,
    the payments and the amount due.  Keeping the marker and the count in the
    extract makes the receipt's differences from an invoice part of the
    parity claim instead of hiding them behind a total.
    """
    out = []
    for cells in rows:
        texts = [t for t, _ in cells]
        if not texts:
            continue
        if texts[0] == "Date" and "Descr." in texts:
            out.append(("COLUMNS", tuple(texts)))
            continue
        if len(cells) == 5:
            marker = bool(re.search(r"(?:^|\s)T$", texts[4]))
            out.append(("ITEM", texts[0], texts[1], amount(texts[2]),
                        amount(texts[3]), amount(texts[4]), marker))
            continue
        if len(cells) == 1:
            match = re.search(r"(\d+)\s*$", texts[0])
            out.append(("COUNT", int(match.group(1)) if match else texts[0]))
            continue
        if len(cells) == 3 and "Payment received" in texts[1]:
            out.append(("PAYMENT", texts[0], texts[1], amount(texts[2])))
            continue
        if len(cells) == 2:
            if not texts[0] and texts[1]:
                out.append(("NOTE", texts[1]))
            elif texts[0]:
                out.append(("TOTAL", texts[0], amount(texts[1])))
            continue
    return out


def tax_invoice(rows):
    """The Tax Invoice document: its column layout, lines, subtotal and due.

    The Scheme template adds optional columns to the same table (Date, Units,
    the discount pair, Net Price / Tax Rate / Tax Amount), so the extractor
    reads the header row first and maps each line by column name.  A tax rate
    is text ("10.00%" or "n/a"), not an amount: the point of this document is
    the tax fields, and the "n/a" on an untaxed line is a value the two
    engines must agree on.  The subtotal row's blank tax-rate cell is kept as
    None, so a filled cell cannot pass for a blank one.
    """
    out = []
    columns = None
    for cells in rows:
        texts = [t for t, _ in cells]
        if not texts:
            continue
        if columns is None and "Description" in texts:
            columns = texts
            out.append(("COLUMNS", tuple(texts)))
            continue
        if columns is None:
            continue
        if len(cells) == len(columns):
            values = []
            for label, text in zip(columns, texts):
                if label in ("Date", "Description", "Units", "Tax Rate",
                             "GST Rate"):
                    values.append(text)
                else:
                    values.append(amount(text) if text.strip() else None)
            out.append(("ITEM", tuple(values)))
            continue
        if texts[0] == "Sub-total":
            values = tuple(amount(t) if t.strip() else None
                           for t in texts[1:])
            out.append(("SUBTOTAL", texts[0], values))
            continue
        if "Payment received" in texts:
            index = next(i for i, t in enumerate(texts)
                         if t.startswith("Payment received"))
            date = texts[0] if index > 0 and DATE.fullmatch(texts[0]) else ""
            out.append(("PAYMENT", date, texts[index],
                        amount(texts[index + 1])))
            continue
        if texts[0].startswith("Amount Due"):
            out.append(("TOTAL", texts[0], amount(texts[1])))
            continue
    return out


def tax_schedule(rows):
    """The US Tax Schedule: its headings and every subtotal, with signs.

    The table is one block per form/schedule line: a "Form or Schedule / Line
    (TXF Code) / Account Name" header, the form, the line, one heading per
    account, and then the account, payer (for format-3 codes), line and
    form-line totals.  The totals carry the class "column-heading-right" in
    both documents, which is what marks them apart from the headings.  The
    per-split transaction rows are not part of the claim (the native report
    does not print them), so the parser is run top-level-only and the
    date/number detail cells are ignored.  Numbers are compared as numbers,
    which keeps the currency symbol out of the comparison but not the sign.
    """
    out = []
    for cells in rows:
        classes = [c.split() for _, c in cells]
        if len(cells) == 1 and classes[0] and \
                classes[0][0] in ("column-heading-left", "total-label-cell"):
            text = cells[0][0]
            if text == "Form or Schedule / Line (TXF Code) / Account Name":
                continue
            if text:
                out.append(("HEADING", text))
            continue
        if any("column-heading-right" in tokens for tokens in classes):
            label = next(t for t, c in cells
                         if "column-heading-right" in c.split())
            numbers = tuple(amount(t) for t, c in cells
                            if is_number(c) and t.strip())
            out.append(("TOTAL", label, numbers))
    return out


def owner_report(rows):
    """The owner reports: the history rows, the two total rows and the aging.

    The four Scheme registrations are one table with Date, Due Date, Reference,
    Type, Description, Debits, Credits and Balance; a "Balance" row carries the
    brought-forward total, "Period Totals" the period's debit/credit/balance,
    and "Total Due"/"Total Credit" the closing balance.  The aging table is
    nested in the Scheme document and a sibling table in the native one; both
    are read from the same row stream because the parser emits nested rows in
    document order.  The company and owner header tables are one-cell rows and
    are skipped.  With more than one A/R or A/P account the history is split
    into account blocks, which the "Account:" heading row keeps in the claim.
    """
    out = []
    for cells in rows:
        texts = [" ".join(t.split()) for t, _ in cells]
        if not texts:
            continue
        if len(cells) == 8 and DATE.fullmatch(texts[0]):
            out.append(("ROW",) + tuple(texts[:5]) +
                       tuple(amount(texts[i]) for i in range(5, 8)))
            continue
        if texts[0] == "Period Totals":
            numbers = tuple(amount(t) for t, c in cells if is_number(c))
            out.append(("TOTAL", "Period Totals") + numbers)
            continue
        if texts[0] in ("Total Due", "Total Credit"):
            numbers = tuple(amount(t) for t, c in cells if is_number(c))
            out.append(("TOTAL", texts[0]) + numbers)
            continue
        if len(cells) == 7 and texts[0] == "Pre-Payment":
            out.append(("AGING_HEAD",) + tuple(texts))
            continue
        if len(cells) == 7 and all(t and re.search(r"[0-9]", t) and
                                   not DATE.fullmatch(t) for t in texts):
            out.append(("AGING",) + tuple(amount(t) for t in texts))
            continue
        if len(cells) == 1 and texts[0].startswith("Account:"):
            out.append(("HEADING", texts[0]))
    return out


def customer_summary(rows):
    """The Customer Summary: (customer, profit, markup, sales, expense).

    The Scheme document has one five-cell row per customer and per commodity,
    the "No Customer" row for the splits no customer owns, a one-cell rule row
    and the Total row.  Markup is printed with a percent sign; amount() drops
    it, so "100%" and "100" are the same number here.  The header row and the
    company table are not five-cell amount rows and fall out.
    """
    out = []
    for cells in rows:
        texts = [" ".join(t.split()) for t, _ in cells]
        if len(cells) != 5 or not texts[0] or texts[0] == "Customer":
            continue
        out.append((texts[0], amount(texts[1]), amount(texts[2]),
                    amount(texts[3]), amount(texts[4])))
    return out


EXTRACTORS = {"trial-balance": trial_balance, "balance-sheet": balance_sheet,
              "transaction-report": transaction_report,
              "general-ledger": general_ledger, "aging": aging,
              "investment-lots": investment_lots,
              "budget-report": budget_report,
              "budget-statement": budget_statement,
              "invoice": invoice, "receipt": receipt,
              "tax-invoice": tax_invoice, "tax-schedule": tax_schedule,
              "owner-report": owner_report,
              "customer-summary": customer_summary}

extractor = EXTRACTORS[sys.argv[3]]
report = sys.argv[4]
# The document extractors scope the parse to the entries of their document:
# the printable invoice's div, the receipt's products div, and the tax
# invoice's table.  All of them have header tables (client, company, details)
# that must not be mistaken for line items.
SCOPES = {"invoice": "entries-table", "receipt": "products",
          "tax-invoice": "entries"}
scope = SCOPES.get(sys.argv[3])
# The tax schedule's totals are built in nested tables, so that parser must
# stay at the outer table's rows and treat the nested markup as cell text.
top_level = sys.argv[3] == "tax-schedule"
scheme = extract(extractor, sys.argv[1], scope, top_level)
native = extract(extractor, sys.argv[2], scope, top_level)

print(f"report: {report}")
print(f"scheme rows: {len(scheme)}")
print(f"native rows: {len(native)}")

if os.environ.get("PARITY_DUMP"):
    for name, extract in (("scheme", scheme), ("native", native)):
        print(f"--- {name} extract")
        for row in extract[:25]:
            print("   ", row)
        if len(extract) > 25:
            print(f"    ... {len(extract)} rows")

if not scheme and not native:
    print("MATCH: both reports are empty")
    sys.exit(0)

if scheme == native:
    print(f"MATCH: {len(scheme)} rows")
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
