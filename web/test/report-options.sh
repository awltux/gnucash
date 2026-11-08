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
if [ "$FAILS" -ne 0 ]; then
    echo "$FAILS check(s) failed"
    exit 1
fi
echo "report option surface passed"
