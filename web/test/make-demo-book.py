#!/usr/bin/env python3
"""Write a realistic demo GnuCash book for the web UI, plus its expectations.

The tree's main fixture, ``sample1.gnucash``, has 104 accounts and two
transactions: almost every account shows a zero balance, so an account that is
broken and an account with no activity look the same.  This script builds a
book whose accounts all carry real, checkable balances:

  * a household chart of accounts -- Assets (checking, savings, cash,
    brokerage), Liabilities (credit card, auto loan, student loan), Equity
    (opening balances), Income (salary with a bonus sub-account, interest,
    other income with gifts and reimbursements) and Expenses (housing with
    rent, utilities and renters insurance; food with groceries and dining;
    transport with fuel, transit and auto insurance; health; personal;
    entertainment; travel; technology; taxes; gifts; fees; loan interest).
    That is 54 accounts, five of them top-level placeholders.  ``Assets:
    Brokerage`` and ``Liabilities:Student Loan`` are deliberately empty so
    the UI can show both the active and the inactive state;

  * twelve complete calendar months of balanced transactions ending with the
    last complete month before the book is generated (override the window
    with ``GNUCASH_DEMO_END_MONTH=YYYY-MM``).  Every month has salary, rent,
    utilities, groceries, dining, transport, insurance, a savings transfer, a
    credit-card payment, a loan payment and interest; the year also has a
    holiday, a laptop, a bonus, gifts and a reimbursement, so there are larger
    one-offs to find in the register and in the reports.  The opening balances
    are booked against ``Equity:Opening Balances``;

  * one transaction on the first day of the window and all further dates
    inside it, so the report range recorded in the expectations contains every
    split and the reports have real rows whichever "current year" default the
    engine applies.

``<book>.expect.json`` is written next to the book.  It records the figures
this script's own arithmetic produced -- the number of accounts and
transactions, the debit-positive (raw) total of every top-level account, the
totals and split counts of the named accounts the checker also reads from a
register page, the split count of every account, and the row count the Trial
Balance must have.  A test can therefore compare the engine's numbers with the
generator's arithmetic, and a wrong generator fails too.  Amounts in the JSON
are integer cents, debit-positive: assets and expenses positive, liabilities,
equity and income negative.  GnuCash may display credit-normal accounts with
the opposite sign depending on the "reversed accounts" preference, so the
checker accepts either whole-book convention but not a different total.

The book is encoded the way the engine stores it: a ``gnc:book`` with one
CURRENCY commodity (USD, fraction 100), ``gnc:account`` objects that name their
parent by GUID (the root account is created by the loader and is not written),
and ``gnc:transaction`` objects whose ``trn:split`` values are
``<value>/<fraction>``.  Every transaction's split values sum to zero; the
generator asserts that before writing, and also asserts that every expense
leaf account has at least one split and that the whole tree sums to zero.

Usage:
    make-demo-book.py <output.gnucash>

The book is deliberately plain: one currency, no prices, no lots, no lots of
business data.  Run it with python3; it needs no GnuCash libraries.
"""
import calendar
import datetime
import json
import os
import sys
import uuid
from xml.sax.saxutils import escape

USD_SCALE = 100
# Two accounts are meant to stay at zero: an inactive brokerage and a paid-off
# student loan.  They are named here so the expectations and the checker agree
# on which zeros are deliberate.
EMPTY_ACCOUNTS = ("Assets:Brokerage", "Liabilities:Student Loan")

# The chart of accounts.  Parents come before their children; a placeholder is
# a heading the reports roll up rather than an account that holds splits.
ACCOUNTS = [
    ("Assets", "ASSET", True),
    ("Assets:Checking", "BANK", False),
    ("Assets:Savings", "BANK", False),
    ("Assets:Cash", "CASH", False),
    ("Assets:Brokerage", "ASSET", False),

    ("Liabilities", "LIABILITY", True),
    ("Liabilities:Credit Card", "CREDIT", False),
    ("Liabilities:Auto Loan", "LIABILITY", False),
    ("Liabilities:Student Loan", "LIABILITY", False),

    ("Equity", "EQUITY", True),
    ("Equity:Opening Balances", "EQUITY", False),

    ("Income", "INCOME", True),
    ("Income:Salary", "INCOME", True),
    ("Income:Salary:Main Job", "INCOME", False),
    ("Income:Salary:Bonus", "INCOME", False),
    ("Income:Interest", "INCOME", False),
    ("Income:Other Income", "INCOME", True),
    ("Income:Other Income:Gifts Received", "INCOME", False),
    ("Income:Other Income:Reimbursements", "INCOME", False),

    ("Expenses", "EXPENSE", True),
    ("Expenses:Housing", "EXPENSE", True),
    ("Expenses:Housing:Rent", "EXPENSE", False),
    ("Expenses:Housing:Utilities", "EXPENSE", True),
    ("Expenses:Housing:Utilities:Electricity", "EXPENSE", False),
    ("Expenses:Housing:Utilities:Internet", "EXPENSE", False),
    ("Expenses:Housing:Utilities:Water", "EXPENSE", False),
    ("Expenses:Housing:Renters Insurance", "EXPENSE", False),
    ("Expenses:Food", "EXPENSE", True),
    ("Expenses:Food:Groceries", "EXPENSE", False),
    ("Expenses:Food:Dining Out", "EXPENSE", False),
    ("Expenses:Transportation", "EXPENSE", True),
    ("Expenses:Transportation:Fuel", "EXPENSE", False),
    ("Expenses:Transportation:Public Transit", "EXPENSE", False),
    ("Expenses:Transportation:Auto Insurance", "EXPENSE", False),
    ("Expenses:Health", "EXPENSE", True),
    ("Expenses:Health:Health Insurance", "EXPENSE", False),
    ("Expenses:Health:Pharmacy", "EXPENSE", False),
    ("Expenses:Personal", "EXPENSE", True),
    ("Expenses:Personal:Clothing", "EXPENSE", False),
    ("Expenses:Personal:Gym", "EXPENSE", False),
    ("Expenses:Entertainment", "EXPENSE", True),
    ("Expenses:Entertainment:Streaming", "EXPENSE", False),
    ("Expenses:Entertainment:Hobbies", "EXPENSE", False),
    ("Expenses:Travel", "EXPENSE", True),
    ("Expenses:Travel:Holiday", "EXPENSE", False),
    ("Expenses:Technology", "EXPENSE", True),
    ("Expenses:Technology:Computer", "EXPENSE", False),
    ("Expenses:Taxes", "EXPENSE", True),
    ("Expenses:Taxes:Income Tax", "EXPENSE", False),
    ("Expenses:Gifts", "EXPENSE", False),
    ("Expenses:Fees", "EXPENSE", True),
    ("Expenses:Fees:Bank Fees", "EXPENSE", False),
    ("Expenses:Interest", "EXPENSE", True),
    ("Expenses:Interest:Loan Interest", "EXPENSE", False),
]

NAMED_ACCOUNTS = (
    "Assets:Checking",
    "Assets:Savings",
    "Assets:Cash",
    "Liabilities:Credit Card",
    "Income:Salary:Main Job",
    "Expenses:Food:Groceries",
    "Expenses:Housing:Rent",
)
REGISTER_ACCOUNT = "Assets:Savings"


def guid():
    return uuid.uuid4().hex


def ts(day, time="10:59:00"):
    return f"{day} {time} +0000"


def money(cents):
    """A gnc_numeric as the XML writes it: value over the currency fraction."""
    return f"{cents}/{USD_SCALE}"


class Account:
    def __init__(self, path, type_, placeholder=False):
        self.path = path
        self.name = path.rsplit(":", 1)[-1]
        self.type = type_
        self.placeholder = placeholder
        self.parent = path.rsplit(":", 1)[0] if ":" in path else None
        self.guid = guid()
        self.balance = 0
        self.splits = 0


class Transaction:
    def __init__(self, number, posted, description, splits):
        self.guid = guid()
        self.number = number
        self.posted = posted
        self.description = description
        # list of (account, cents, memo, action)
        self.splits = splits


def last_day(year, month):
    return calendar.monthrange(year, month)[1]


def iso(year, month, day):
    return f"{year:04d}-{month:02d}-{min(day, last_day(year, month)):02d}"


def month_window():
    """The twelve complete months ending last month.

    A book generated today must not contain future-dated splits: the account
    balances and the default reports stop at today, so December's salary would
    silently disappear from the tree.  ``GNUCASH_DEMO_END_MONTH=YYYY-MM`` pins
    the window for a reproducible run.
    """
    override = os.environ.get("GNUCASH_DEMO_END_MONTH", "").strip()
    if override:
        try:
            year, month = (int(part) for part in override.split("-", 1))
        except ValueError:
            raise SystemExit(
                "GNUCASH_DEMO_END_MONTH must be YYYY-MM, got %r" % override)
    else:
        end = datetime.date.today().replace(day=1) - datetime.timedelta(days=1)
        year, month = end.year, end.month
    months = []
    for back in range(11, -1, -1):
        y, m = year, month - back
        while m < 1:
            m += 12
            y -= 1
        months.append((y, m))
    return months


def build(out_path):
    accounts = [Account(*spec) for spec in ACCOUNTS]
    by_path = {account.path: account for account in accounts}
    if len(by_path) != len(accounts):
        raise SystemExit("duplicate account path in ACCOUNTS")

    months = month_window()
    first_year, first_month = months[0]
    last_year, last_month = months[-1]
    start = iso(first_year, first_month, 1)
    end = iso(last_year, last_month, last_day(last_year, last_month))

    transactions = []

    def add(date, description, splits):
        total = sum(cents for _, cents, _, _ in splits)
        if total != 0:
            raise SystemExit(
                f"unbalanced transaction {description!r} on {date}: {total}")
        number = "D-%04d" % (len(transactions) + 1)
        transactions.append(Transaction(number, date, description, splits))

    # ------------------------------------------------------- opening balance
    # Assets and debts as they stood the day the book starts.  The equity
    # split is whatever makes the transaction balance.
    opening = [
        (by_path["Assets:Checking"], 450000, "Opening balance", ""),
        (by_path["Assets:Savings"], 1200000, "Opening balance", ""),
        (by_path["Assets:Cash"], 50000, "Opening balance", ""),
        (by_path["Liabilities:Credit Card"], -85000, "Opening balance", ""),
        (by_path["Liabilities:Auto Loan"], -1400000, "Opening balance", ""),
    ]
    equity = -sum(cents for _, cents, _, _ in opening)
    opening.append((by_path["Equity:Opening Balances"], equity, "", ""))
    add(start, "Opening balances", opening)

    # ---------------------------------------------------------- the year
    for index, (year, month) in enumerate(months):
        def on(day):
            return iso(year, month, day)

        # Income.
        add(on(25), "Monthly salary",
            [(by_path["Assets:Checking"], 420000, "Net pay", "Deposit"),
             (by_path["Expenses:Taxes:Income Tax"], 90000, "Withholding", ""),
             (by_path["Income:Salary:Main Job"], -510000, "", "")])

        # Housing.
        add(on(1), "Rent",
            [(by_path["Expenses:Housing:Rent"], 145000, "Monthly rent", "Buy"),
             (by_path["Assets:Checking"], -145000, "", "")])
        electricity = 9200 + (index * 137) % 4900
        add(on(12), "Electricity bill",
            [(by_path["Expenses:Housing:Utilities:Electricity"], electricity,
              "Monthly bill", "Buy"),
             (by_path["Assets:Checking"], -electricity, "Auto-pay", "")])
        add(on(15), "Internet",
            [(by_path["Expenses:Housing:Utilities:Internet"], 6500, "", "Buy"),
             (by_path["Assets:Checking"], -6500, "Auto-pay", "")])
        if index % 3 == 0:
            water = 3900 + (index * 77) % 900
            add(on(18), "Water bill",
                [(by_path["Expenses:Housing:Utilities:Water"], water,
                  "Quarterly bill", "Buy"),
                 (by_path["Assets:Checking"], -water, "Auto-pay", "")])
        add(on(20), "Renters insurance",
            [(by_path["Expenses:Housing:Renters Insurance"], 1850, "", "Buy"),
             (by_path["Assets:Checking"], -1850, "Auto-pay", "")])

        # Food: two supermarket trips a month, plus dining on the card and a
        # little cash spent at the farmers' market each quarter.
        for day, base, spread, memo in (
                (5, 19500, 6200, "First shop"),
                (19, 9200, 3400, "Top-up shop")):
            amount = base + (index * 613 + day * 29) % spread
            add(on(day), "Grocery store",
                [(by_path["Expenses:Food:Groceries"], amount, memo, "Buy"),
                 (by_path["Assets:Checking"], -amount, "", "")])
        dining = 6200 + (index * 421) % 2800
        add(on(8), "Restaurant",
            [(by_path["Expenses:Food:Dining Out"], dining, "Dinner", "Buy"),
             (by_path["Liabilities:Credit Card"], -dining, "", "")])
        if index % 3 == 0:
            add(on(21), "Farmers' market",
                [(by_path["Expenses:Food:Groceries"], 3500, "Cash", "Buy"),
                 (by_path["Assets:Cash"], -3500, "", "")])

        # Transportation: fuel and subscriptions on the card, the rest by
        # bank transfer.
        fuel = 4400 + (index * 233) % 1500
        add(on(10), "Fuel",
            [(by_path["Expenses:Transportation:Fuel"], fuel, "", "Buy"),
             (by_path["Liabilities:Credit Card"], -fuel, "", "")])
        add(on(7), "Transit pass",
            [(by_path["Expenses:Transportation:Public Transit"], 7500, "",
              "Buy"),
             (by_path["Assets:Checking"], -7500, "", "")])
        add(on(3), "Auto insurance",
            [(by_path["Expenses:Transportation:Auto Insurance"], 11200, "",
              "Buy"),
             (by_path["Assets:Checking"], -11200, "Auto-pay", "")])

        # Health and personal.
        add(on(5), "Health insurance",
            [(by_path["Expenses:Health:Health Insurance"], 21800, "", "Buy"),
             (by_path["Assets:Checking"], -21800, "Auto-pay", "")])
        if index in (1, 4, 7, 10):
            pharmacy = 1800 + (index * 311) % 1400
            add(on(16), "Pharmacy",
                [(by_path["Expenses:Health:Pharmacy"], pharmacy, "", "Buy"),
                 (by_path["Liabilities:Credit Card"], -pharmacy, "", "")])
        add(on(2), "Gym membership",
            [(by_path["Expenses:Personal:Gym"], 4500, "", "Buy"),
             (by_path["Assets:Checking"], -4500, "Auto-pay", "")])
        if index in (2, 6, 9):
            clothes = 6500 + (index * 509) % 5500
            add(on(11), "Clothing",
                [(by_path["Expenses:Personal:Clothing"], clothes, "", "Buy"),
                 (by_path["Liabilities:Credit Card"], -clothes, "", "")])

        # Entertainment.
        add(on(14), "Streaming subscription",
            [(by_path["Expenses:Entertainment:Streaming"], 1599, "", "Buy"),
             (by_path["Liabilities:Credit Card"], -1599, "Auto-pay", "")])
        if index in (1, 5, 8, 11):
            hobby = 2500 + (index * 173) % 3000
            add(on(22), "Hobby supplies",
                [(by_path["Expenses:Entertainment:Hobbies"], hobby, "", "Buy"),
                 (by_path["Liabilities:Credit Card"], -hobby, "", "")])

        # Taxes, fees, gifts.
        if index in (4, 11):
            gift = 6000 if index == 4 else 12000
            add(on(9), "Gift",
                [(by_path["Expenses:Gifts"], gift, "Birthday present", "Buy"),
                 (by_path["Assets:Checking"], -gift, "", "")])
        add(on(28), "Bank fee",
            [(by_path["Expenses:Fees:Bank Fees"], 1200, "Account fee", ""),
             (by_path["Assets:Checking"], -1200, "", "")])

        # Debt service: a level loan payment split into principal (which
        # reduces the liability) and interest (an expense), plus the card
        # payment, which pays down the card without being an expense.
        add(on(28), "Auto loan payment",
            [(by_path["Assets:Checking"], -36500, "", ""),
             (by_path["Liabilities:Auto Loan"], 32000, "Principal", ""),
             (by_path["Expenses:Interest:Loan Interest"], 4500, "Interest",
              "")])
        add(on(24), "Credit card payment",
            [(by_path["Liabilities:Credit Card"], 40000, "Payment", ""),
             (by_path["Assets:Checking"], -40000, "", "")])

        # Savings: a transfer and the month's interest.
        add(on(26), "Transfer to savings",
            [(by_path["Assets:Savings"], 50000, "Monthly transfer", ""),
             (by_path["Assets:Checking"], -50000, "", "")])
        interest = 2800 + index * 25
        add(iso(year, month, last_day(year, month)), "Savings interest",
            [(by_path["Assets:Savings"], interest, "", ""),
             (by_path["Income:Interest"], -interest, "", "")])

        # The year's one-offs.
        if index == 3:
            add(on(17), "New laptop",
                [(by_path["Expenses:Technology:Computer"], 185000,
                  "Home office", "Buy"),
                 (by_path["Liabilities:Credit Card"], -185000, "", "")])
        if index == 5:
            add(on(15), "Annual bonus",
                [(by_path["Assets:Checking"], 250000, "Bonus", "Deposit"),
                 (by_path["Income:Salary:Bonus"], -250000, "", "")])
        if index == 6:
            add(on(20), "Summer holiday",
                [(by_path["Expenses:Travel:Holiday"], 280000,
                  "Flights and hotel", "Buy"),
                 (by_path["Liabilities:Credit Card"], -280000, "", "")])
        if index == 7:
            add(on(13), "Expense reimbursement",
                [(by_path["Assets:Checking"], 34000, "Travel claim", "Deposit"),
                 (by_path["Income:Other Income:Reimbursements"], -34000, "",
                  "")])
        if index == 9:
            add(on(18), "Gift received",
                [(by_path["Assets:Checking"], 25000, "From family", "Deposit"),
                 (by_path["Income:Other Income:Gifts Received"], -25000, "",
                  "")])

    # ------------------------------------------------------- expectations
    balances = {account.path: 0 for account in accounts}
    split_counts = {account.path: 0 for account in accounts}
    for transaction in transactions:
        for account, cents, _, _ in transaction.splits:
            balances[account.path] += cents
            split_counts[account.path] += 1
            account.balance += cents
            account.splits += 1

    top_level = [account for account in accounts if account.parent is None]
    totals = {}
    for top in top_level:
        totals[top.path] = sum(
            balances[account.path] for account in accounts
            if account.path == top.path
            or account.path.startswith(top.path + ":"))

    # Every figure below is this script's own arithmetic; the book is only
    # written after the invariants hold.
    if sum(totals.values()) != 0:
        raise SystemExit("top-level totals do not sum to zero: %r" % totals)
    for account in accounts:
        if account.path in EMPTY_ACCOUNTS:
            if account.balance != 0 or account.splits != 0:
                raise SystemExit(f"{account.path} was meant to be empty")
        elif account.type == "EXPENSE" and not account.placeholder:
            if account.splits == 0:
                raise SystemExit(f"expense account {account.path} has no splits")

    expect = {
        "generator": "web/test/make-demo-book.py",
        "book": os.path.basename(out_path),
        "period": {"start": start, "end": end},
        "accounts": len(accounts),
        "top_level_count": len(top_level),
        "transactions": len(transactions),
        "splits": sum(split_counts.values()),
        "top_level_totals": totals,
        "named_accounts": {
            path: {"total": balances[path], "splits": split_counts[path]}
            for path in NAMED_ACCOUNTS
        },
        "empty_accounts": list(EMPTY_ACCOUNTS),
        "register_account": REGISTER_ACCOUNT,
        "split_counts": split_counts,
        "reports": {
            # Every account type in this book is a Trial Balance type, so the
            # report must print one row per account plus its total row.
            "Trial Balance": {"data_rows": len(accounts) + 1},
            # The P&L and the balance sheet collapse below two levels of
            # subaccounts, so their rows are fewer than the account count; the
            # thresholds catch a report that loses whole sections.
            "Profit & Loss": {"min_data_rows": 30},
            "Balance Sheet": {"min_data_rows": 12},
        },
        "note": ("Cents, debit-positive.  Credit-normal accounts may be shown "
                 "negated under the 'reversed accounts' preference; the "
                 "checker accepts either whole-book convention."),
    }

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

    w('<gnc:commodity version="2.0.0">')
    w('  <cmdty:space>CURRENCY</cmdty:space>')
    w('  <cmdty:id>USD</cmdty:id>')
    w('  <cmdty:name>US Dollar</cmdty:name>')
    w('  <cmdty:fraction>100</cmdty:fraction>')
    w('</gnc:commodity>')

    for account in accounts:
        w('<gnc:account version="2.0.0">')
        w(f'  <act:name>{escape(account.name)}</act:name>')
        w(f'  <act:id type="guid">{account.guid}</act:id>')
        w(f'  <act:type>{account.type}</act:type>')
        w('  <act:commodity>')
        w('    <cmdty:space>CURRENCY</cmdty:space>')
        w('    <cmdty:id>USD</cmdty:id>')
        w('  </act:commodity>')
        w('  <act:commodity-scu>100</act:commodity-scu>')
        w(f'  <act:description>{escape(account.path)}</act:description>')
        if account.placeholder:
            w('  <act:slots>')
            w('    <slot>')
            w('      <slot:key>placeholder</slot:key>')
            w('      <slot:value type="string">true</slot:value>')
            w('    </slot>')
            w('  </act:slots>')
        if account.parent is not None:
            parent = by_path[account.parent]
            w(f'  <act:parent type="guid">{parent.guid}</act:parent>')
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
        w(f'  <trn:description>{escape(transaction.description)}'
          '</trn:description>')
        w('  <trn:splits>')
        for account, cents, memo, action in transaction.splits:
            w('    <trn:split>')
            w(f'      <split:id type="guid">{guid()}</split:id>')
            if memo:
                w(f'      <split:memo>{escape(memo)}</split:memo>')
            if action:
                w(f'      <split:action>{escape(action)}</split:action>')
            w('      <split:reconciled-state>n</split:reconciled-state>')
            w(f'      <split:value>{money(cents)}</split:value>')
            w(f'      <split:quantity>{money(cents)}</split:quantity>')
            w(f'      <split:account type="guid">{account.guid}'
              '</split:account>')
            w('    </trn:split>')
        w('  </trn:splits>')
        w('</gnc:transaction>')

    w('</gnc:book>')
    w('</gnc-v2>')

    with open(out_path, "w", encoding="utf-8") as handle:
        handle.write("\n".join(out) + "\n")

    expect_path = out_path + ".expect.json"
    with open(expect_path, "w", encoding="utf-8") as handle:
        json.dump(expect, handle, indent=2)
        handle.write("\n")

    print(f"wrote {out_path}: {len(accounts)} accounts, "
          f"{len(transactions)} transactions, {sum(split_counts.values())} "
          f"splits, {start}..{end}")
    print(f"wrote {expect_path}")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("usage: make-demo-book.py <output.gnucash>")
    build(sys.argv[1])
