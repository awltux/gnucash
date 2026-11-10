#!/bin/bash
# Acceptance test for the demo book the web UI is validated against.
#
# Usage: demo-book-check.sh <build-dir> <source-dir>
#
# Generates a 54-account, twelve-month household book with
# web/test/make-demo-book.py, starts gnc-webd on it exactly as
# web/run-smoke-test.sh does, and compares the engine's own numbers with the
# arithmetic the generator recorded in <book>.expect.json.  The point is that
# a fixture whose accounts are all $0.00 cannot tell a broken account from an
# inactive one; this one can, and both the generator and the engine are checked
# against each other:
#
#   * the account and top-level counts from /api/book and the account tree;
#   * the raw total of every top-level account and of the named accounts,
#     from /api/accounts (GnuCash may display credit-normal accounts with the
#     opposite sign depending on the "reversed accounts" preference, so either
#     whole-book convention is accepted -- but not a different total);
#   * the deliberately empty accounts really are empty;
#   * one account's register page, by HTML row count and by the balance it
#     prints, plus every account's split count from /api/register, which is
#     how the loaded transaction count is verified (there is no transaction
#     counting endpoint);
#   * the Trial Balance, Profit & Loss and Balance Sheet render with the rows
#     and headings the book implies;
#   * the server log has no book-load warning (the build machines have no
#     compiled GSettings schemas, so the known preferences noise is noted and
#     ignored).
#
# The book and its expectation file live in a temporary directory outside the
# repository; KEEP_DEMO_BOOK=1 keeps them for inspection.
set -u

BUILD=${1:?usage: demo-book-check.sh <build-dir> <source-dir>}
SRC=${2:?usage: demo-book-check.sh <build-dir> <source-dir>}

fail() { echo "FAIL: $*" >&2; exit 1; }

command -v python3 >/dev/null 2>&1 || fail "python3 is required"
command -v curl >/dev/null 2>&1 || fail "curl is required"

[ -x "$BUILD/bin/gnc-webd" ] || fail "gnc-webd was not built in $BUILD"
[ -f "$SRC/web/test/make-demo-book.py" ] \
    || fail "generator not found at $SRC/web/test/make-demo-book.py"
BACKEND="$BUILD/lib/gnucash/libgncmod-backend-xml.so"
[ -f "$BACKEND" ] || fail "the XML backend was not built ($BACKEND)"
SCHEMAS="$BUILD/share/glib-2.0/schemas"
CCACHE=$(ls -d "$BUILD"/lib/*/guile/3.0/site-ccache 2>/dev/null | head -1)

if [ ! -f "$SCHEMAS/gschemas.compiled" ]; then
    # The schemas live in the GNOME tree; these headless builds have none.
    # The server works from defaults, but say so rather than pretend all is
    # well -- the log's gnc.app-utils noise below has this cause.
    echo "note: no compiled schemas in $SCHEMAS (running with defaults)"
    SCHEMAS=""
fi

if [ -n "${GNUCASH_DEMO_DIR:-}" ]; then
    WORK="$GNUCASH_DEMO_DIR"
    mkdir -p "$WORK" || fail "cannot create $WORK"
    CLEAN_WORK=0
else
    WORK=$(mktemp -d "${TMPDIR:-/tmp}/gnucash-demo.XXXXXX") \
        || fail "cannot create a temporary directory"
    CLEAN_WORK=1
fi

BOOK="$WORK/demo.gnucash"
EXPECT="$BOOK.expect.json"
LOG="$WORK/server.log"
REGISTERS="$WORK/registers"
mkdir -p "$REGISTERS"
SERVER_PID=""

cleanup() {
    if [ -n "$SERVER_PID" ]; then
        kill "$SERVER_PID" 2>/dev/null
        wait "$SERVER_PID" 2>/dev/null
    fi
    if [ "$CLEAN_WORK" = 1 ] && [ -z "${KEEP_DEMO_BOOK:-}" ]; then
        rm -rf "$WORK"
    fi
}
trap cleanup EXIT

# ---------------------------------------------------------------- generate
echo "== generating the demo book"
python3 "$SRC/web/test/make-demo-book.py" "$BOOK" \
    || fail "make-demo-book.py failed"
[ -f "$EXPECT" ] || fail "the generator did not write $EXPECT"

# ------------------------------------------------------------- start server
echo "== starting gnc-webd"
env GNC_UNINSTALLED=YES GNC_BUILDDIR="$BUILD" \
    ${SCHEMAS:+GSETTINGS_SCHEMA_DIR="$SCHEMAS"} \
    LD_LIBRARY_PATH="$BUILD/lib:$BUILD/lib/gnucash" \
    GNC_MODULE_PATH="$BUILD/lib/gnucash" \
    GUILE_LOAD_PATH="$BUILD/share/guile/site/3.0" \
    GUILE_LOAD_COMPILED_PATH="${CCACHE:-}" \
    GNC_WEBUI_DIR="$SRC/web/ui" \
    GNC_WEBUI_REPORTS="$WORK/specs" \
    "$BUILD/bin/gnc-webd" --backend "$BACKEND" "$BOOK" > "$LOG" 2>&1 &
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

# ------------------------------------------------------------------- fetch
curl -fsS "${URL}api/book" -o "$WORK/book.json" \
    || { cat "$LOG" >&2; fail "GET api/book failed"; }
curl -fsS "${URL}api/accounts" -o "$WORK/accounts.json" \
    || { cat "$LOG" >&2; fail "GET api/accounts failed"; }

START=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["period"]["start"])' "$EXPECT") \
    || fail "cannot read the period start from $EXPECT"
END=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["period"]["end"])' "$EXPECT") \
    || fail "cannot read the period end from $EXPECT"

# The register page the checker looks at is the one the expectations name.
REGISTER_GUID=$(python3 - "$EXPECT" "$WORK/accounts.json" <<'PY'
import json
import sys

expect = json.load(open(sys.argv[1]))
root = json.load(open(sys.argv[2]))
node = root
for part in expect["register_account"].split(":"):
    matches = [child for child in node["children"] if child["name"] == part]
    if len(matches) != 1:
        raise SystemExit("cannot find " + expect["register_account"])
    node = matches[0]
print(node["guid"])
PY
) || fail "cannot locate the register account in api/accounts"
curl -fsS "${URL}register/acct-guid=$REGISTER_GUID" -o "$WORK/register.html" \
    || { cat "$LOG" >&2; fail "GET the register page failed"; }

# Every account's split list, used to prove every transaction's splits loaded.
python3 - "$WORK/accounts.json" > "$WORK/guids.txt" <<'PY'
import json
import sys

root = json.load(open(sys.argv[1]))


def walk(node, path):
    for child in node["children"]:
        child_path = path + ":" + child["name"] if path else child["name"]
        print(child["guid"] + "\t" + child_path)
        walk(child, child_path)


walk(root, "")
PY
while IFS=$'\t' read -r guid path; do
    curl -fsS "${URL}api/register/$guid" -o "$REGISTERS/$guid.json" \
        || { cat "$LOG" >&2; fail "GET api/register for $path failed"; }
done < "$WORK/guids.txt"

curl -fsS --get --data-urlencode "end=$END" \
    "${URL}report/Trial%20Balance" -o "$WORK/trial-balance.html" \
    || { cat "$LOG" >&2; fail "GET Trial Balance failed"; }
curl -fsS --get --data-urlencode "start=$START" --data-urlencode "end=$END" \
    "${URL}report/Profit%20%26%20Loss" -o "$WORK/profit-loss.html" \
    || { cat "$LOG" >&2; fail "GET Profit & Loss failed"; }
curl -fsS --get --data-urlencode "end=$END" \
    "${URL}report/Balance%20Sheet" -o "$WORK/balance-sheet.html" \
    || { cat "$LOG" >&2; fail "GET Balance Sheet failed"; }

# --------------------------------------------------------------- assertions
echo "== comparing the engine with the generator's arithmetic"
python3 - "$EXPECT" "$BOOK" "$WORK/book.json" "$WORK/accounts.json" \
    "$WORK/register.html" "$REGISTERS" "$WORK/guids.txt" \
    "$WORK/trial-balance.html" "$WORK/profit-loss.html" \
    "$WORK/balance-sheet.html" "$LOG" <<'PY'
import json
import re
import sys
from decimal import Decimal

(expect_path, book_path, book_json_path, accounts_json_path,
 register_html_path, registers_dir, guids_path, trial_path, profit_loss_path,
 balance_sheet_path, log_path) = sys.argv[1:12]

expect = json.load(open(expect_path, encoding="utf-8"))
failures = []


def check(name, ok, detail=""):
    if ok:
        print("PASS: " + name + ((" (" + detail + ")") if detail else ""))
    else:
        print("FAIL: " + name + ((" -- " + detail) if detail else ""))
        failures.append(name)


def parse_money(text):
    """Cents from an engine amount such as '$1,234.56' or '-$12.00'.

    The amounts carry Unicode bidi isolates and a currency symbol; none of
    them is part of the number.
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


def money_text(cents):
    if cents is None:
        return "missing"
    sign = "-" if cents < 0 else ""
    cents = abs(cents)
    return "%s$%d.%02d" % (sign, cents // 100, cents % 100)


def read(path):
    with open(path, encoding="utf-8") as handle:
        return handle.read()


root = json.load(open(accounts_json_path, encoding="utf-8"))


def find(path):
    node = root
    for part in path.split(":"):
        matches = [child for child in node["children"] if child["name"] == part]
        if len(matches) != 1:
            return None
        node = matches[0]
    return node


def count_nodes(node):
    return sum(1 + count_nodes(child) for child in node["children"])


def credit_normal(path):
    return path.split(":", 1)[0] in ("Liabilities", "Equity", "Income")


# ---------------------------------------------------------------- the book
book = json.load(open(book_json_path, encoding="utf-8"))
check("api/book account count", book.get("accounts") == expect["accounts"],
      "engine %r, generator %r" % (book.get("accounts"), expect["accounts"]))
check("api/book top-level count",
      book.get("top_level") == expect["top_level_count"],
      "engine %r, generator %r" % (book.get("top_level"),
                                   expect["top_level_count"]))
check("api/accounts tree size", count_nodes(root) == expect["accounts"],
      "engine %r, generator %r" % (count_nodes(root), expect["accounts"]))

# The fixture itself: the declared transaction count and a zero sum per
# transaction.  This catches a bad generator before blaming the engine.
from xml.etree import ElementTree

ns = {"gnc": "http://www.gnucash.org/XML/gnc",
      "trn": "http://www.gnucash.org/XML/trn",
      "split": "http://www.gnucash.org/XML/split"}
tree = ElementTree.parse(book_path)
transactions = tree.getroot().findall(".//gnc:transaction", ns)
check("fixture transaction count",
      len(transactions) == expect["transactions"],
      "%d in the XML, generator says %d" % (len(transactions),
                                            expect["transactions"]))

unbalanced = []
for transaction in transactions:
    total = 0
    for split in transaction.findall(".//trn:split", ns):
        value = split.find("split:value", ns).text.strip()
        numerator, denominator = (int(part) for part in value.split("/", 1))
        total += numerator * 100 // denominator
    if total != 0:
        number = transaction.find("trn:num", ns).text
        unbalanced.append("%s:%d" % (number, total))
check("fixture transactions balance", not unbalanced,
      "%d transactions, all sum to zero" % len(transactions)
      if not unbalanced else ", ".join(unbalanced[:5]))

# ------------------------------------------------------------ the balances
raw_totals = {path: parse_money(find(path)["total"])
              for path in expect["top_level_totals"]}


def expected_display(cents, path, convention):
    if convention == "credit" and credit_normal(path):
        return -cents
    return cents


def convention_matches(convention, totals):
    return all(raw_totals[path] == expected_display(cents, path, convention)
               for path, cents in totals.items())


raw_only = convention_matches("none", expect["top_level_totals"])
credit = convention_matches("credit", expect["top_level_totals"])
convention = "none" if raw_only else "credit"
top_level_detail = ", ".join(
    "%s %s (generator %s)" % (
        path, money_text(raw_totals[path]),
        money_text(expected_display(cents, path, convention))
        if (raw_only or credit) else money_text(cents))
    for path, cents in expect["top_level_totals"].items())
check("top-level totals", raw_only or credit,
      ("matched the %s sign convention: " % convention
       if (raw_only or credit) else "no whole-book convention matched: ")
      + top_level_detail)

named_actual = {}
for path, spec in expect["named_accounts"].items():
    node = find(path)
    if node is None:
        named_actual[path] = None
        continue
    named_actual[path] = parse_money(node["total"])
bad = [path for path, spec in expect["named_accounts"].items()
       if named_actual[path] != expected_display(spec["total"], path,
                                                 convention)]
check("named account totals", not bad,
      "; ".join("%s engine %s, generator %s" %
                (path, money_text(named_actual[path]),
                 money_text(expected_display(
                     expect["named_accounts"][path]["total"],
                     path, convention)))
                for path in (bad or list(expect["named_accounts"]))))

bad_empty = []
for path in expect["empty_accounts"]:
    node = find(path)
    actual = parse_money(node["total"]) if node else None
    if actual != 0:
        bad_empty.append("%s=%s" % (path, actual))
check("deliberately empty accounts", not bad_empty,
      "all %d print zero" % len(expect["empty_accounts"])
      if not bad_empty else ", ".join(bad_empty))

# ------------------------------------------------------------- the register
register = read(register_html_path)
register_rows = register.count("<tr") - 1       # one <tr> is the header
register_account = expect["register_account"]
expected_entries = expect["named_accounts"][register_account]["splits"]
check("register page entries", register_rows == expected_entries,
      "%d rows, generator says %d splits" %
      (register_rows, expected_entries))

match = re.search(r"Balance:\s*([^<]+)</p>", register)
if match is None:
    check("register page balance", False, "no Balance line in the page")
else:
    balance = parse_money(match.group(1))
    expected_balance = expected_display(
        expect["named_accounts"][register_account]["total"],
        register_account, convention)
    check("register page balance", balance == expected_balance,
          "engine %s, generator %s" % (money_text(balance),
                                       money_text(expected_balance)))

# Every split of every transaction, account by account.  The engine has no
# transaction-count endpoint, so this is the load check that matters: a
# transaction that failed to load loses its splits here.
guid_to_path = {}
for line in read(guids_path).splitlines():
    guid, path = line.split("\t", 1)
    guid_to_path[guid] = path

bad_counts = []
total_splits = 0
for guid, path in guid_to_path.items():
    data = json.load(open(registers_dir + "/" + guid + ".json",
                          encoding="utf-8"))
    actual = len(data.get("splits", []))
    total_splits += actual
    if actual != expect["split_counts"][path]:
        bad_counts.append("%s=%d (expected %d)" %
                          (path, actual, expect["split_counts"][path]))
check("per-account split counts", not bad_counts,
      "%d accounts, %d splits" % (len(guid_to_path), total_splits)
      if not bad_counts else ", ".join(bad_counts[:5]))
check("total splits loaded", total_splits == expect["splits"],
      "engine %d, generator %d" % (total_splits, expect["splits"]))

# --------------------------------------------------------------- reports
def report_rows(text):
    # render_html emits one <tr> for the header and one per data row.
    return text.count("<tr") - 1


def total_amount(html, label):
    """The amount in the engine's one-amount total row labelled `label`."""
    pattern = (r"<tr><td class='total-label-cell'>" + re.escape(label) +
               r"</td><td class='total-number-cell'>([^<]*)</td></tr>")
    match = re.search(pattern, html)
    return parse_money(match.group(1)) if match else None


trial = read(trial_path)
check("Trial Balance rendered",
      "<html" in trial and "Trial Balance" in trial)
check("Trial Balance rows",
      report_rows(trial) == expect["reports"]["Trial Balance"]["data_rows"],
      "%d data rows, generator says %d" %
      (report_rows(trial), expect["reports"]["Trial Balance"]["data_rows"]))
check("Trial Balance amount cells", trial.count("number-cell") > 10,
      "%d amount cells" % trial.count("number-cell"))
tb_totals = re.search(
    r"<tr><td class='total-label-cell'></td>"
    r"<td class='total-number-cell'>([^<]*)</td>"
    r"<td class='total-number-cell'>([^<]*)</td></tr>", trial)
if tb_totals is None:
    check("Trial Balance debit = credit", False, "no total row")
else:
    debit = parse_money(tb_totals.group(1))
    credit = parse_money(tb_totals.group(2))
    check("Trial Balance debit = credit", debit == credit and debit > 0,
          "debit %s, credit %s" % (money_text(debit), money_text(credit)))

profit_loss = read(profit_loss_path)
check("Profit & Loss rendered",
      "<html" in profit_loss and "Profit &amp; Loss" in profit_loss)
minimum = expect["reports"]["Profit & Loss"]["min_data_rows"]
check("Profit & Loss rows", report_rows(profit_loss) >= minimum,
      "%d data rows (at least %d)" % (report_rows(profit_loss), minimum))
revenue = total_amount(profit_loss, "Total Revenue")
expenses = total_amount(profit_loss, "Total Expenses")
expected_revenue = -expect["top_level_totals"]["Income"]
expected_expenses = expect["top_level_totals"]["Expenses"]
net = expected_revenue - expected_expenses
net_label = "Net income for Period" if net >= 0 else "Net loss for Period"
net_match = re.search(
    r"<tr><td class='text-cell'>" + re.escape(net_label) +
    r"</td><td class='number-cell'>([^<]*)</td></tr>", profit_loss)
net_amount = parse_money(net_match.group(1)) if net_match else None
check("Profit & Loss amounts",
      revenue == expected_revenue and expenses == expected_expenses
      and net_match is not None and net_amount == abs(net),
      "revenue %s/%s, expenses %s/%s, %s %s/%s" %
      (money_text(revenue), money_text(expected_revenue),
       money_text(expenses), money_text(expected_expenses), net_label,
       money_text(net_amount), money_text(abs(net))))

balance_sheet = read(balance_sheet_path)
check("Balance Sheet rendered",
      "<html" in balance_sheet and "Balance Sheet" in balance_sheet)
minimum = expect["reports"]["Balance Sheet"]["min_data_rows"]
check("Balance Sheet rows", report_rows(balance_sheet) >= minimum,
      "%d data rows (at least %d)" % (report_rows(balance_sheet), minimum))
assets = total_amount(balance_sheet, "Total Assets")
liabilities = total_amount(balance_sheet, "Total Liabilities")
total_liabilities_equity = total_amount(balance_sheet,
                                        "Total Liabilities &amp; Equity")
expected_assets = expect["top_level_totals"]["Assets"]
expected_liabilities = -expect["top_level_totals"]["Liabilities"]
check("Balance Sheet amounts",
      assets == expected_assets and liabilities == expected_liabilities
      and total_liabilities_equity == expected_assets,
      "assets %s/%s, liabilities %s/%s, liabilities+equity %s/%s" %
      (money_text(assets), money_text(expected_assets),
       money_text(liabilities), money_text(expected_liabilities),
       money_text(total_liabilities_equity), money_text(expected_assets)))

# ---------------------------------------------------------------- the log
# Book-load warnings must be absent.  The schema-less headless builds emit a
# known, unrelated block of GSettings assertions; count it and ignore it.
log = read(log_path)
noise = ("gnc.app-utils", "reversed account preference set")
noise_lines = [line for line in log.splitlines()
               if any(marker in line for marker in noise)]
bad_log = [line for line in log.splitlines()
           if re.search(r"\b(ERROR|WARN|CRITICAL)\b", line)
           and not any(marker in line for marker in noise)]
check("book loads without warnings", not bad_log,
      "no book warnings; %d build-environment GSettings lines ignored" %
      len(noise_lines)
      if not bad_log else " | ".join(bad_log[:3]))

print()
if failures:
    print("demo book check FAILED (%d assertion%s)" %
          (len(failures), "" if len(failures) == 1 else "s"))
    sys.exit(1)
print("demo book check passed")
PY

STATUS=$?
if [ "$STATUS" -ne 0 ]; then
    fail "one or more assertions failed"
fi
