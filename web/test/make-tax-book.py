#!/usr/bin/env python3
"""Write a GnuCash XML book for the US Tax Schedule report and TXF export.

No book in the tree has tax-related accounts with US TXF codes, so there is
nothing for the schedule to group: sample1 has none and make-invoice-book.py
only has a sales-tax table.  This script builds a book outside the repository
with an individual (F1040) tax setup and activity in 2026:

  * an Income subtree with two Schedule B interest accounts (the same TXF code
    N287, so the report's per-payer sub-totals are exercised), a W-2 salary
    account (N460), a dividend account (N286, the "ReinvD" TXF action) and
    Schedule C gross receipts (N293);
  * an Expense subtree with Schedule C advertising (N304), supplies (N301)
    and taxes/licences (N316), Schedule 1 IRA contribution (N262), and two
    federal estimated-tax payments (N521, the per-split TXF format 6 records);
  * a zero-activity state estimated-tax account (N522), a tax-report-only
    account (N000, which the report prints but the TXF file must omit), a
    tax-related account with no code at all, and an asset account carrying a
    code that is only valid for a different tax entity type -- the last two
    are the Scheme report's error cases and must not appear in the table;
  * two federal estimated-tax payments, and an advertising refund credit, so
    both signs of an expense are covered.

Every amount is exact to the cent and every transaction balances.  The tax
type ("F1040") and name live in the book's top-level tax_US option frame, the
same keys gnc_get_current_book_tax_type()/name() read.

Usage:
    make-tax-book.py <output.gnucash>

The book's period is 2026-01-01..2026-12-31 for the report's From/To options.
"""
import sys
import uuid

TAX_YEAR = 2026


def guid():
    return uuid.uuid4().hex


def ts(day, time="10:59:00"):
    return f"{day} {time} +0000"


def money(cents):
    return f"{cents}/100"


class Account:
    def __init__(self, name, type_, parent=None, placeholder=False,
                 tax_related=False, code=None, payer_source=None):
        self.guid = guid()
        self.name = name
        self.type = type_
        self.parent = parent
        self.placeholder = placeholder
        self.tax_related = tax_related
        self.code = code
        self.payer_source = payer_source


def main(argv):
    if len(argv) < 2:
        raise SystemExit(__doc__)
    out_path = argv[1]

    root = Account("Root Account", "ROOT")
    assets = Account("Assets", "ASSET", root, placeholder=True)
    liabilities = Account("Liabilities", "LIABILITY", root, placeholder=True)
    income = Account("Income", "INCOME", root, placeholder=True)
    expenses = Account("Expenses", "EXPENSE", root, placeholder=True)

    checking = Account("Checking Account", "BANK", assets)
    brokerage = Account("Brokerage Cash", "ASSET", assets, tax_related=True,
                        code="N1864")  # valid for F1065 only
    interest = Account("Interest Income", "INCOME", income, tax_related=True,
                       code="N287")
    credit_union = Account("Credit Union Interest", "INCOME", income,
                           tax_related=True, code="N287")
    salary = Account("Salary", "INCOME", income, tax_related=True,
                     code="N460", payer_source="parent")
    dividends = Account("Dividend Income", "INCOME", income, tax_related=True,
                        code="N286")
    consulting = Account("Consulting Income", "INCOME", income,
                         tax_related=True, code="N293")
    advertising = Account("Advertising", "EXPENSE", expenses,
                          tax_related=True, code="N304")
    supplies = Account("Office Supplies", "EXPENSE", expenses,
                       tax_related=True, code="N301")
    taxes = Account("Taxes and Licenses", "EXPENSE", expenses,
                    tax_related=True, code="N316")
    federal = Account("Federal Estimated Tax", "EXPENSE", expenses,
                      tax_related=True, code="N521")
    state = Account("State Estimated Tax", "EXPENSE", expenses,
                    tax_related=True, code="N522")
    ira = Account("IRA Contribution", "EXPENSE", expenses, tax_related=True,
                  code="N262")
    n000 = Account("Non-TXF Adjustment", "EXPENSE", expenses,
                   tax_related=True, code="N000")
    uncoded = Account("Unclassified Expense", "EXPENSE", expenses,
                      tax_related=True)
    accounts = [root, assets, liabilities, income, expenses, checking,
                brokerage, interest, credit_union, salary, dividends,
                consulting, advertising, supplies, taxes, federal, state,
                ira, n000, uncoded]

    # (number, date, description, [(amount, account, memo, action)])
    transactions = [
        ("T-001", "2026-01-31", "First Bank interest", [
            (12500, checking, "January statement", "Deposit"),
            (-12500, interest, "January statement", "Deposit")]),
        ("T-002", "2026-02-28", "Credit union interest", [
            (7500, checking, "February statement", "Deposit"),
            (-7500, credit_union, "February statement", "Deposit")]),
        ("T-003", "2026-03-01", "March payroll", [
            (100000, checking, "March salary", "Deposit"),
            (-100000, salary, "March salary", "Deposit")]),
        ("T-004", "2026-03-15", "Dividend", [
            (5000, checking, "Quarterly dividend", "Deposit"),
            (-5000, dividends, "Quarterly dividend", "Deposit")]),
        ("T-005", "2026-04-01", "Consulting work", [
            (40000, checking, "Client invoice", "Deposit"),
            (-40000, consulting, "Client invoice", "Deposit")]),
        ("T-006", "2026-04-10", "Advertising", [
            (4000, advertising, "Trade magazine", "Expense"),
            (-4000, checking, "Trade magazine", "Expense")]),
        ("T-007", "2026-04-12", "Advertising refund", [
            (-500, advertising, "Refund", "Expense"),
            (500, checking, "Refund", "Expense")]),
        ("T-008", "2026-04-15", "Q1 federal estimate", [
            (10000, federal, "Q1 estimate", "Expense"),
            (-10000, checking, "Q1 estimate", "Expense")]),
        ("T-009", "2026-05-01", "Office supplies", [
            (2500, supplies, "Paper and toner", "Expense"),
            (-2500, checking, "Paper and toner", "Expense")]),
        ("T-010", "2026-05-15", "Licence fee", [
            (1500, taxes, "Business licence", "Expense"),
            (-1500, checking, "Business licence", "Expense")]),
        ("T-011", "2026-06-15", "Q2 federal estimate", [
            (15000, federal, "Q2 estimate", "Expense"),
            (-15000, checking, "Q2 estimate", "Expense")]),
        ("T-012", "2026-07-01", "IRA contribution", [
            (20000, ira, "2026 contribution", "Expense"),
            (-20000, checking, "2026 contribution", "Expense")]),
        ("T-013", "2026-08-01", "Tax report only adjustment", [
            (1000, n000, "Report only", "Expense"),
            (-1000, checking, "Report only", "Expense")]),
        ("T-014", "2026-08-15", "Brokerage cash", [
            (2000, brokerage, "Cash sweep", "Expense"),
            (-2000, checking, "Cash sweep", "Expense")]),
        ("T-015", "2026-09-01", "Unclassified expense", [
            (500, uncoded, "No tax code", "Expense"),
            (-500, checking, "No tax code", "Expense")]),
    ]

    out = []
    w = out.append
    w('<?xml version="1.0" encoding="utf-8" ?>')
    w('<gnc-v2')
    for prefix, uri in (
            ("gnc", "gnc"), ("act", "act"), ("book", "book"), ("cd", "cd"),
            ("cmdty", "cmdty"), ("price", "price"), ("slot", "slot"),
            ("split", "split"), ("sx", "sx"), ("trn", "trn"), ("ts", "ts"),
            ("fs", "fs"), ("bgt", "bgt"), ("recurrence", "recurrence"),
            ("lot", "lot"), ("addr", "addr"), ("billterm", "billterm"),
            ("bt-days", "bt-days"), ("bt-prox", "bt-prox"), ("cust", "cust"),
            ("employee", "employee"), ("entry", "entry"), ("invoice", "invoice"),
            ("job", "job"), ("order", "order"), ("owner", "owner"),
            ("taxtable", "taxtable"), ("tte", "tte"), ("vendor", "vendor")):
        w(f'     xmlns:{prefix}="http://www.gnucash.org/XML/{uri}"' +
          (">" if prefix == "vendor" else ""))
    w('<gnc:count-data cd:type="book">1</gnc:count-data>')
    w('<gnc:book version="2.0.0">')
    w(f'<book:id type="guid">{guid()}</book:id>')
    # gnc_get_current_book_tax_type()/name() read "tax_US/type" and
    # "tax_US/name" as top-level book slots (qof_book_get_string_option).
    w('<book:slots>')
    w('  <slot>')
    w('    <slot:key>tax_US</slot:key>')
    w('    <slot:value type="frame">')
    w('      <slot><slot:key>type</slot:key>'
      '<slot:value type="string">F1040</slot:value></slot>')
    w('      <slot><slot:key>name</slot:key>'
      '<slot:value type="string">Doe Household</slot:value></slot>')
    w('    </slot:value>')
    w('  </slot>')
    w('</book:slots>')
    w('<gnc:count-data cd:type="commodity">1</gnc:count-data>')
    w(f'<gnc:count-data cd:type="account">{len(accounts)}</gnc:count-data>')
    w(f'<gnc:count-data cd:type="transaction">{len(transactions)}'
      '</gnc:count-data>')
    w('<gnc:commodity version="2.0.0"><cmdty:space>CURRENCY</cmdty:space>'
      '<cmdty:id>USD</cmdty:id><cmdty:name>US Dollar</cmdty:name>'
      '<cmdty:fraction>100</cmdty:fraction></gnc:commodity>')

    for account in accounts:
        w('<gnc:account version="2.0.0">')
        w(f'  <act:name>{account.name}</act:name>')
        w(f'  <act:id type="guid">{account.guid}</act:id>')
        w(f'  <act:type>{account.type}</act:type>')
        w('  <act:commodity><cmdty:space>CURRENCY</cmdty:space>'
          '<cmdty:id>USD</cmdty:id></act:commodity>')
        w('  <act:commodity-scu>100</act:commodity-scu>')
        w(f'  <act:description>{account.name}</act:description>')
        slots = []
        if account.placeholder:
            slots.append(('placeholder', '<slot:value type="string">true'
                                         '</slot:value>'))
        if account.tax_related:
            slots.append(('tax-related', '<slot:value type="string">true'
                                         '</slot:value>'))
        if account.code is not None:
            frame = ['<slot:value type="frame">',
                     '<slot><slot:key>code</slot:key>'
                     f'<slot:value type="string">{account.code}</slot:value>'
                     '</slot>']
            if account.payer_source is not None:
                frame.append('<slot><slot:key>payer-name-source</slot:key>'
                             '<slot:value type="string">'
                             f'{account.payer_source}</slot:value></slot>')
            frame.append('</slot:value>')
            slots.append(('tax-US', "".join(frame)))
        if slots:
            w('  <act:slots>')
            for key, value in slots:
                w(f'    <slot><slot:key>{key}</slot:key>{value}</slot>')
            w('  </act:slots>')
        if account.parent is not None:
            w(f'  <act:parent type="guid">{account.parent.guid}</act:parent>')
        w('</gnc:account>')

    for number, posted, description, splits in transactions:
        w('<gnc:transaction version="2.0.0">')
        w(f'  <trn:id type="guid">{guid()}</trn:id>')
        w('  <trn:currency><cmdty:space>CURRENCY</cmdty:space>'
          '<cmdty:id>USD</cmdty:id></trn:currency>')
        w(f'  <trn:num>{number}</trn:num>')
        w(f'  <trn:date-posted><ts:date>{ts(posted)}</ts:date>'
          '</trn:date-posted>')
        w(f'  <trn:date-entered><ts:date>{ts(posted, "20:39:41")}</ts:date>'
          '</trn:date-entered>')
        w(f'  <trn:description>{description}</trn:description>')
        w('  <trn:splits>')
        for amount, account, memo, action in splits:
            w('    <trn:split>')
            w(f'      <split:id type="guid">{guid()}</split:id>')
            w(f'      <split:memo>{memo}</split:memo>')
            w(f'      <split:action>{action}</split:action>')
            w('      <split:reconciled-state>n</split:reconciled-state>')
            w(f'      <split:value>{money(amount)}</split:value>')
            w(f'      <split:quantity>{money(amount)}</split:quantity>')
            w(f'      <split:account type="guid">{account.guid}'
              '</split:account>')
            w('    </trn:split>')
        w('  </trn:splits>')
        w('</gnc:transaction>')

    w('</gnc:book>')
    w('</gnc-v2>')

    with open(out_path, "w", encoding="utf-8") as handle:
        handle.write("\n".join(out) + "\n")
    print(f"wrote {out_path}: tax type F1040, {len(accounts)} accounts, "
          f"{len(transactions)} transactions")
    print("taxed accounts: " + ", ".join(
        f"{a.name} ({a.code})" for a in accounts if a.code is not None))
    print("error cases: no-code 'Unclassified Expense', F1065-only code "
          "'Brokerage Cash' (N1864)")


if __name__ == "__main__":
    main(sys.argv)
