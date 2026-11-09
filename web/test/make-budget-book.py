#!/usr/bin/env python3
"""Write a GnuCash XML book with a budget, for the budget report tests.

No fixture book in the source tree carries budget values (the only one with a
budget at all, ``libgnucash/backend/dbi/test/test-dbi.xml``, has 12 empty
periods and no accounts), so the native Budget Report has nothing to compare
against.  This script builds a book outside the repository with:

  * four top-level accounts -- Assets, Liabilities, Income, Expenses -- of
    exactly the types the Scheme budget report's default account selection
    accepts (ASSET, LIABILITY, INCOME, EXPENSE), plus two children under each
    of the first two, and a third level under Expenses: Food: Groceries.  The
    Scheme default display depth is two levels, so Groceries is hidden but
    still feeds its parent's rolled-up budget and recursive actual;
  * a three-month budget, January to March 2026, with values on the leaves
    (Checking, Credit Card, Salary, Interest, Rent, Groceries) and on one
    parent (Food).  A parent with its own value is the budget model's
    "inclusive" rule, so Food's value wins over Groceries'; Assets has no
    value of its own, so its row must sum its children.  Savings has neither
    a budget nor an actual, so its row is all zeros;
  * transactions in every budget period, one on each period boundary
    (2026-01-01 and 2026-03-31) and one outside the budget (2025-12-15), so
    the period cut is testable;
  * one closing transaction (``book_closing`` KVP) in January, which both
    engines must keep out of the actual amounts.

The budget is encoded the way the engine stores it: a ``gnc:budget`` object
with a monthly recurrence that starts on the first period's first day, and a
``bgt:slots`` frame whose key is the account GUID and whose value is a frame
of period index -> numeric amount.  Periods without a slot are unset, which
the report prints as "." in the Budget column (not as a zero).

Usage:
    make-budget-book.py <output.gnucash>

The book is deliberately plain: one currency (USD), no prices, no lots.
Run it with python3; it needs no GnuCash libraries.
"""
import sys
import uuid

USD_SCALE = 100
PERIODS = ["2026-01-01", "2026-02-01", "2026-03-01"]
BUDGET_NAME = "Three Month Budget"


def guid():
    return uuid.uuid4().hex


def ts(day, time="10:59:00"):
    return f"{day} {time} +0000"


def money(cents):
    """A gnc_numeric as the XML writes it: value over the currency fraction."""
    return f"{cents}/{USD_SCALE}"


class Account:
    def __init__(self, name, type_, parent=None, placeholder=False):
        self.guid = guid()
        self.name = name
        self.type = type_            # ASSET, LIABILITY, INCOME, EXPENSE
        self.parent = parent
        self.placeholder = placeholder


class Transaction:
    def __init__(self, number, posted, description, splits,
                 closing=False):
        self.guid = guid()
        self.number = number
        self.posted = posted
        self.description = description
        self.splits = splits         # list of (account, cents, memo, action)
        self.closing = closing


def build(out_path):
    # ----------------------------------------------------------- accounts
    assets = Account("Assets", "ASSET", placeholder=True)
    liabilities = Account("Liabilities", "LIABILITY", placeholder=True)
    income = Account("Income", "INCOME", placeholder=True)
    expenses = Account("Expenses", "EXPENSE", placeholder=True)

    checking = Account("Checking", "ASSET", assets)
    savings = Account("Savings", "ASSET", assets)
    credit_card = Account("Credit Card", "LIABILITY", liabilities)
    salary = Account("Salary", "INCOME", income)
    interest = Account("Interest", "INCOME", income)
    rent = Account("Rent", "EXPENSE", expenses)
    food = Account("Food", "EXPENSE", expenses)
    groceries = Account("Groceries", "EXPENSE", food)

    accounts = [assets, liabilities, income, expenses,
                checking, savings, credit_card, salary, interest,
                rent, food, groceries]

    # ----------------------------------------------------------- budget
    # account -> {period index: cents}.  A missing period is unset.
    budget = {
        checking:    {0: 100000, 1: 110000, 2: 120000},
        credit_card: {0: 50000, 2: 60000},
        salary:      {0: 500000, 1: 500000, 2: 500000},
        interest:    {1: 10000, 2: 10000},
        rent:        {0: 150000, 1: 150000, 2: 150000},
        # Food has its own value, so its children's values are not summed:
        # this is the budget model's "values are inclusive of sub-accounts".
        food:        {0: 70000, 1: 80000, 2: 90000},
        groceries:   {0: 40000, 1: 50000, 2: 60000},
    }

    # ------------------------------------------------------- transactions
    transactions = [
        # January.  The two boundary-day splits are inside their period.
        Transaction("T-001", "2026-01-01", "Groceries", [
            (groceries, 5000, "January groceries", "Buy"),
            (checking, -5000, "", ""),
        ]),
        Transaction("T-002", "2026-01-05", "Salary", [
            (checking, 480000, "", "Deposit"),
            (salary, -480000, "", ""),
        ]),
        Transaction("T-003", "2026-01-10", "Groceries", [
            (groceries, 65000, "January groceries", "Buy"),
            (checking, -65000, "", ""),
        ]),
        Transaction("T-004", "2026-01-15", "Rent", [
            (rent, 145000, "January rent", "Buy"),
            (checking, -145000, "", ""),
        ]),
        Transaction("T-005", "2026-01-20", "Card purchase", [
            (groceries, 20000, "On the card", "Buy"),
            (credit_card, -20000, "", ""),
        ]),
        # A closing transaction: its splits must not reach the actual column.
        Transaction("T-006", "2026-01-31", "Closing Entries", [
            (salary, 5000, "", ""),
            (rent, -5000, "", ""),
        ], closing=True),
        # February.
        Transaction("T-007", "2026-02-03", "Salary", [
            (checking, 510000, "", "Deposit"),
            (salary, -510000, "", ""),
        ]),
        Transaction("T-008", "2026-02-10", "Rent", [
            (rent, 150000, "February rent", "Buy"),
            (checking, -150000, "", ""),
        ]),
        Transaction("T-009", "2026-02-14", "Groceries", [
            (groceries, 75000, "February groceries", "Buy"),
            (checking, -75000, "", ""),
        ]),
        Transaction("T-010", "2026-02-20", "Interest", [
            (checking, 12000, "", "Deposit"),
            (interest, -12000, "", ""),
        ]),
        # March.  A split on the last day of the last period is inside it.
        Transaction("T-011", "2026-03-02", "Salary", [
            (checking, 500000, "", "Deposit"),
            (salary, -500000, "", ""),
        ]),
        Transaction("T-012", "2026-03-10", "Rent", [
            (rent, 150000, "March rent", "Buy"),
            (checking, -150000, "", ""),
        ]),
        Transaction("T-013", "2026-03-20", "Groceries", [
            (groceries, 85000, "March groceries", "Buy"),
            (checking, -85000, "", ""),
        ]),
        Transaction("T-014", "2026-03-31", "Groceries", [
            (groceries, 6000, "March groceries", "Buy"),
            (checking, -6000, "", ""),
        ]),
        # Before the budget: must not appear in any period column.
        Transaction("T-015", "2025-12-15", "Rent", [
            (rent, 99900, "Outside the budget", "Buy"),
            (checking, -99900, "", ""),
        ]),
    ]

    # --------------------------------------------------------------- write
    out = []
    w = out.append

    w('<?xml version="1.0" encoding="utf-8" ?>')
    w('<gnc-v2')
    namespaces = (
        ("gnc", "gnc"), ("act", "act"), ("book", "book"), ("cd", "cd"),
        ("cmdty", "cmdty"), ("price", "price"), ("slot", "slot"),
        ("split", "split"), ("sx", "sx"), ("trn", "trn"), ("ts", "ts"),
        ("fs", "fs"), ("bgt", "bgt"), ("recurrence", "recurrence"),
        ("lot", "lot"), ("addr", "addr"), ("billterm", "billterm"),
        ("bt-days", "bt-days"), ("bt-prox", "bt-prox"), ("cust", "cust"),
        ("employee", "employee"), ("entry", "entry"), ("invoice", "invoice"),
        ("job", "job"), ("order", "order"), ("owner", "owner"),
        ("taxtable", "taxtable"), ("tte", "tte"), ("vendor", "vendor"))
    for index, (prefix, uri) in enumerate(namespaces):
        closing = ">" if index == len(namespaces) - 1 else ""
        w(f'     xmlns:{prefix}="http://www.gnucash.org/XML/{uri}"{closing}')
    w('<gnc:count-data cd:type="book">1</gnc:count-data>')
    w('<gnc:book version="2.0.0">')
    w(f'<book:id type="guid">{guid()}</book:id>')
    w('<gnc:count-data cd:type="commodity">1</gnc:count-data>')
    w(f'<gnc:count-data cd:type="account">{len(accounts)}</gnc:count-data>')
    w(f'<gnc:count-data cd:type="transaction">{len(transactions)}'
      '</gnc:count-data>')
    w('<gnc:count-data cd:type="budget">1</gnc:count-data>')

    w('<gnc:commodity version="2.0.0">')
    w('  <cmdty:space>CURRENCY</cmdty:space>')
    w('  <cmdty:id>USD</cmdty:id>')
    w('  <cmdty:name>US Dollar</cmdty:name>')
    w('  <cmdty:fraction>100</cmdty:fraction>')
    w('</gnc:commodity>')

    for account in accounts:
        w('<gnc:account version="2.0.0">')
        w(f'  <act:name>{account.name}</act:name>')
        w(f'  <act:id type="guid">{account.guid}</act:id>')
        w(f'  <act:type>{account.type}</act:type>')
        w('  <act:commodity>')
        w('    <cmdty:space>CURRENCY</cmdty:space>')
        w('    <cmdty:id>USD</cmdty:id>')
        w('  </act:commodity>')
        w('  <act:commodity-scu>100</act:commodity-scu>')
        w(f'  <act:description>{account.name}</act:description>')
        if account.placeholder:
            w('  <act:slots>')
            w('    <slot>')
            w('      <slot:key>placeholder</slot:key>')
            w('      <slot:value type="string">true</slot:value>')
            w('    </slot>')
            w('  </act:slots>')
        if account.parent is not None:
            w(f'  <act:parent type="guid">{account.parent.guid}</act:parent>')
        w('</gnc:account>')

    for transaction in transactions:
        w('<gnc:transaction version="2.0.0">')
        w(f'  <trn:id type="guid">{transaction.guid}</trn:id>')
        w('  <trn:currency>')
        w('    <cmdty:space>CURRENCY</cmdty:space>')
        w('    <cmdty:id>USD</cmdty:id>')
        w('  </trn:currency>')
        w(f'  <trn:num>{transaction.number}</trn:num>')
        w('  <trn:date-posted>')
        w(f'    <ts:date>{ts(transaction.posted)}</ts:date>')
        w('  </trn:date-posted>')
        w('  <trn:date-entered>')
        w(f'    <ts:date>{ts(transaction.posted, "20:39:41")}</ts:date>')
        w('  </trn:date-entered>')
        w(f'  <trn:description>{transaction.description}</trn:description>')
        if transaction.closing:
            w('  <trn:slots>')
            w('    <slot>')
            w('      <slot:key>book_closing</slot:key>')
            w('      <slot:value type="integer">1</slot:value>')
            w('    </slot>')
            w('  </trn:slots>')
        w('  <trn:splits>')
        for account, cents, memo, action in transaction.splits:
            w('    <trn:split>')
            w(f'      <split:id type="guid">{guid()}</split:id>')
            if memo:
                w(f'      <split:memo>{memo}</split:memo>')
            if action:
                w(f'      <split:action>{action}</split:action>')
            w('      <split:reconciled-state>n</split:reconciled-state>')
            w(f'      <split:value>{money(cents)}</split:value>')
            w(f'      <split:quantity>{money(cents)}</split:quantity>')
            w(f'      <split:account type="guid">{account.guid}'
              '</split:account>')
            w('    </trn:split>')
        w('  </trn:splits>')
        w('</gnc:transaction>')

    # The budget object.  Its slots are the per-account, per-period values:
    # key = account GUID, value = frame of period index -> numeric.
    w('<gnc:budget version="2.0.0">')
    w(f'  <bgt:id type="guid">{guid()}</bgt:id>')
    w(f'  <bgt:name>{BUDGET_NAME}</bgt:name>')
    w('  <bgt:description>Generated by make-budget-book.py</bgt:description>')
    w(f'  <bgt:num-periods>{len(PERIODS)}</bgt:num-periods>')
    w('  <bgt:recurrence version="1.0.0">')
    w('    <recurrence:mult>1</recurrence:mult>')
    w('    <recurrence:period_type>month</recurrence:period_type>')
    w('    <recurrence:start>')
    w(f'      <gdate>{PERIODS[0]}</gdate>')
    w('    </recurrence:start>')
    w('  </bgt:recurrence>')
    w('  <bgt:slots>')
    for account in accounts:
        values = budget.get(account)
        if not values:
            continue
        w('    <slot>')
        w(f'      <slot:key>{account.guid}</slot:key>')
        w('      <slot:value type="frame">')
        for period in sorted(values):
            w('        <slot>')
            w(f'          <slot:key>{period}</slot:key>')
            w('          <slot:value type="numeric">'
              f'{money(values[period])}</slot:value>')
            w('        </slot>')
        w('      </slot:value>')
        w('    </slot>')
    w('  </bgt:slots>')
    w('</gnc:budget>')

    w('</gnc:book>')
    w('</gnc-v2>')

    with open(out_path, "w", encoding="utf-8") as handle:
        handle.write("\n".join(out) + "\n")
    print(f"wrote {out_path}: {len(accounts)} accounts, "
          f"{len(transactions)} transactions, 1 budget "
          f"({len(PERIODS)} periods)")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("usage: make-budget-book.py <output.gnucash>")
    build(sys.argv[1])
