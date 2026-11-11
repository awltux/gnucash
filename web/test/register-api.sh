#!/bin/bash
# The read-only register API: GET /api/register/<account-guid>.
#
# Usage: register-api.sh <build-dir> <source-dir>
#
# Generates the 190-row demo book with web/test/make-demo-book.py, starts
# gnc-webd read-only on it with a throwaway report store, and checks that the
# JSON rows are the engine's own view, not a second calculation:
#
#   * every leaf account's rows parse, and each row carries the fields the
#     page shows (split and transaction GUID, date, num, description, memo,
#     transfer account and its GUID, reconcile state, amount, running balance);
#   * the running balances are cumulative (each row is the previous row plus
#     its amount), and the last one equals both the JSON's own balance and
#     xaccAccountGetBalance() as printed by /api/accounts for an asset leaf;
#   * the row count equals the server-rendered register page's row count;
#   * an unknown GUID is 400 and names the GUID; an empty account is an empty
#     split array, not an error;
#   * GET /api/transaction/<guid> returns every split of the row's transaction,
#     and an unknown transaction is 400 naming it.
#
# Exit status: 0 when every check passes, 1 otherwise.
set -u

BUILD=${1:?usage: register-api.sh <build-dir> <source-dir>}
SRC=${2:?usage: register-api.sh <build-dir> <source-dir>}

SCHEMAS="$BUILD/share/glib-2.0/schemas"
CCACHE=$(ls -d "$BUILD"/lib/*/guile/3.0/site-ccache 2>/dev/null | head -1)

fail() { echo "FAIL: $*" >&2; exit 1; }

[ -x "$BUILD/bin/gnc-webd" ] || fail "gnc-webd was not built"
[ -f "$SRC/web/test/make-demo-book.py" ] || fail "make-demo-book.py not found"
command -v curl >/dev/null || fail "curl is required"
command -v python3 >/dev/null || fail "python3 is required"

if [ ! -f "$SCHEMAS/gschemas.compiled" ]; then
    echo "note: no compiled schemas in $SCHEMAS (running with defaults)"
    SCHEMAS=""
fi

TMP=$(mktemp -d)
LOG="$TMP/server.log"
cleanup() {
    if [ -n "${SERVER_PID:-}" ]; then kill "$SERVER_PID" 2>/dev/null; fi
    rm -rf "$TMP"
}
trap cleanup EXIT

# A 190-row register, so ordering and running balances are checked on more
# than a single row.  The generator needs no GnuCash libraries.
BOOK="$TMP/demo.gnucash"
python3 "$SRC/web/test/make-demo-book.py" "$BOOK" >/dev/null \
    || fail "could not generate the demo book"

env GNC_UNINSTALLED=YES GNC_BUILDDIR="$BUILD" \
    ${SCHEMAS:+GSETTINGS_SCHEMA_DIR="$SCHEMAS"} \
    LD_LIBRARY_PATH="$BUILD/lib:$BUILD/lib/gnucash" \
    GNC_MODULE_PATH="$BUILD/lib/gnucash" \
    GUILE_LOAD_PATH="$BUILD/share/guile/site/3.0" \
    GUILE_LOAD_COMPILED_PATH="${CCACHE:-}" \
    GNC_WEBUI_DIR="$SRC/web/ui" \
    GNC_WEBUI_REPORTS="$TMP/reports.json" \
    "$BUILD/bin/gnc-webd" "$BOOK" > "$LOG" 2>&1 &
SERVER_PID=$!

URL=""
for _ in $(seq 1 300); do
    URL=$(grep -oE 'http://127\.0\.0\.1:[0-9]+/[0-9a-f]+/' "$LOG" | head -1)
    [ -n "$URL" ] && break
    kill -0 "$SERVER_PID" 2>/dev/null || break
    sleep 0.2
done
[ -n "$URL" ] || { cat "$LOG" >&2; fail "server did not start"; }
echo "server: $URL"

# request <path> -- sets CODE and BODY (the body is left in $TMP/body).
CODE=""
BODY=""
request() {
    local path=$1
    CODE=$(curl -sS -o "$TMP/body" -w '%{http_code}' "${URL}${path}") \
        || fail "GET $path failed"
    BODY=$(cat "$TMP/body")
}

echo "== an unknown account is a named 400, not a 404 page"
UNKNOWN=0123456789abcdef0123456789abcdef
request "api/register/$UNKNOWN"
[ "$CODE" = "400" ] || fail "an unknown account answered $CODE (expected 400): $BODY"
echo "$BODY" | grep -q '"error":"unknown-account"' \
    || fail "the refusal is not named unknown-account: $BODY"
echo "$BODY" | grep -qF "$UNKNOWN" \
    || fail "the refusal does not name the GUID asked for: $BODY"
echo "$BODY" | grep -q '"message"' || fail "the refusal has no message: $BODY"
echo "unknown account: 400 names $UNKNOWN"

request "api/register/not-a-guid"
[ "$CODE" = "400" ] || fail "a malformed GUID answered $CODE (expected 400): $BODY"
echo "$BODY" | grep -qF "not-a-guid" \
    || fail "the malformed-GUID refusal does not name it: $BODY"
echo "malformed GUID: 400 names not-a-guid"

echo "== an unknown transaction is a named 400"
request "api/transaction/$UNKNOWN"
[ "$CODE" = "400" ] || fail "an unknown transaction answered $CODE (expected 400): $BODY"
echo "$BODY" | grep -q '"error":"unknown-transaction"' \
    || fail "the transaction refusal is not named: $BODY"
echo "$BODY" | grep -qF "$UNKNOWN" \
    || fail "the transaction refusal does not name the GUID: $BODY"
echo "unknown transaction: 400 names $UNKNOWN"

echo "== rows, running balances, and the engine's own total"
python3 - "$URL" <<'PY'
import json
import re
import sys
import urllib.error
import urllib.request
from decimal import Decimal

url = sys.argv[1]
failures = []


def check(name, ok, detail=""):
    print(("PASS: " if ok else "FAIL: ") + name +
          ((" (" + detail + ")") if detail else ""))
    if not ok:
        failures.append(name)


def fetch(path):
    """Return (status, content-type, body bytes).  4xx is data, not an error."""
    try:
        with urllib.request.urlopen(url + path) as response:
            return response.status, response.headers.get("Content-Type", ""), \
                   response.read()
    except urllib.error.HTTPError as error:
        return error.code, error.headers.get("Content-Type", ""), error.read()


def money(text):
    """Cents from an engine amount like '$1,234.56', '-$12.00' or '(£3.00)'.

    The engine may wrap the number in Unicode bidi isolates; they are not part
    of the value.  Returns None for an unparseable amount.
    """
    if text is None:
        return None
    text = text.strip()
    negative = False
    if text.startswith("(") and text.endswith(")"):
        negative = True
        text = text[1:-1]
    text = re.sub(r"[^0-9,.\-]", "", text)
    text = text.replace(",", "")
    if text.startswith("-"):
        negative = not negative
        text = text[1:]
    if not text:
        return None
    value = int((Decimal(text) * 100).to_integral_value())
    return -value if negative else value


status, content_type, body = fetch("api/accounts")
check("GET /api/accounts succeeds", status == 200, str(status))
root = json.loads(body)


def walk(node, path):
    for child in node["children"]:
        child_path = path + ":" + child["name"] if path else child["name"]
        yield child_path, child
        yield from walk(child, child_path)


leaves = list(walk(root, ""))
check("the fixture has leaf accounts", len(leaves) > 30, "%d leaves" % len(leaves))

populated = []
empty = []
bad_shapes = []
bad_running = []
bad_fields = []
bad_dates = []
json_without_balance = []

for path, account in leaves:
    if account["children"]:
        continue
    status, content_type, body = fetch("api/register/" + account["guid"])
    if status != 200 or "application/json" not in content_type:
        bad_shapes.append("%s status %s type %s" % (path, status, content_type))
        continue
    data = json.loads(body)
    if not isinstance(data, dict) or not isinstance(data.get("splits"), list) \
            or "account" not in data or "row_count" not in data:
        bad_shapes.append("%s has the wrong shape" % path)
        continue

    splits = data["splits"]
    if data["row_count"] != len(splits):
        bad_shapes.append("%s row_count %s != %d splits" %
                          (path, data["row_count"], len(splits)))
    if not splits:
        empty.append((path, account))
        continue
    populated.append((path, account, data))

    running = 0
    dates = []
    for row in splits:
        missing = [key for key in (
            "guid", "transaction_guid", "date", "date_iso", "num",
            "description", "memo", "transfer_account", "transfer_account_guid",
            "reconcile", "reconciled", "voided", "amount", "running_balance")
            if key not in row]
        if missing:
            bad_fields.append("%s missing %s" % (path, ", ".join(missing)))
            continue
        amount = money(row["amount"])
        row_running = money(row["running_balance"])
        if amount is None or row_running is None:
            bad_running.append("%s unparseable amount %r / running %r" %
                               (path, row["amount"], row["running_balance"]))
            continue
        running += amount
        if row_running != running:
            bad_running.append("%s row %s: running %d != cumulative %d" %
                               (path, row["guid"], row_running, running))
        dates.append(row["date_iso"])
    if dates != sorted(dates):
        bad_dates.append("%s: %s" % (path, dates[:4]))
    if money(data["balance"]) != running:
        json_without_balance.append("%s balance %r != cumulative %d" %
                                    (path, data["balance"], running))

check("every register has the documented shape and row_count",
      not bad_shapes, "; ".join(bad_shapes[:3]))
check("every row carries every field the page shows",
      not bad_fields, "; ".join(bad_fields[:3]))
check("every running balance is the cumulative sum of the amounts",
      not bad_running, "; ".join(bad_running[:3]))
check("every register is in ascending date order (the desktop's default)",
      not bad_dates, "; ".join(bad_dates[:3]))
check("the JSON's own balance equals the last running balance",
      not json_without_balance, "; ".join(json_without_balance[:3]))
check("the fixture has both populated and empty accounts",
      bool(populated) and bool(empty),
      "%d populated, %d empty" % (len(populated), len(empty)))
check("one register has enough rows to page through",
      bool(populated) and max(len(data["splits"]) for _, _, data in populated)
      >= 100,
      str(max((len(data["splits"]) for _, _, data in populated), default=0)) +
      " rows")

# The header the UI builds needs the account's own balance and the tree's
# subtree total, plus the flags that choose between one figure and two.  The
# subtree total must be the same number /api/accounts shows for that account;
# a placeholder's own balance is genuinely zero because every split is in a
# child, and an asset leaf's own balance is already its subtree total.
def account_at(path):
    return next((account for candidate, account in leaves
                 if candidate == path), None)


assets = account_at("Assets")
check("the fixture has the Assets placeholder account", assets is not None)
if assets is not None:
    status, content_type, body = fetch("api/register/" + assets["guid"])
    assets_register = json.loads(body)
    check("GET /api/register/Assets succeeds", status == 200, str(status))
    check("a placeholder's own balance is zero",
          money(assets_register["balance"]) == 0,
          repr(assets_register["balance"]))
    check("a placeholder's subtree total equals /api/accounts' total",
          money(assets_register["subtree_balance"]) == money(assets["total"]),
          "%r vs %r" % (assets_register["subtree_balance"], assets["total"]))
    check("a placeholder's own balance differs from its subtree total",
          assets_register["own_equals_subtree"] is False)
    check("a placeholder is flagged placeholder",
          assets_register["placeholder"] is True)
    check("a placeholder is flagged as having children",
          assets_register["has_children"] is True and
          bool(assets_register["children"]))
    check("a placeholder names its children in tree order",
          [child["guid"] for child in assets_register["children"]] ==
          [child["guid"] for child in assets["children"]],
          "%d names" % len(assets_register["children"]))

checking = account_at("Assets:Checking")
check("the fixture has the Assets:Checking leaf", checking is not None)
if checking is not None:
    status, content_type, body = fetch("api/register/" + checking["guid"])
    checking_register = json.loads(body)
    check("GET /api/register/Assets:Checking succeeds",
          status == 200, str(status))
    check("a leaf's own balance equals its subtree total",
          checking_register["own_equals_subtree"] is True and
          money(checking_register["balance"]) ==
          money(checking_register["subtree_balance"]),
          "%r vs %r" % (checking_register["balance"],
                        checking_register["subtree_balance"]))
    check("a leaf's subtree total equals /api/accounts' total",
          money(checking_register["subtree_balance"]) == money(checking["total"]),
          "%r vs %r" % (checking_register["subtree_balance"],
                        checking["total"]))
    check("a leaf is not a placeholder",
          checking_register["placeholder"] is False)
    check("a leaf has no children",
          checking_register["has_children"] is False and
          checking_register["children"] == [])

# A credit-normal parent is sign-reversed in exactly the same way as the tree:
# /api/accounts reports Liabilities as negative, and the register's subtree
# total must carry the same sign rather than the raw engine sign.
liabilities = account_at("Liabilities")
if liabilities is not None:
    status, content_type, body = fetch("api/register/" + liabilities["guid"])
    liabilities_register = json.loads(body)
    check("a credit-normal parent's subtree total keeps the tree's sign",
          money(liabilities_register["subtree_balance"]) ==
          money(liabilities["total"]) and
          liabilities_register["subtree_negative"] ==
          liabilities["negative"],
          "%r vs %r" % (liabilities_register["subtree_balance"],
                        liabilities["total"]))

# The account the gate names: a leaf asset, whose /api/accounts total is its
# own balance (no children, and assets are not sign-reversed).
named = next(((path, account, data) for path, account, data in populated
              if path == "Assets:Checking"), None)
check("the fixture has a populated leaf asset (Checking Account)",
      named is not None)
if named and empty:
    path, account, data = named
    tree_total = money(account["total"])
    last = data["splits"][-1]
    last_running = money(last["running_balance"])
    check("the last running balance equals the account balance from /api/accounts",
          tree_total is not None and last_running == tree_total,
          "%s: register %s, api/accounts %r" %
          (path, last["running_balance"], account["total"]))
    check("the last running balance equals the JSON's balance",
          last["running_balance"] == data["balance"],
          "%r vs %r" % (last["running_balance"], data["balance"]))

    status, content_type, body = fetch("register/acct-guid=" + account["guid"])
    html = body.decode("utf-8")
    page_rows = html.count("<tr") - 1  # one <tr> is the header
    check("the register page renders one row per JSON split",
          status == 200 and page_rows == len(data["splits"]),
          "%d page rows, %d JSON splits" % (page_rows, len(data["splits"])))
    match = re.search(r"Balance:\s*([^<]+)</p>", html)
    page_balance = money(match.group(1)) if match else None
    check("the register page prints the JSON's balance",
          page_balance == money(data["balance"]),
          "page %r, JSON %r" % (match.group(1) if match else None,
                                data["balance"]))

    # The transaction panel's endpoint: every split of that row's transaction.
    row = data["splits"][0]
    status, content_type, body = fetch(
        "api/transaction/" + row["transaction_guid"])
    check("GET /api/transaction/<guid> succeeds", status == 200, str(status))
    transaction = json.loads(body)
    check("the transaction carries every split and the register's account",
          isinstance(transaction.get("splits"), list) and
          len(transaction["splits"]) >= 2 and
          any(split["account_guid"] == account["guid"]
              for split in transaction["splits"]),
          "%d splits" % len(transaction.get("splits", [])))
    check("the transaction's splits are balanced",
          sum(money(split["amount"]) for split in transaction["splits"]) == 0,
          ", ".join(split["amount"] for split in transaction["splits"]))

    # A two-split row names the other account and its GUID; a split
    # transaction names the engine's placeholder and has no single account.
    two_split = [row for row in data["splits"]
                 if row["transfer_account_guid"]]
    many_split = [row for row in data["splits"]
                  if row["transfer_account"] == "-- Split Transaction --"]
    check("a two-split row names its transfer account and GUID",
          bool(two_split) and all(
              row["transfer_account"] and row["transfer_account_guid"]
              for row in two_split),
          "%d rows" % len(two_split))
    check("a split transaction uses the engine's placeholder, not a guess",
          bool(many_split) and all(
              row["transfer_account_guid"] == "" for row in many_split),
          "%d rows" % len(many_split))

    # An empty account is an empty array, not an error.
    empty_path, empty_account = empty[0]
    status, content_type, body = fetch(
        "api/register/" + empty_account["guid"])
    empty_data = json.loads(body)
    check("an account with no splits is an empty array, not an error",
          status == 200 and empty_data["splits"] == [] and
          empty_data["row_count"] == 0 and empty_data["balance"] is not None,
          "%s: status %s, splits %r" %
          (empty_path, status, empty_data["splits"]))

if failures:
    print("\n%d check(s) failed" % len(failures))
    sys.exit(1)
PY
STATUS=$?
[ "$STATUS" = "0" ] || fail "register API checks failed"

# A last look at the wire shape book-save.sh and demo-book-check.sh rely on:
# the body is an object with an account and a splits array (not a bare array).
request "api/accounts"
GUID=$(python3 - "$TMP/body" <<'PY'
import json
import sys
accounts = json.load(open(sys.argv[1]))
stack = list(accounts["children"])
while stack:
    node = stack.pop()
    if not node["children"]:
        print(node["guid"])
        break
    stack.extend(node["children"])
PY
)
request "api/register/$GUID"
echo "$BODY" | grep -q '"account":' || fail "the register body has no account field"
echo "$BODY" | grep -q '"splits":\[' || fail "the register body has no splits array"

echo
echo "register API: all checks passed"
