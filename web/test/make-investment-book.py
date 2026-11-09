#!/usr/bin/env python3
"""Write a GnuCash XML book with a brokerage account, stock lots and prices,
for the Investment Lots report tests.

The fixture books in the source tree have no stock account at all (the
old-format xml2/*.gml2 books have stock accounts but no price database and no
lots), so the native Investment Lots report cannot be compared with the Scheme
one on them.  This script builds a book outside the repository with:

  * one security, NASDAQ:ACME (fraction 10000, i.e. four share decimals);
  * a stock account, Assets:Investments:Brokerage, in that security, and a
    USD bank account under Assets:Bank:Checking;
  * an income account (Income:Capital Gains) and an expense account
    (Expenses:Commissions), so gains and fees have somewhere to go;
  * four lots, chosen so that every column of the report has something to
    show:
      - lot A: bought 100 ACME on 2025-03-10 (before the report's default
        start date), two partial sales in 2026 (one at a gain, one at a
        smaller gain per share), so it exercises the pre-window purchase
        handling, the pro-rata basis of two sales, long-term treatment, and
        the still-open end amount;
      - lot B: bought 50 ACME on 2025-08-15 and never sold, so its realised
        columns stay empty and its unrealised gain flips between short and
        long term depending on the report date;
      - lot C: bought 40 ACME on 2026-02-20 (inside the report window) and
        partly sold on 2026-04-01, so both bought and sold columns have
        in-window numbers, and the sale is short-term;
      - lot D: bought 20 ACME on 2026-01-05 and sold out on 2026-06-01, a
        closed lot -- its end amount is zero and it gets a close date;
  * a "Realized Gain/Loss" split (zero amount, non-zero value, in the same
    lot) for each sale, the way GnuCash's capital-gains code records them, so
    the report's gains-mismatch check has the right number to compare with;
  * a price database with ACME prices in USD at five dates, including one the
    day before the default report date, one out of range of the earlier
    report date (so the "No price found" path can be exercised), and one that
    makes the end value differ from the basis.

Usage:
    make-investment-book.py <output.gnucash> [--two-accounts]

The book is deliberately plain: one currency (USD), no trading accounts, no
fees on the trades.  Amounts are written as the XML writes them (value over
the transaction currency's fraction, quantity over the account's fraction).

With --two-accounts a second stock account, Assets:Investments:Retirement,
carries a lot of the same security with one partial sale, which is what
exercises the report's per-account sections and its grand total.

Run it with python3; it needs no GnuCash libraries.
"""
import sys
import uuid

SHARE_SCALE = 10000            # NASDAQ:ACME's fraction in this book
USD_SCALE = 100


def guid():
    return uuid.uuid4().hex


def ts(day, time="10:59:00"):
    return f"{day} {time} +0000"


def usd(cents):
    """A USD gnc_numeric as the XML writes it."""
    return f"{cents}/{USD_SCALE}"


def shares(count):
    """A share quantity; count may be a fraction of a share."""
    return f"{int(round(count * SHARE_SCALE))}/{SHARE_SCALE}"


class Account:
    def __init__(self, name, type_, parent=None, placeholder=False,
                 commodity=("CURRENCY", "USD"), fraction=USD_SCALE):
        self.guid = guid()
        self.name = name
        self.type = type_
        self.parent = parent
        self.placeholder = placeholder
        self.commodity = commodity
        self.fraction = fraction
        self.lots = []


class Lot:
    def __init__(self, account, title):
        self.guid = guid()
        self.account = account
        self.title = title
        account.lots.append(self)


class Transaction:
    def __init__(self, number, posted, description, splits, action=""):
        self.guid = guid()
        self.number = number
        self.posted = posted
        self.description = description
        self.action = action
        self.splits = splits


class Split:
    """One split: value in the transaction currency, quantity in the
    account's own commodity."""

    def __init__(self, account, value_cents, quantity, lot=None, memo="",
                 action=""):
        self.guid = guid()
        self.account = account
        self.value_cents = value_cents
        self.quantity = quantity      # already an XML fraction string
        self.lot = lot
        self.memo = memo
        self.action = action


def main(argv):
    if len(argv) < 2:
        raise SystemExit(__doc__)
    out_path = argv[1]
    two_accounts = "--two-accounts" in argv[2:]

    # ------------------------------------------------------------- accounts
    root = Account("Root Account", "ROOT")
    assets = Account("Assets", "ASSET", root, placeholder=True)
    liabilities = Account("Liabilities", "LIABILITY", root, placeholder=True)
    income = Account("Income", "INCOME", root, placeholder=True)
    expenses = Account("Expenses", "EXPENSE", root, placeholder=True)
    investments = Account("Investments", "ASSET", assets, placeholder=True)
    bank = Account("Bank", "BANK", assets, placeholder=True)
    brokerage = Account("Brokerage", "STOCK", investments,
                        commodity=("NASDAQ", "ACME"), fraction=SHARE_SCALE)
    retirement = Account("Retirement", "STOCK", investments,
                         commodity=("NASDAQ", "ACME"), fraction=SHARE_SCALE)
    checking = Account("Checking Account", "BANK", bank)
    gains = Account("Capital Gains", "INCOME", income)
    commissions = Account("Commissions", "EXPENSE", expenses)
    accounts = [root, assets, liabilities, income, expenses, investments,
                bank, brokerage, checking, gains, commissions]
    if two_accounts:
        accounts.append(retirement)

    # ----------------------------------------------------------------- lots
    lot_a = Lot(brokerage, "ACME bought 2025-03-10")
    lot_b = Lot(brokerage, "ACME bought 2025-08-15")
    lot_c = Lot(brokerage, "ACME bought 2026-02-20")
    lot_d = Lot(brokerage, "ACME bought 2026-01-05")
    lot_e = Lot(retirement, "ACME bought 2025-05-05")

    def buy(number, posted, lot, count, price_cents, account=None):
        account = account or brokerage
        cost = int(round(count * price_cents))
        return Transaction(
            number, posted, f"Buy {count:g} ACME @ {price_cents / 100:.2f}",
            [Split(account, cost, shares(count), lot=lot,
                   memo=f"{count:g} ACME", action="Buy"),
             Split(checking, -cost, usd(-cost),
                   memo=f"Buy {count:g} ACME", action="Buy")],
            action="Buy")

    def sell(number, posted, lot, count, price_cents, account=None):
        account = account or brokerage
        proceeds = int(round(count * price_cents))
        return Transaction(
            number, posted, f"Sell {count:g} ACME @ {price_cents / 100:.2f}",
            [Split(account, -proceeds, shares(-count), lot=lot,
                   memo=f"{count:g} ACME", action="Sell"),
             Split(checking, proceeds, usd(proceeds),
                   memo=f"Sell {count:g} ACME", action="Sell")],
            action="Sell")

    def realised_gain(number, posted, lot, gain_cents, account=None):
        """The capital-gains transaction: a zero-share split in the stock
        account (in the lot, so the report's gain check sees it) and the
        offsetting income split."""
        account = account or brokerage
        return Transaction(
            number, posted, "Realized Gain/Loss",
            [Split(account, gain_cents, shares(0), lot=lot,
                   memo="Realized Gain/Loss", action="Gain"),
             Split(gains, -gain_cents, usd(-gain_cents),
                   memo="Realized Gain/Loss", action="Gain")],
            action="Gain")

    transactions = [
        # Lot A: 100 shares at $10.00, before the report's default start.
        buy("BUY-1", "2025-03-10", lot_a, 100, 1000),
        # Lot B: 50 shares at $12.00, also before the default start.
        buy("BUY-2", "2025-08-15", lot_b, 50, 1200),
        # Lot D: 20 shares at $9.00, inside 2026.
        buy("BUY-3", "2026-01-05", lot_d, 20, 900),
        # Lot C: 40 shares at $15.00, inside 2026.
        buy("BUY-4", "2026-02-20", lot_c, 40, 1500),
        # Lot A first sale: 30 shares at $20.00 -> basis 300, gain 300.
        sell("SELL-1", "2026-03-15", lot_a, 30, 2000),
        realised_gain("GAIN-1", "2026-03-15", lot_a, 30000),
        # Lot C sale: 10 of 40 shares at $16.00 -> basis 150, gain 10.
        sell("SELL-2", "2026-04-01", lot_c, 10, 1600),
        realised_gain("GAIN-2", "2026-04-01", lot_c, 1000),
        # Lot A second sale: 20 of the remaining 70 shares at $18.00 ->
        # basis 200, gain 160.
        sell("SELL-3", "2026-05-10", lot_a, 20, 1800),
        realised_gain("GAIN-3", "2026-05-10", lot_a, 16000),
        # Lot D closed out: 20 shares at $21.00 -> basis 180, gain 240.
        sell("SELL-4", "2026-06-01", lot_d, 20, 2100),
        realised_gain("GAIN-4", "2026-06-01", lot_d, 24000),
    ]

    if two_accounts:
        # The second account's lot: bought before the default start date,
        # partly sold so it has both realised and unrealised numbers.
        transactions.extend([
            buy("BUY-5", "2025-05-05", lot_e, 10, 1100, account=retirement),
            sell("SELL-5", "2026-04-15", lot_e, 5, 2000, account=retirement),
            realised_gain("GAIN-5", "2026-04-15", lot_e, 4500,
                          account=retirement),
        ])

    # ---------------------------------------------------------------- prices
    # (date, USD cents per share).  The default report date is "today", so
    # the newest price is the one the default options use; the earlier ones
    # let a run with end=2026-02-28 show a different end value (or none).
    prices = [
        ("2025-06-30", 1400),
        ("2026-01-02", 1800),
        ("2026-03-31", 1900),
        ("2026-06-29", 2200),
        ("2026-09-30", 2500),
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
    w('<gnc:count-data cd:type="commodity">2</gnc:count-data>')
    w('<gnc:count-data cd:type="account">%d</gnc:count-data>' % len(accounts))
    w('<gnc:count-data cd:type="transaction">%d</gnc:count-data>'
      % len(transactions))
    w('<gnc:count-data cd:type="price">%d</gnc:count-data>' % len(prices))

    for space, mnemonic, name, fraction in (
            ("CURRENCY", "USD", "US Dollar", USD_SCALE),
            ("NASDAQ", "ACME", "Acme Corporation", SHARE_SCALE)):
        w('<gnc:commodity version="2.0.0">')
        w(f'  <cmdty:space>{space}</cmdty:space>')
        w(f'  <cmdty:id>{mnemonic}</cmdty:id>')
        w(f'  <cmdty:name>{name}</cmdty:name>')
        w(f'  <cmdty:xcode>{mnemonic}</cmdty:xcode>')
        w(f'  <cmdty:fraction>{fraction}</cmdty:fraction>')
        w('</gnc:commodity>')

    w('<gnc:pricedb version="1">')
    for day, cents in prices:
        w('  <price>')
        w(f'    <price:id type="guid">{guid()}</price:id>')
        w('    <price:commodity>')
        w('      <cmdty:space>NASDAQ</cmdty:space>')
        w('      <cmdty:id>ACME</cmdty:id>')
        w('    </price:commodity>')
        w('    <price:currency>')
        w('      <cmdty:space>CURRENCY</cmdty:space>')
        w('      <cmdty:id>USD</cmdty:id>')
        w('    </price:currency>')
        w('    <price:time>')
        w(f'      <ts:date>{ts(day, "00:00:00")}</ts:date>')
        w('    </price:time>')
        w('    <price:source>user:price-editor</price:source>')
        w('    <price:type>last</price:type>')
        w(f'    <price:value>{usd(cents)}</price:value>')
        w('  </price>')
    w('</gnc:pricedb>')

    def lot_tree(lot):
        w('    <gnc:lot version="2.0.0">')
        w(f'      <lot:id type="guid">{lot.guid}</lot:id>')
        w('      <lot:slots>')
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
        w(f'    <cmdty:space>{account.commodity[0]}</cmdty:space>')
        w(f'    <cmdty:id>{account.commodity[1]}</cmdty:id>')
        w('  </act:commodity>')
        w(f'  <act:commodity-scu>{account.fraction}</act:commodity-scu>')
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
        w('  <trn:splits>')
        for split in transaction.splits:
            w('    <trn:split>')
            w(f'      <split:id type="guid">{split.guid}</split:id>')
            if split.memo:
                w(f'      <split:memo>{split.memo}</split:memo>')
            if split.action:
                w(f'      <split:action>{split.action}</split:action>')
            w('      <split:reconciled-state>n</split:reconciled-state>')
            w(f'      <split:value>{usd(split.value_cents)}</split:value>')
            w(f'      <split:quantity>{split.quantity}</split:quantity>')
            w(f'      <split:account type="guid">{split.account.guid}'
              '</split:account>')
            if split.lot is not None:
                w(f'      <split:lot type="guid">{split.lot.guid}</split:lot>')
            w('    </trn:split>')
        w('  </trn:splits>')
        w('</gnc:transaction>')

    w('</gnc:book>')
    w('</gnc-v2>')

    with open(out_path, "w", encoding="utf-8") as handle:
        handle.write("\n".join(out) + "\n")
    print(f"wrote {out_path}: {len(accounts)} accounts, "
          f"{len(transactions)} transactions, "
          f"{len(brokerage.lots) + len(retirement.lots)} lots, "
          f"{len(prices)} prices")


if __name__ == "__main__":
    main(sys.argv)
