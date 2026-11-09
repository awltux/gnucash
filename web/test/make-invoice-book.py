#!/usr/bin/env python3
"""Write a GnuCash XML book with invoices, a tax table and a payment, for the
invoice document report tests and the owner reports.

The fixture books in the source tree carry at most one invoice (sample1 has a
single, untaxed vendor bill and no customer), so they cannot exercise an
invoice document: entries with quantities and prices, a tax table applied to
some entries, billing terms, or a payment applied to the document.  This
script builds a book outside the repository with:

  * two customers (Acme Widgets Ltd, Beta Corp), one vendor (Delta Supply),
    one employee (Eve Employee) and one job (Website redesign, owned by
    Acme), each with an address and an id, and a company identity in the
    book's Business options (name, address, phone, email, company id);
  * a 10% sales-tax table posting to a Sales Tax liability account;
  * a Net 30 bill term;
  * a posted customer invoice INV-1001 with three entries -- two taxable at
    10% and one not, and a 10% discount on the third -- plus a partial
    payment, so the report has to print subtotal, tax, total, a payment row
    and the amount due;
  * a posted vendor bill BILL-2001 with one untaxed entry, plus a partial
    payment, so the report's vendor path is exercised too;
  * a second customer invoice INV-1002 for Beta Corp, so Customer Summary has
    a second customer row and the Customer Report has a second owner to
    select;
  * an employee expense voucher EXP-3001 for Eve Employee, posted to
    A/Payable, so the Employee Report has an owner and a document;
  * a job invoice JOB-4001 owned by the Website redesign job, posted to
    A/Receivable, so the Job Report has a document and the Customer Report
    for Acme exercises the end-owner (job -> customer) query.

The invoice, owner and job ids and GUIDs are printed at the end; the GUID is
what a saved Scheme report needs to select an object, the id is what the
native route takes.

Usage:
    make-invoice-book.py <output.gnucash>

How the invoice is encoded (the parts the report reads):

  * gnc:GncCustomer / gnc:GncVendor carry the owner name, id and address;
    gnc:GncInvoice/invoice:owner points at one of them.
  * gnc:GncInvoice carries the document id, posted date, billing id, notes
    and the posted transaction, lot and account.  The due date is not on the
    invoice object: gncInvoiceGetDateDue() reads the posted transaction's
    trans-date-due slot, so it is written there.
  * gnc:GncBillTerm (days type, bt-days:due-days) provides the billing terms
    the invoice points at with invoice:terms.
  * gnc:GncTaxTable holds one gnc:GncTaxTableEntry: its account (`tte:acct`),
    type `PERCENT` and amount 10/1 (ten percent; the engine reads a
    percentage as a bare number and divides by 100).  An entry opts into it
    with `entry:i-taxtable`, `entry:i-taxable` 1 and `entry:i-taxincluded` 0.
  * gnc:GncEntry carries the line: date, description, action, quantity, the
    income account (entry:i-acct) and unit price (entry:i-price), the tax
    table reference, and entry:invoice linking it to the document.
  * Every posted document has a normal GnuCash transaction.  Its A/R or A/P
    split carries the invoice lot (split:lot) and its value is the document
    total; the counter splits carry the net income and the tax.  The payment
    transaction's A/R split also carries the invoice lot, which is how the
    Scheme report finds the applied payment (gnc-lot-get-split-list).

Run it with python3; it needs no GnuCash libraries.
"""
import sys
import uuid

INVOICE_ID = "INV-1001"
BILL_ID = "BILL-2001"
SECOND_INVOICE_ID = "INV-1002"
EMPLOYEE_VOUCHER_ID = "EXP-3001"
JOB_INVOICE_ID = "JOB-4001"


def guid():
    return uuid.uuid4().hex


def ts(day, time="10:59:00"):
    return f"{day} {time} +0000"


def money(cents):
    """A gnc_numeric as the XML writes it: value over 100."""
    return f"{cents}/100"


def price(cents):
    """A unit price; the same representation, a different meaning."""
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
    def __init__(self, kind, name, owner_id, addr1, addr2, city):
        self.guid = guid()
        self.kind = kind            # "customer" | "vendor" | "employee"
        self.name = name
        self.owner_id = owner_id
        self.addr1 = addr1
        self.addr2 = addr2
        self.city = city


class Job:
    def __init__(self, name, job_id, owner):
        self.guid = guid()
        self.name = name
        self.job_id = job_id
        self.owner = owner          # the customer the job belongs to


def owner_tag(owner):
    """The XML owner:type for an Owner or Job."""
    if isinstance(owner, Job):
        return "gncJob"
    return {"customer": "gncCustomer", "vendor": "gncVendor",
            "employee": "gncEmployee"}[owner.kind]


class Lot:
    def __init__(self, account, invoice=None, title=""):
        self.guid = guid()
        self.account = account
        self.invoice = invoice
        self.title = title
        account.lots.append(self)


class Entry:
    def __init__(self, description, action, quantity, unit_price_cents,
                 account, taxable=False, discount_cents=0):
        self.guid = guid()
        self.description = description
        self.action = action
        self.quantity = quantity            # gnc_numeric numerator over 1e6
        self.unit_price_cents = unit_price_cents
        self.account = account
        self.taxable = taxable
        self.discount_cents = discount_cents


class Invoice:
    def __init__(self, owner, number, posted, due, account, counter_account,
                 kind, entries, terms=None, billing_id="", notes=""):
        self.guid = guid()
        self.owner = owner                  # Owner or Job
        self.number = number
        self.posted = posted
        self.due = due
        self.account = account              # A/Receivable or A/Payable
        self.counter_account = counter_account
        # "customer"/"job" documents use the customer entry fields (i-*);
        # "vendor"/"employee" documents use the bill fields (b-*).
        self.kind = kind
        self.entries = entries
        self.terms = terms
        self.billing_id = billing_id
        self.notes = notes
        self.txn_guid = guid()
        self.split_guid = guid()
        self.lot = Lot(account, invoice=self, title=f"{kind} {number}")


class Payment:
    def __init__(self, invoice, number, posted, cents, account,
                 counter_account, memo=""):
        self.invoice = invoice
        self.number = number
        self.posted = posted
        self.cents = cents
        self.account = account
        self.counter_account = counter_account
        self.memo = memo
        self.txn_guid = guid()
        self.owner_split_guid = guid()
        self.counter_split_guid = guid()


def main(argv):
    if len(argv) < 2:
        raise SystemExit(__doc__)
    out_path = argv[1]

    # ------------------------------------------------------------- accounts
    root = Account("Root Account", "ROOT")
    assets = Account("Assets", "ASSET", root, placeholder=True)
    liabilities = Account("Liabilities", "LIABILITY", root, placeholder=True)
    income = Account("Income", "INCOME", root, placeholder=True)
    expenses = Account("Expenses", "EXPENSE", root, placeholder=True)
    checking = Account("Checking Account", "BANK", assets)
    receivable = Account("Accounts Receivable", "RECEIVABLE", assets)
    payable = Account("Accounts Payable", "PAYABLE", liabilities)
    sales_tax = Account("Sales Tax", "LIABILITY", liabilities)
    consulting = Account("Consulting Income", "INCOME", income)
    licences = Account("Licence Income", "INCOME", income)
    support = Account("Support Income", "INCOME", income)
    office = Account("Office Supplies", "EXPENSE", expenses)
    travel = Account("Travel", "EXPENSE", expenses)
    accounts = [root, assets, liabilities, income, expenses, checking,
                receivable, payable, sales_tax, consulting, licences, support,
                office, travel]

    # --------------------------------------------------------------- owners
    acme = Owner("customer", "Acme Widgets Ltd", "C001", "12 Acme Way",
                 "Springfield", "IL 62701")
    beta = Owner("customer", "Beta Corp", "C002", "8 Beta Boulevard",
                 "Capital City", "IL 62702")
    delta = Owner("vendor", "Delta Supply", "V001", "3 Delta Road",
                  "Shelbyville", "IL 62565")
    eve = Owner("employee", "Eve Employee", "E001", "5 Eve Lane",
                "Ogdenville", "IL 62703")
    owners = [acme, beta, delta, eve]

    # The job belongs to Acme, so Acme's Customer Report also covers the job's
    # invoice: the Scheme query matches the invoice owner's *end* owner.
    website = Job("Website redesign", "JOB-001", acme)
    jobs = [website]

    # -------------------------------------------------------------- the tax
    # 10% of the taxable line, posted to the Sales Tax liability account.
    tax_table_guid = guid()
    tax_table_name = "Sales Tax 10%"

    # --------------------------------------------------------------- terms
    terms_guid = guid()

    # ------------------------------------------------------------ invoices
    # Amounts are chosen so the tax and the discount are exact to the cent:
    #   2 x 150.00                     = 300.00, tax 30.00
    #   1 x  99.99  (not taxable)      =  99.99, tax  0.00
    #   3 x  20.00 less 10% discount   =  54.00, tax  5.40
    # subtotal 453.99, tax 35.40, total 489.39.
    customer_invoice = Invoice(
        acme, INVOICE_ID, "2026-03-02", "2026-04-01", receivable, consulting,
        "customer",
        [Entry("Consulting", "Service", 2_000_000, 15000, consulting,
               taxable=True),
         Entry("Software licence", "Material", 1_000_000, 9999, licences),
         Entry("Support", "Service", 3_000_000, 2000, support, taxable=True,
               discount_cents=1000)],
        terms=terms_guid, billing_id="PO-7781",
        notes="Thank you for your business.")
    vendor_bill = Invoice(
        delta, BILL_ID, "2026-03-05", "2026-04-04", payable, office, "vendor",
        [Entry("Office chairs", "Material", 4_000_000, 12000, office)])
    # A second customer, so Customer Summary has two customer rows and the
    # Customer Report has an owner that is not the first one in the book.
    beta_invoice = Invoice(
        beta, SECOND_INVOICE_ID, "2026-03-10", "2026-04-09", receivable,
        consulting, "customer",
        [Entry("Consulting", "Service", 1_000_000, 15000, consulting)])
    # An employee expense voucher: an A/Payable document with bill entries.
    employee_voucher = Invoice(
        eve, EMPLOYEE_VOUCHER_ID, "2026-03-12", "2026-04-11", payable,
        travel, "employee",
        [Entry("Client visit travel", "Material", 2_000_000, 9000, travel)])
    # A job invoice: a customer document whose owner is the job.
    job_invoice = Invoice(
        website, JOB_INVOICE_ID, "2026-03-15", "2026-04-14", receivable,
        consulting, "job",
        [Entry("Website design", "Service", 1_000_000, 26000, consulting)])
    invoices = [customer_invoice, vendor_bill, beta_invoice, employee_voucher,
                job_invoice]

    payment = Payment(customer_invoice, "PMT-1001", "2026-03-20", 20000,
                      receivable, checking, memo="Partial payment INV-1001")
    # A vendor payment, so the Vendor Report has a payment row too.
    vendor_payment = Payment(vendor_bill, "PMT-2001", "2026-03-25", 10000,
                             payable, checking,
                             memo="Partial payment BILL-2001")
    payments = [payment, vendor_payment]

    invoice_total_cents = 48939
    invoice_subtotal_cents = 45399
    invoice_tax_cents = 3540

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

    # The Scheme invoice report's "company table" reads these Business
    # options through qof-book-get-option; the same path the native model
    # reads.  qof_book_get_option prepends the "options" frame, so the
    # Business frame lives under a top-level "options" key rather than at
    # the root of the book slots.  They are not part of the numbers the
    # parity test compares, but they make the document an actual invoice.
    w('<book:slots>')
    w('  <slot>')
    w('    <slot:key>options</slot:key>')
    w('    <slot:value type="frame">')
    w('      <slot>')
    w('        <slot:key>Business</slot:key>')
    w('        <slot:value type="frame">')
    for key, value in (
            ("Company Name", "Example Trading Co"),
            ("Company Contact Person", "Jane Doe"),
            ("Company Address", "99 Market Street\nChicago, IL 60601"),
            ("Company Phone Number", "555-0100"),
            ("Company Email Address", "billing@example.com"),
            ("Company Website URL", "https://example.com"),
            ("Company ID", "EIN-12-3456789")):
        w('          <slot>')
        w(f'            <slot:key>{key}</slot:key>')
        w(f'            <slot:value type="string">{value}</slot:value>')
        w('          </slot>')
    w('        </slot:value>')
    w('      </slot>')
    w('    </slot:value>')
    w('  </slot>')
    w('</book:slots>')

    w('<gnc:count-data cd:type="commodity">1</gnc:count-data>')
    w(f'<gnc:count-data cd:type="account">{len(accounts)}</gnc:count-data>')
    w(f'<gnc:count-data cd:type="transaction">'
      f'{len(invoices) + len(payments)}</gnc:count-data>')
    w(f'<gnc:count-data cd:type="gnc:GncEntry">'
      f'{sum(len(i.entries) for i in invoices)}</gnc:count-data>')
    w(f'<gnc:count-data cd:type="gnc:GncInvoice">{len(invoices)}'
      '</gnc:count-data>')
    w('<gnc:count-data cd:type="gnc:GncCustomer">2</gnc:count-data>')
    w('<gnc:count-data cd:type="gnc:GncVendor">1</gnc:count-data>')
    w('<gnc:count-data cd:type="gnc:GncEmployee">1</gnc:count-data>')
    w('<gnc:count-data cd:type="gnc:GncJob">1</gnc:count-data>')
    w('<gnc:count-data cd:type="gnc:GncTaxTable">1</gnc:count-data>')
    w('<gnc:count-data cd:type="gnc:GncBillTerm">1</gnc:count-data>')

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

    # ---------------------------------------------------------- tax table
    w('<gnc:GncTaxTable version="2.0.0">')
    w(f'  <taxtable:guid type="guid">{tax_table_guid}</taxtable:guid>')
    w(f'  <taxtable:name>{tax_table_name}</taxtable:name>')
    w('  <taxtable:refcount>2</taxtable:refcount>')
    w('  <taxtable:invisible>0</taxtable:invisible>')
    w('  <taxtable:entries>')
    w('    <gnc:GncTaxTableEntry>')
    w(f'      <tte:acct type="guid">{sales_tax.guid}</tte:acct>')
    w('      <tte:amount>10/1</tte:amount>')
    w('      <tte:type>PERCENT</tte:type>')
    w('    </gnc:GncTaxTableEntry>')
    w('  </taxtable:entries>')
    w('</gnc:GncTaxTable>')

    # ----------------------------------------------------------- bill term
    w('<gnc:GncBillTerm version="2.0.0">')
    w(f'  <billterm:guid type="guid">{terms_guid}</billterm:guid>')
    w('  <billterm:name>Net 30</billterm:name>')
    w('  <billterm:desc>Net 30</billterm:desc>')
    w('  <billterm:refcount>1</billterm:refcount>')
    w('  <billterm:invisible>0</billterm:invisible>')
    w('  <billterm:days>')
    w('    <bt-days:due-days>30</bt-days:due-days>')
    w('  </billterm:days>')
    w('</gnc:GncBillTerm>')

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
            w(f'          <slot:value type="guid">{invoice_guid}'
              '</slot:value>')
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

    # Customer invoice: debit A/R with the document total, credit the income
    # accounts with the net lines and the tax account with the tax.
    transaction(customer_invoice.txn_guid, customer_invoice.number,
                customer_invoice.posted, acme.name, "I", customer_invoice.due,
                [(customer_invoice.split_guid, invoice_total_cents,
                  receivable, customer_invoice.number, "Invoice",
                  customer_invoice.lot),
                 (guid(), -30000, consulting, "Consulting", "Invoice", None),
                 (guid(), -9999, licences, "Software licence", "Invoice",
                  None),
                 (guid(), -5400, support, "Support", "Invoice", None),
                 (guid(), -invoice_tax_cents, sales_tax, "Sales Tax", "Invoice",
                  None)],
                invoice_guid=customer_invoice.guid)
    # Vendor bill: credit A/P, debit the expense.
    transaction(vendor_bill.txn_guid, vendor_bill.number, vendor_bill.posted,
                delta.name, "I", vendor_bill.due,
                [(vendor_bill.split_guid, -48000, payable, vendor_bill.number,
                  "Bill", vendor_bill.lot),
                 (guid(), 48000, office, "Office chairs", "Bill", None)],
                invoice_guid=vendor_bill.guid)
    # Second customer invoice.
    transaction(beta_invoice.txn_guid, beta_invoice.number,
                beta_invoice.posted, beta.name, "I", beta_invoice.due,
                [(beta_invoice.split_guid, 15000, receivable,
                  beta_invoice.number, "Invoice", beta_invoice.lot),
                 (guid(), -15000, consulting, "Consulting", "Invoice", None)],
                invoice_guid=beta_invoice.guid)
    # Employee expense voucher: credit A/P, debit the expense.
    transaction(employee_voucher.txn_guid, employee_voucher.number,
                employee_voucher.posted, eve.name, "I", employee_voucher.due,
                [(employee_voucher.split_guid, -18000, payable,
                  employee_voucher.number, "Expense",
                  employee_voucher.lot),
                 (guid(), 18000, travel, "Client visit travel", "Expense",
                  None)],
                invoice_guid=employee_voucher.guid)
    # Job invoice: debit A/R, credit income.
    transaction(job_invoice.txn_guid, job_invoice.number,
                job_invoice.posted, website.name, "I", job_invoice.due,
                [(job_invoice.split_guid, 26000, receivable,
                  job_invoice.number, "Invoice", job_invoice.lot),
                 (guid(), -26000, consulting, "Website design", "Invoice",
                  None)],
                invoice_guid=job_invoice.guid)
    # Payments: the A/R or A/P split carries the invoice lot, so it is an
    # applied payment rather than a prepayment.
    transaction(payment.txn_guid, payment.number, payment.posted, acme.name,
                "P", None,
                [(payment.owner_split_guid, -payment.cents, receivable,
                  payment.memo, "Payment", customer_invoice.lot),
                 (payment.counter_split_guid, payment.cents, checking,
                  payment.memo, "Payment", None)])
    transaction(vendor_payment.txn_guid, vendor_payment.number,
                vendor_payment.posted, delta.name, "P", None,
                [(vendor_payment.owner_split_guid, vendor_payment.cents,
                  payable, vendor_payment.memo, "Payment", vendor_bill.lot),
                 (vendor_payment.counter_split_guid, -vendor_payment.cents,
                  checking, vendor_payment.memo, "Payment", None)])

    for invoice in invoices:
        for entry in invoice.entries:
            w('<gnc:GncEntry version="2.0.0">')
            w(f'  <entry:guid type="guid">{entry.guid}</entry:guid>')
            w('  <entry:date>')
            w(f'    <ts:date>{ts(invoice.posted, "08:00:00")}</ts:date>')
            w('  </entry:date>')
            w('  <entry:entered>')
            w(f'    <ts:date>{ts(invoice.posted, "08:05:00")}</ts:date>')
            w('  </entry:entered>')
            w(f'  <entry:description>{entry.description}</entry:description>')
            w(f'  <entry:action>{entry.action}</entry:action>')
            w(f'  <entry:qty>{entry.quantity}/1000000</entry:qty>')
            if invoice.kind in ("customer", "job"):
                w(f'  <entry:i-acct type="guid">{entry.account.guid}'
                  '</entry:i-acct>')
                w(f'  <entry:i-price>{price(entry.unit_price_cents)}'
                  '</entry:i-price>')
                if entry.discount_cents:
                    w(f'  <entry:i-discount>{money(entry.discount_cents)}'
                      '</entry:i-discount>')
                    w('  <entry:i-disc-type>PERCENT</entry:i-disc-type>')
                    w('  <entry:i-disc-how>PRETAX</entry:i-disc-how>')
                w(f'  <entry:i-taxable>{1 if entry.taxable else 0}'
                  '</entry:i-taxable>')
                w('  <entry:i-taxincluded>0</entry:i-taxincluded>')
                if entry.taxable:
                    w(f'  <entry:i-taxtable type="guid">{tax_table_guid}'
                      '</entry:i-taxtable>')
                w(f'  <entry:invoice type="guid">{invoice.guid}'
                  '</entry:invoice>')
            else:
                w(f'  <entry:b-acct type="guid">{entry.account.guid}'
                  '</entry:b-acct>')
                w(f'  <entry:b-price>{price(entry.unit_price_cents)}'
                  '</entry:b-price>')
                w('  <entry:b-taxable>0</entry:b-taxable>')
                w('  <entry:b-taxincluded>0</entry:b-taxincluded>')
                w(f'  <entry:bill type="guid">{invoice.guid}</entry:bill>')
            w('  <entry:billable>0</entry:billable>')
            w('</gnc:GncEntry>')

    for invoice in invoices:
        w('<gnc:GncInvoice version="2.0.0">')
        w(f'  <invoice:guid type="guid">{invoice.guid}</invoice:guid>')
        w(f'  <invoice:id>{invoice.number}</invoice:id>')
        w('  <invoice:owner version="2.0.0">')
        w(f'    <owner:type>{owner_tag(invoice.owner)}</owner:type>')
        w(f'    <owner:id type="guid">{invoice.owner.guid}</owner:id>')
        w('  </invoice:owner>')
        w('  <invoice:opened>')
        w(f'    <ts:date>{ts(invoice.posted, "08:00:00")}</ts:date>')
        w('  </invoice:opened>')
        w('  <invoice:posted>')
        w(f'    <ts:date>{ts(invoice.posted)}</ts:date>')
        w('  </invoice:posted>')
        if invoice.terms is not None:
            w(f'  <invoice:terms type="guid">{invoice.terms}'
              '</invoice:terms>')
        if invoice.billing_id:
            w(f'  <invoice:billing_id>{invoice.billing_id}'
              '</invoice:billing_id>')
        if invoice.notes:
            w(f'  <invoice:notes>{invoice.notes}</invoice:notes>')
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
        w('      <slot:value type="integer">0</slot:value>')
        w('    </slot>')
        w('  </invoice:slots>')
        w('</gnc:GncInvoice>')

    for owner in owners:
        if owner.kind == "employee":
            tag = "GncEmployee"
            prefix = "employee"
        elif owner.kind == "customer":
            tag = "GncCustomer"
            prefix = "cust"
        else:
            tag = "GncVendor"
            prefix = "vendor"
        w(f'<gnc:{tag} version="2.0.0">')
        w(f'  <{prefix}:guid type="guid">{owner.guid}</{prefix}:guid>')
        if owner.kind == "employee":
            w(f'  <employee:username>{owner.name}</employee:username>')
        else:
            w(f'  <{prefix}:name>{owner.name}</{prefix}:name>')
        w(f'  <{prefix}:id>{owner.owner_id}</{prefix}:id>')
        w(f'  <{prefix}:addr version="2.0.0">')
        w(f'    <addr:name>{owner.name}</addr:name>')
        w(f'    <addr:addr1>{owner.addr1}</addr:addr1>')
        w(f'    <addr:addr2>{owner.addr2}</addr:addr2>')
        w(f'    <addr:addr3>{owner.city}</addr:addr3>')
        w('    <addr:phone>555-0100</addr:phone>')
        w('    <addr:email>accounts@example.com</addr:email>')
        w(f'  </{prefix}:addr>')
        if owner.kind == "customer":
            # The customer XML reader requires these; the vendor one has
            # neither a shipping address nor a credit limit.
            w('  <cust:shipaddr version="2.0.0">')
            w(f'    <addr:name>{owner.name}</addr:name>')
            w(f'    <addr:addr1>{owner.addr1}</addr:addr1>')
            w('  </cust:shipaddr>')
            w('  <cust:discount>0/100</cust:discount>')
            w('  <cust:credit>0/100</cust:credit>')
        if owner.kind == "employee":
            w('  <employee:workday>8/1</employee:workday>')
            w('  <employee:rate>0/100</employee:rate>')
        else:
            w(f'  <{prefix}:taxincluded>USEGLOBAL</{prefix}:taxincluded>')
        w(f'  <{prefix}:active>1</{prefix}:active>')
        w(f'  <{prefix}:currency>')
        w('    <cmdty:space>CURRENCY</cmdty:space>')
        w('    <cmdty:id>USD</cmdty:id>')
        w(f'  </{prefix}:currency>')
        if owner.kind == "vendor":
            w('  <vendor:use-tt>0</vendor:use-tt>')
        w(f'</gnc:{tag}>')

    for job in jobs:
        w('<gnc:GncJob version="2.0.0">')
        w(f'  <job:guid type="guid">{job.guid}</job:guid>')
        w(f'  <job:id>{job.job_id}</job:id>')
        w(f'  <job:name>{job.name}</job:name>')
        w('  <job:owner version="2.0.0">')
        w(f'    <owner:type>{owner_tag(job.owner)}</owner:type>')
        w(f'    <owner:id type="guid">{job.owner.guid}</owner:id>')
        w('  </job:owner>')
        w('  <job:active>1</job:active>')
        w('</gnc:GncJob>')

    w('</gnc:book>')
    w('</gnc-v2>')

    with open(out_path, "w", encoding="utf-8") as handle:
        handle.write("\n".join(out) + "\n")
    print(f"wrote {out_path}: {len(invoices)} invoices, "
          f"{sum(len(i.entries) for i in invoices)} entries, "
          f"1 tax table ({tax_table_name}), 1 bill term, "
          f"{len(payments)} payments")
    print(f"invoice {INVOICE_ID} guid {customer_invoice.guid}")
    print(f"invoice {BILL_ID} guid {vendor_bill.guid}")
    print(f"invoice {SECOND_INVOICE_ID} guid {beta_invoice.guid}")
    print(f"invoice {EMPLOYEE_VOUCHER_ID} guid {employee_voucher.guid}")
    print(f"invoice {JOB_INVOICE_ID} guid {job_invoice.guid}")
    print(f"customer {acme.name} guid {acme.guid}")
    print(f"customer {beta.name} guid {beta.guid}")
    print(f"vendor {delta.name} guid {delta.guid}")
    print(f"employee {eve.name} guid {eve.guid}")
    print(f"job {website.name} guid {website.guid}")
    print(f"amounts: subtotal {invoice_subtotal_cents}/100, "
          f"tax {invoice_tax_cents}/100, total {invoice_total_cents}/100")

    # A machine-readable companion so a test can select an owner without
    # parsing the human-readable lines above.  One "kind name guid" per row.
    identifiers = [
        ("invoice", INVOICE_ID, customer_invoice.guid),
        ("invoice", BILL_ID, vendor_bill.guid),
        ("invoice", SECOND_INVOICE_ID, beta_invoice.guid),
        ("invoice", EMPLOYEE_VOUCHER_ID, employee_voucher.guid),
        ("invoice", JOB_INVOICE_ID, job_invoice.guid),
        ("customer", acme.name, acme.guid),
        ("customer", beta.name, beta.guid),
        ("vendor", delta.name, delta.guid),
        ("employee", eve.name, eve.guid),
        ("job", website.name, website.guid),
    ]
    with open(out_path + ".ids", "w", encoding="utf-8") as handle:
        for kind, name, value in identifiers:
            handle.write(f"{kind}\t{name}\t{value}\n")


if __name__ == "__main__":
    main(sys.argv)
