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
        print(option["name"])
PY

    # A real baseline for the reports that need an account selection: their
    # date options do nothing while the report is the empty-selection warning.
    base=""
    if grep -q '"name":"accounts"' "$OPTDIR/options.json"; then
        base="accounts=all"
    fi
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
    esac

    while IFS= read -r opt; do
        [ "$opt" = "currency" ] && continue
        case "$opt" in
            start) probe="start=2018-12-31" ;;
            end) probe="end=2030-12-31" ;;
            accounts) probe="accounts=all" ;;
            period) probe="period=all" ;;
            *) bad "$report advertises unknown option '$opt'"; continue ;;
        esac

        if [ "$opt" = "accounts" ]; then
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
