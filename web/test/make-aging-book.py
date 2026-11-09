#!/usr/bin/env python3
"""Write a GnuCash XML book with invoices, bills and payments, for the aging
report tests.

The fixture books in the source tree carry at most one invoice (sample1 has a
single vendor bill), so they cannot exercise the aging buckets.  This script
builds a book outside the repository with:

  * one A/Receivable and one A/Payable account (a second A/Receivable account
    with --two-receivable, to exercise the multi-account layout);
  * two customers (Alpha Corp, Bravo Ltd) and two vendors (Delta Supply, Echo
    Parts);
  * eight customer invoices, one customer credit note (a negative document
    whose lot balance must reduce its bucket) and three vendor bills, with due
    dates spread over every bucket relative to the report date 2026-06-30 --
    including one due exactly 90 days before it (the 91+ boundary) and one due
    exactly on it (which is 0-30 days, because the buckets compare against the
    end of the report day);
  * a partial payment, a full payment, and one unapplied payment
    ("pre-payment") per side.

Usage:
    make-aging-book.py <output.gnucash> [--two-receivable]

The book is deliberately plain: one currency (USD), no taxes and no bill
terms.  The due date lives where gncInvoiceGetDateDue() reads it, in the
posted transaction's trans-date-due slot.

Run it with python3; it needs no GnuCash libraries.
"""
import sys
import uuid

REPORT_DATE = "2026-06-30"       # the date the buckets are measured from


def guid():
    return uuid.uuid4().hex


def ts(day, time="10:59:00"):
    return f"{day} {time} +0000"


def money(cents):
    """A gnc_numeric as the XML writes it: value over 100."""
    return f"{cents}/100"


class Account:
    def __init__(self, name, type_, parent=None, placeholder=False):
        self.guid = guid()
        self.name = name
        self.type = type_
        self.parent = parent
        self.placeholder = placeholder
        self.lots = []


class Owner:
    def __init__(self, kind, name, owner_id, addr1, active=True):
        self.guid = guid()
        self.kind = kind            # "customer" | "vendor"
        self.name = name
        self.owner_id = owner_id
        self.addr1 = addr1
        self.active = active


class Lot:
    def __init__(self, account, invoice=None, owner=None, title=""):
        self.guid = guid()
        self.account = account
        self.invoice = invoice      # Invoice object for invoice lots
        self.owner = owner          # Owner object for payment/prepayment lots
        self.title = title
        account.lots.append(self)


class Invoice:
    def __init__(self, owner, number, posted, due, cents, account,
                 counter_account, action, credit_note=False):
        self.guid = guid()
        self.owner = owner
        self.number = number
        self.posted = posted
        self.due = due
        self.cents = cents
        self.account = account          # A/Receivable or A/Payable
        self.counter_account = counter_account
        self.action = action
        self.credit_note = credit_note
        self.txn_guid = guid()
        self.split_guid = guid()
        self.counter_split_guid = guid()
        self.entry_guid = guid()
        self.lot = Lot(account, invoice=self, title=f"{action} {number}")


class Payment:
    """A payment linked to an invoice puts its A/P or A/R split into the
    *invoice* lot, which is how the report finds the remaining balance; an
    unapplied payment gets a lot of its own carrying the owner, which is the
    case the aging report calls a "Pre-Payment"."""

    def __init__(self, owner, number, posted, cents, account, counter_account,
                 invoice=None, lot=None, memo=""):
        self.owner = owner
        self.number = number
        self.posted = posted
        self.cents = cents              # positive amount moved
        self.account = account
        self.counter_account = counter_account
        self.invoice = invoice
        self.lot = lot
        self.memo = memo
        self.txn_guid = guid()
        self.owner_split_guid = guid()   # the A/P or A/R split
        self.counter_split_guid = guid()


def main(argv):
    if len(argv) < 2:
        raise SystemExit(__doc__)
    out_path = argv[1]
    two_receivable = "--two-receivable" in argv[2:]

    # ------------------------------------------------------------- accounts
    root = Account("Root Account", "ROOT")
    assets = Account("Assets", "ASSET", root, placeholder=True)
    liab = Account("Liabilities", "LIABILITY", root, placeholder=True)
    income = Account("Income", "INCOME", root, placeholder=True)
    expense = Account("Expenses", "EXPENSE", root, placeholder=True)
    checking = Account("Checking Account", "BANK", assets)
    ar = Account("Accounts Receivable", "RECEIVABLE", assets)
    ar2 = Account("Accounts Receivable 2", "RECEIVABLE", assets)
    ap = Account("Accounts Payable", "PAYABLE", liab)
    sales = Account("Sales", "INCOME", income)
    office = Account("Office Supplies", "EXPENSE", expense)
    accounts = [root, assets, liab, income, expense, checking, ar, ap, sales,
                office]
    if two_receivable:
        accounts.append(ar2)

    # --------------------------------------------------------------- owners
    alpha = Owner("customer", "Alpha Corp", "C001", "1 Alpha Way")
    bravo = Owner("customer", "Bravo Ltd", "C002", "2 Bravo Street")
    delta = Owner("vendor", "Delta Supply", "V001", "3 Delta Road")
    echo = Owner("vendor", "Echo Parts", "V002", "4 Echo Avenue")
    owners = [alpha, bravo, delta, echo]

    # Bravo's invoices go to the second A/Receivable account when the
    # multi-account layout is being tested.
    bravos_ar = ar2 if two_receivable else ar

    # ------------------------------------------------------------- invoices
    customer_invoices = [
        Invoice(alpha, "INV-100", "2026-01-15", "2026-02-14", 10000, ar,
                sales, "Invoice"),
        Invoice(alpha, "INV-101", "2026-04-20", "2026-05-20", 20000, ar,
                sales, "Invoice"),
        Invoice(alpha, "INV-105", "2026-03-01", "2026-03-31", 6000, ar,
                sales, "Invoice"),
        Invoice(alpha, "INV-106", "2026-02-01", "2026-04-01", 2500, ar,
                sales, "Invoice"),
        Invoice(bravo, "INV-102", "2026-06-05", "2026-07-05", 30000, bravos_ar,
                sales, "Invoice"),
        Invoice(bravo, "INV-103", "2026-05-10", "2026-06-09", 40000, bravos_ar,
                sales, "Invoice"),
        Invoice(bravo, "INV-104", "2026-03-16", "2026-04-15", 50000, bravos_ar,
                sales, "Invoice"),
        Invoice(bravo, "INV-107", "2026-06-01", "2026-06-30", 5000, bravos_ar,
                sales, "Invoice"),
        # A credit note is an invoice transaction with the opposite sign, so
        # its lot balance is negative and it must reduce its bucket.
        Invoice(alpha, "CN-300", "2026-05-05", "2026-06-04", -4000, ar,
                sales, "Credit Note", credit_note=True),
    ]
    vendor_bills = [
        Invoice(delta, "BILL-200", "2026-02-01", "2026-03-03", 11100, ap,
                office, "Bill"),
        Invoice(delta, "BILL-201", "2026-05-15", "2026-06-14", 22200, ap,
                office, "Bill"),
        Invoice(echo, "BILL-202", "2026-06-10", "2026-07-10", 33300, ap,
                office, "Bill"),
    ]
    invoices = customer_invoices + vendor_bills

    # ------------------------------------------------------------- payments
    inv101 = next(i for i in invoices if i.number == "INV-101")
    inv105 = next(i for i in invoices if i.number == "INV-105")
    bill201 = next(i for i in invoices if i.number == "BILL-201")
    bravo_prepay_lot = Lot(bravos_ar, owner=bravo, title="Prepayment Bravo Ltd")
    delta_prepay_lot = Lot(ap, owner=delta, title="Prepayment Delta Supply")

    payments = [
        Payment(alpha, "PMT-101", "2026-05-25", 5000, ar, checking,
                invoice=inv101, memo="Partial payment INV-101"),
        Payment(alpha, "PMT-105", "2026-03-20", 6000, ar, checking,
                invoice=inv105, memo="Payment INV-105"),
        Payment(bravo, "PMT-PRE", "2026-06-20", 7500, bravos_ar, checking,
                lot=bravo_prepay_lot, memo="Prepayment without invoice"),
        Payment(delta, "PMT-201", "2026-05-20", 2200, ap, checking,
                invoice=bill201, memo="Partial payment BILL-201"),
        Payment(delta, "PMT-PRE", "2026-06-25", 4400, ap, checking,
                lot=delta_prepay_lot, memo="Vendor prepayment"),
    ]

    # --------------------------------------------------------------- write
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
            ("employee", "employee"), ("entry", "entry"),
            ("invoice", "invoice"), ("job", "job"), ("order", "order"),
            ("owner", "owner"), ("taxtable", "taxtable"), ("tte", "tte"),
            ("vendor", "vendor")):
        w(f'     xmlns:{prefix}="http://www.gnucash.org/XML/{uri}"' +
          (">" if prefix == "vendor" else ""))
    w('<gnc:count-data cd:type="book">1</gnc:count-data>')
    w('<gnc:book version="2.0.0">')
    w(f'<book:id type="guid">{guid()}</book:id>')
    w('<gnc:count-data cd:type="commodity">1</gnc:count-data>')
    w(f'<gnc:count-data cd:type="account">{len(accounts)}</gnc:count-data>')
    w(f'<gnc:count-data cd:type="transaction">'
      f'{len(invoices) + len(payments)}</gnc:count-data>')
    w(f'<gnc:count-data cd:type="gnc:GncEntry">{len(invoices)}'
      '</gnc:count-data>')
    w(f'<gnc:count-data cd:type="gnc:GncInvoice">{len(invoices)}'
      '</gnc:count-data>')
    w('<gnc:count-data cd:type="gnc:GncCustomer">2</gnc:count-data>')
    w('<gnc:count-data cd:type="gnc:GncVendor">2</gnc:count-data>')

    w('<gnc:commodity version="2.0.0">')
    w('  <cmdty:space>CURRENCY</cmdty:space>')
    w('  <cmdty:id>USD</cmdty:id>')
    w('  <cmdty:name>US Dollar</cmdty:name>')
    w('  <cmdty:fraction>100</cmdty:fraction>')
    w('</gnc:commodity>')

    def lot_tree(lot):
        w('    <gnc:lot version="2.0.0">')
        w(f'      <lot:id type="guid">{lot.guid}</lot:id>')
        w('      <lot:slots>')
        if lot.invoice is not None:
            w('        <slot>')
            w('          <slot:key>gncInvoice</slot:key>')
            w('          <slot:value type="frame">')
            w('            <slot>')
            w('              <slot:key>invoice-guid</slot:key>')
            w(f'              <slot:value type="guid">{lot.invoice.guid}'
              '</slot:value>')
            w('            </slot>')
            w('          </slot:value>')
            w('        </slot>')
        if lot.owner is not None:
            kind = {"customer": 2, "vendor": 4}[lot.owner.kind]
            w('        <slot>')
            w('          <slot:key>gncOwner</slot:key>')
            w('          <slot:value type="frame">')
            w('            <slot>')
            w('              <slot:key>owner-type</slot:key>')
            w(f'              <slot:value type="integer">{kind}'
              '</slot:value>')
            w('            </slot>')
            w('            <slot>')
            w('              <slot:key>owner-guid</slot:key>')
            w(f'              <slot:value type="guid">{lot.owner.guid}'
              '</slot:value>')
            w('            </slot>')
            w('          </slot:value>')
            w('        </slot>')
        w('        <slot>')
        w('          <slot:key>title</slot:key>')
        w(f'          <slot:value type="string">{lot.title}</slot:value>')
        w('        </slot>')
        w('      </lot:slots>')
        w('    </gnc:lot>')

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
        if account.lots:
            w('  <act:lots>')
            for lot in account.lots:
                lot_tree(lot)
            w('  </act:lots>')
        w('</gnc:account>')

    def transaction(txn_id, number, posted, description, type_slot, due,
                    splits, invoice_guid=None):
        w('<gnc:transaction version="2.0.0">')
        w(f'  <trn:id type="guid">{txn_id}</trn:id>')
        w('  <trn:currency>')
        w('    <cmdty:space>CURRENCY</cmdty:space>')
        w('    <cmdty:id>USD</cmdty:id>')
        w('  </trn:currency>')
        w(f'  <trn:num>{number}</trn:num>')
        w('  <trn:date-posted>')
        w(f'    <ts:date>{ts(posted)}</ts:date>')
        w('  </trn:date-posted>')
        w('  <trn:date-entered>')
        w(f'    <ts:date>{ts(posted, "20:39:41")}</ts:date>')
        w('  </trn:date-entered>')
        w(f'  <trn:description>{description}</trn:description>')
        w('  <trn:slots>')
        if invoice_guid is not None:
            w('    <slot>')
            w('      <slot:key>gncInvoice</slot:key>')
            w('      <slot:value type="frame">')
            w('        <slot>')
            w('          <slot:key>invoice-guid</slot:key>')
            w(f'          <slot:value type="guid">{invoice_guid}</slot:value>')
            w('        </slot>')
            w('      </slot:value>')
            w('    </slot>')
        if due is not None:
            w('    <slot>')
            w('      <slot:key>trans-date-due</slot:key>')
            w('      <slot:value type="timespec">')
            w(f'        <ts:date>{ts(due)}</ts:date>')
            w('      </slot:value>')
            w('    </slot>')
        w('    <slot>')
        w('      <slot:key>trans-txn-type</slot:key>')
        w(f'      <slot:value type="string">{type_slot}</slot:value>')
        w('    </slot>')
        w('  </trn:slots>')
        w('  <trn:splits>')
        for split_id, amount, account, memo, action, lot in splits:
            w('    <trn:split>')
            w(f'      <split:id type="guid">{split_id}</split:id>')
            if memo:
                w(f'      <split:memo>{memo}</split:memo>')
            if action:
                w(f'      <split:action>{action}</split:action>')
            w('      <split:reconciled-state>n</split:reconciled-state>')
            w(f'      <split:value>{money(amount)}</split:value>')
            w(f'      <split:quantity>{money(amount)}</split:quantity>')
            w(f'      <split:account type="guid">{account.guid}'
              '</split:account>')
            if lot is not None:
                w(f'      <split:lot type="guid">{lot.guid}</split:lot>')
            w('    </trn:split>')
        w('  </trn:splits>')
        w('</gnc:transaction>')

    for invoice in invoices:
        # The A/P or A/R split carries the invoice lot, the counter split does
        # not.  A customer invoice debits A/Receivable, a vendor bill credits
        # A/Payable.
        sign = 1 if invoice.account.type == "RECEIVABLE" else -1
        transaction(invoice.txn_guid, invoice.number, invoice.posted,
                    invoice.owner.name, "I", invoice.due,
                    [(invoice.split_guid, sign * invoice.cents,
                      invoice.account, invoice.number, invoice.action,
                      invoice.lot),
                     (invoice.counter_split_guid, -sign * invoice.cents,
                      invoice.counter_account, invoice.number,
                      invoice.action, None)],
                    invoice_guid=invoice.guid)

    for payment in payments:
        lot = payment.invoice.lot if payment.invoice is not None else payment.lot
        if payment.account.type == "RECEIVABLE":
            apar_amount = -payment.cents     # a customer receipt credits A/R
            bank_amount = payment.cents
        else:
            apar_amount = payment.cents      # a vendor payment debits A/P
            bank_amount = -payment.cents
        transaction(payment.txn_guid, payment.number, payment.posted,
                    payment.owner.name, "P", None,
                    [(payment.owner_split_guid, apar_amount, payment.account,
                      payment.memo, "Payment", lot),
                     (payment.counter_split_guid, bank_amount,
                      payment.counter_account, payment.memo, "Payment",
                      None)])

    for invoice in invoices:
        w('<gnc:GncEntry version="2.0.0">')
        w(f'  <entry:guid type="guid">{invoice.entry_guid}</entry:guid>')
        w('  <entry:date>')
        w(f'    <ts:date>{ts(invoice.posted, "08:00:00")}</ts:date>')
        w('  </entry:date>')
        w('  <entry:entered>')
        w(f'    <ts:date>{ts(invoice.posted, "08:05:00")}</ts:date>')
        w('  </entry:entered>')
        w(f'  <entry:description>{invoice.number}</entry:description>')
        w(f'  <entry:action>{invoice.action}</entry:action>')
        w('  <entry:qty>1000000/1000000</entry:qty>')
        if invoice.account.type == "RECEIVABLE":
            w(f'  <entry:i-acct type="guid">{invoice.counter_account.guid}'
              '</entry:i-acct>')
            w(f'  <entry:i-price>{money(invoice.cents)}</entry:i-price>')
            w('  <entry:i-taxable>0</entry:i-taxable>')
            w('  <entry:i-taxincluded>0</entry:i-taxincluded>')
        else:
            w(f'  <entry:b-acct type="guid">{invoice.counter_account.guid}'
              '</entry:b-acct>')
            w(f'  <entry:b-price>{money(invoice.cents)}</entry:b-price>')
            w('  <entry:b-taxable>0</entry:b-taxable>')
            w('  <entry:b-taxincluded>0</entry:b-taxincluded>')
        w('  <entry:billable>0</entry:billable>')
        if invoice.account.type == "RECEIVABLE":
            w(f'  <entry:invoice type="guid">{invoice.guid}</entry:invoice>')
        else:
            w(f'  <entry:bill type="guid">{invoice.guid}</entry:bill>')
        w('</gnc:GncEntry>')

    for invoice in invoices:
        w('<gnc:GncInvoice version="2.0.0">')
        w(f'  <invoice:guid type="guid">{invoice.guid}</invoice:guid>')
        w(f'  <invoice:id>{invoice.number}</invoice:id>')
        w('  <invoice:owner version="2.0.0">')
        w('    <owner:type>gncCustomer</owner:type>'
          if invoice.owner.kind == "customer" else
          '    <owner:type>gncVendor</owner:type>')
        w(f'    <owner:id type="guid">{invoice.owner.guid}</owner:id>')
        w('  </invoice:owner>')
        w('  <invoice:opened>')
        w(f'    <ts:date>{ts(invoice.posted, "08:00:00")}</ts:date>')
        w('  </invoice:opened>')
        w('  <invoice:posted>')
        w(f'    <ts:date>{ts(invoice.posted)}</ts:date>')
        w('  </invoice:posted>')
        w('  <invoice:active>1</invoice:active>')
        w(f'  <invoice:posttxn type="guid">{invoice.txn_guid}'
          '</invoice:posttxn>')
        w(f'  <invoice:postlot type="guid">{invoice.lot.guid}'
          '</invoice:postlot>')
        w(f'  <invoice:postacc type="guid">{invoice.account.guid}'
          '</invoice:postacc>')
        w('  <invoice:currency>')
        w('    <cmdty:space>CURRENCY</cmdty:space>')
        w('    <cmdty:id>USD</cmdty:id>')
        w('  </invoice:currency>')
        w('  <invoice:slots>')
        w('    <slot>')
        w('      <slot:key>credit-note</slot:key>')
        w(f'      <slot:value type="integer">'
          f'{1 if invoice.credit_note else 0}</slot:value>')
        w('    </slot>')
        w('  </invoice:slots>')
        w('</gnc:GncInvoice>')

    for owner in owners:
        tag = "GncCustomer" if owner.kind == "customer" else "GncVendor"
        prefix = "cust" if owner.kind == "customer" else "vendor"
        w(f'<gnc:{tag} version="2.0.0">')
        w(f'  <{prefix}:guid type="guid">{owner.guid}</{prefix}:guid>')
        w(f'  <{prefix}:name>{owner.name}</{prefix}:name>')
        w(f'  <{prefix}:id>{owner.owner_id}</{prefix}:id>')
        w(f'  <{prefix}:addr version="2.0.0">')
        w(f'    <addr:name>{owner.name}</addr:name>')
        w(f'    <addr:addr1>{owner.addr1}</addr:addr1>')
        w('    <addr:phone>555-0100</addr:phone>')
        w('    <addr:email>sales@example.com</addr:email>')
        w(f'  </{prefix}:addr>')
        if owner.kind == "customer":
            # The customer XML reader requires all of these; the vendor one
            # has neither a shipping address nor a credit limit.
            w('  <cust:shipaddr version="2.0.0">')
            w(f'    <addr:name>{owner.name}</addr:name>')
            w(f'    <addr:addr1>{owner.addr1}</addr:addr1>')
            w('  </cust:shipaddr>')
            w('  <cust:discount>0/100</cust:discount>')
            w('  <cust:credit>0/100</cust:credit>')
        w(f'  <{prefix}:taxincluded>USEGLOBAL</{prefix}:taxincluded>')
        w(f'  <{prefix}:active>{1 if owner.active else 0}</{prefix}:active>')
        w(f'  <{prefix}:currency>')
        w('    <cmdty:space>CURRENCY</cmdty:space>')
        w('    <cmdty:id>USD</cmdty:id>')
        w(f'  </{prefix}:currency>')
        if owner.kind == "vendor":
            w('  <vendor:use-tt>0</vendor:use-tt>')
        w(f'</gnc:{tag}>')

    w('</gnc:book>')
    w('</gnc-v2>')

    with open(out_path, "w", encoding="utf-8") as handle:
        handle.write("\n".join(out) + "\n")
    print(f"wrote {out_path}: {len(invoices)} invoices, {len(payments)} "
          f"payments, {len(owners)} owners, {len(accounts)} accounts")


if __name__ == "__main__":
    main(sys.argv)
