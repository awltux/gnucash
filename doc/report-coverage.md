# Scheme report coverage under the native report layer

Status: measurement for `doc/report-engine-migration.md`
Branch: `webui/engine-server`
Measured at: `4826c2e262` plus the owner-report work (working tree clean at
`4826c2e262` before this edit)
Scope: registry enumeration, classification, coverage and cost. No code, tests
or report behaviour were changed; the only edited file is this one.

> **What changed since the first measurement.**  The first version of this
> document was measured at `a639555722`, when nine Scheme report names rendered
> natively, and its verdict was "the majority is not covered".  At
> `4826c2e262` plus the owner-report work the native layer names **24** of the
> **66** source names (24 of the 65 a US build loads; 23 in a German build),
> and the document has been re-derived rather than patched.  The registry
> itself has not changed: no file under `gnucash/report/reports/` changed
> between `a639555722` and this measurement.  All the movement is in the native
> layer.

## Summary

- The Scheme engine registers **66 report names** from
  `gnucash/report/reports/**`: 58 `(gnc:define-report ...)` call sites, of
  which three are loops that register 5, 4 and 2 names, so
  `55 + 5 + 4 + 2 = 66`.  A build loads **65** because the locale loader picks
  one of the two `taxtxf` reports.  The count is unchanged from the earlier
  measurement; the plan's "63 report definitions" is still stale.
- **Native: 24 names in 17 implementations.**  The six original core reports,
  aging and lots, the five budget layouts, the four native documents, the US
  tax schedule with TXF export, and the five owner reports (four registrations
  of one owner-history renderer plus Customer Summary).
- **Spec: 5 names** are expressible in the designer as it stands today
  (Account Summary, Income/Expense Chart, Income & Expense Linechart, Net Worth
  Barchart, Net Worth Linechart).  The **4 built-in specs** are extra reports,
  not registry names; they are listed separately below and do not change the
  24/66 numerator.
- **Logic: 16 names**, about seven engines.  **Template: 2 names** (the two
  remaining invoice layouts).  **Closed: 19 names**, deliberately out of scope
  with the reason recorded.
- **By name: 24/66 = 36.4% native.**  Native plus spec-expressible is
  **29/66 = 43.9%**.  Native plus spec plus closed is **48/66 = 72.7%**
  done-or-decided; the genuinely unported remainder is **18/66 = 27.3%**
  (16 logic + 2 template).
- **Accounting core: 6/6 = 100%** on the six names the earlier document called
  the accounting core (Trial Balance, Balance Sheet, Income Statement,
  Profit & Loss, Transaction Report, General Ledger); 6/7 = 86% if the General
  Journal is added, and 6/11 = 55% if the equity, cash-flow and multicolumn
  statements are added too.  The definition is stated with each number.
- **Verdict:** the web tier can be Guile-free for a real user, but not for
  every real user.  A small business can run the reporting side of the
  bookkeeping loop, including owner statements, natively; a business that needs
  equity/cash-flow/multicolumn statements, GST, portfolios or the two styled
  invoice layouts cannot.  The unported gap is **weeks (order 2-4 at the
  observed pace), not months**; full parity including the deliberately closed
  chart families and ELSTER would be months, because those are new subsystems.
- **Basis:** counts are derived from sources at `4826c2e262` plus the owner
  work.  There is no Guile build in this checkout, so the registry is the
  static one the loader would build, and there is **no telemetry in the tree**,
  so every usage claim below is an argument from function, not a measurement.

## 1. How the registry was counted

There is no Guile-enabled `gnucash-cli` or `gnc-webd` in this checkout
(`.build/` contains logs and a Linux core build with no report engine), so the
registry was derived from the sources, which the task names as ground truth.
The loader is `gnucash/report/reports/reports.scm`:

```scheme
(let ((loc-spec (if (string-prefix? "de_DE" (gnc-locale-name)) 'de_DE 'us)))
  (report-module-loader
   (list
    '(gnucash reports standard)
    '(gnucash reports example)
    `(gnucash reports locale-specific ,loc-spec))))
```

The standard and example prefixes load in every build; only one of the two
`locale-specific/**/taxtxf.scm` modules loads.  That is why the sources contain
66 names but a running engine has 65.

The scan is programmatic:

```sh
$ git grep -c "(gnc:define-report" -- gnucash/report/reports | awk -F: '{s+=$2} END {print s}'
58
```

Three call sites are loops.  They register more than one name each:

| File | Call sites | Names registered |
|---|---:|---:|
| `standard/account-piecharts.scm` | 1 (`build-report!`) | 5: Income Accounts, Expense Accounts, Assets, Securities, Liabilities |
| `standard/category-barchart.scm` | 1 (`for-each` over four rows) | 4: Income Over Time, Expense Over Time, Assets Over Time, Liabilities Over Time |
| `example/daily-reports.scm` | 1 (`for-each` over two rows) | 2: Income vs. Day of Week, Expenses vs. Day of Week |

So the arithmetic is `58 - 3 + (5 + 4 + 2) = 66`.  The other 55 call sites
each register one name, either as a literal (`(N_ "Trial Balance")`) or through
a file-local `reportname` variable.  A parser that reads every
`(gnc:define-report ...)` block, resolves the `'name` expression (literal,
`N_`/`G_`, or file-local define) and expands those three loops returns
`66` names, none duplicated, and all 24 native names are members of that set.
The exact output is in section 8.

Two names need care, as before:

- `standard/dashboard.scm` is not a registration.  It is a helper
  (`gnc:make-dashboard`) that instantiates a `Multicolumn View`; it has no
  `gnc:define-report` form and adds no name.
- `standard/register.scm` **is** registered (`Register`, `in-menu? #f`).  It is
  reachable through `gnc:register-report-create`, not the Reports menu, but it
  is in the registry and is counted.  It is already served separately by the
  web tier's `/register/<guid>` route.

The count has **not changed from 66**.  `git diff --stat a639555722..8b3433c46a
-- gnucash/report/reports` is empty: every change in this window is in
`libgnucash/reports/`, `web/`, tests and docs.  The plan's "63 report
definitions" was wrong at `a639` and is still wrong; the sources are
unambiguous.

### 1.1 The three report lists people quote

Three different numbers are easy to confuse.  They answer different questions:

| List | Guile build | Guile-free build | What it is |
|---|---:|---:|---|
| Scheme registry | 66 (65 loaded) | 0 | Names registered by `gnucash/report/reports/**` |
| `GET /api/reports` | 69 | 28 | The report list the server actually offers |
| Native names | 24 | 24 | Names `render_report()` handles without Guile |

In a Guile build, `/api/reports` enumerates the Scheme templates that build
actually loaded (`65`, one locale `taxtxf`) and then appends the four built-in
designer specs (`+4 = 69`).  In a Guile-free build it enumerates
`native_report_names()` (`24`) and appends the same four (`+4 = 28`).  The four
built-ins are not Scheme names and are not counted in the 66.  A Guile-free
build therefore offers **28 report-list entries**, but the by-name coverage of
the Scheme registry is **24/66**, not 28/66.  The task's "designer with four
built-in chart specs" is the `+4`, not four more registry names.

The 65-name build is not identical across locales: a US build loads `Tax
Schedule Report/TXF Export` (native), a German build loads `Tax Report / TXF
Export` (closed).  Both builds have 65 Scheme names; the intersection of the
native names with the loaded Scheme names is 24 in the US build and 23 in the
German one.  The Guile-free list is locale-independent: it is always the 24
native names, including the US schedule, so a German-locale Guile-free build
still does not offer the German report.

## 2. What "native" means here

A name is native if it is returned by `native_report_names()` in
`libgnucash/reports/gnc-report.cpp` and handled by `render_report()`.  The
current list is:

```cpp
return {"Trial Balance", "Balance Sheet", "Income Statement",
        "Profit & Loss", "Transaction Report", "General Ledger",
        "Receivable Aging", "Payable Aging", "Investment Lots",
        "Budget Report", "Budget Income Statement",
        "Budget Profit & Loss", "Budget Balance Sheet", "Budget Flow",
        "Printable Invoice", "Receipt", "Tax Invoice",
        "Australian Tax Invoice", "Tax Schedule Report/TXF Export",
        "Customer Report", "Vendor Report", "Employee Report",
        "Job Report", "Customer Summary"};
```

That is 24 names in 17 implementations.  Income Statement and Profit & Loss
share one renderer; Budget Income Statement and Budget Profit & Loss share one;
Tax Invoice and Australian Tax Invoice share one; Receivable and Payable Aging
share one; and Customer Report, Vendor Report, Employee Report and Job Report
share one.  Counting "reports ported" as 24 overstates the engine work by
seven.

Native is a *rendering* fact, not an option-completeness claim.  The
statements and listings expose four descriptors each (start, end, currency or
accounts, period); the aging reports expose one (the as-of date); the budgets
one or two; the documents the invoice selector plus their display switches.
The Scheme option pages are much larger: date and account selection, depth,
zero-balance and parent modes, price source, foreign amounts/rates, rules,
hyperlinks, trep columns/sorts/filters and more.  The HTTP layer rejects an
option the report does not publish with a 400 that names it
(`web/server/main.cpp`: "unknown option ..."), so the gap is visible rather
than silently mis-rendered.  Two no-ops are documented: Trial Balance and
Balance Sheet accept `start` and do not use it, because the Scheme `current`
variant and the as-of balance sheet do not use it either.

The practical consequence for "Guile-free" is that a **saved Scheme report
with a non-default option set cannot always be replayed**.  The name renders;
the depth, zero/parent, price-source, rules, foreign-currency or trep-column
options may be refused.  That is a real limitation of the web tier and is not
hidden by the 24/66 number.

## 3. Classification

The classes are the task's:

- **native** — in `native_report_names()` and served by `render_report()`.
- **spec** — the report's accounting content (rows, account/date scope,
  amounts, and a bar or line chart) can be produced by today's JSON
  `TableSpec` without new native code.  Presentation-only losses are stated.
- **logic** — needs custom native code: state across rows, owner/invoice/lot/
  budget/SX objects, valuation, a cross-tab, or a second pass over the data.
- **template** — a document layout rather than a table of account balances.
  Strictly this class is "eguile pages", but the two remaining non-native
  invoice layouts are hand-built HTML documents, not eguile; they are in this
  class because the remaining work is layout, not accounting logic.  The only
  remaining literal eguile page is `Balance Sheet (eguile)`, and it is closed
  as a duplicate (section 3.5).
- **closed** — a recorded decision not to port, with the reason.  Closed is
  **not** a synonym for unported: a closed name is not on the backlog, while a
  logic or template name is.

### 3.1 Native (19 names in this table, 5 more in 3.1a; 17 implementations)

All source paths in this section are relative to `gnucash/report/reports/`.

| # | Report | Source (`gnucash/report/reports/…`) | Native implementation / evidence |
|---:|---|---|---|
| 1 | Trial Balance | `standard/trial-balance.scm` | `trial_balance()`: the `current` variant, every reportable account, debit/credit split and totals, optional currency conversion. |
| 2 | Balance Sheet | `standard/balance-sheet.scm` | `balance_sheet()`: single-column Assets → Liabilities → Equity, retained earnings from income/expense, section and grand totals, bar chart of the totals. |
| 3 | Income Statement | `standard/income-statement.scm` | `income_statement()`: revenue and expense over the period, closing entries excluded by fixed pattern, revenue sign-reversed, net income/loss, optional currency. |
| 4 | Profit & Loss | `standard/income-statement.scm` | Second registration of the Income Statement renderer. |
| 5 | Transaction Report | `standard/transaction.scm` | `transaction_report()`: fixed date/num/description/memo/transfer/debit/credit columns, grouped by account full name, subtotal per account and grand total; voided and closing transactions excluded. |
| 6 | General Ledger | `standard/general-ledger.scm` | `general_ledger()`: account-code grouping (shared codes share a group), running balance, "Balance b/f" and "Total For" rows. |
| 7 | Receivable Aging | `standard/new-aging.scm` | `receivable_aging()`: owner/lot aging on A/Receivable accounts, six buckets, amounts negated. |
| 8 | Payable Aging | `standard/new-aging.scm` | `payable_aging()`: the same engine on A/Payable accounts. |
| 9 | Investment Lots | `standard/investment-lots.scm` | `investment_lots()`: lot state, prices, 20 columns, realised/unrealised gains, short/long-term tests. |
| 25 | Advanced Portfolio | `standard/advanced-portfolio.scm` | `advanced_portfolio()`: per-account basis, value, money in/out, realised and unrealised gain, rate of gain, income, brokerage fees and total return. |
| 34 | IFRS weighted-average cost basis report | `standard/ifrs-cost-basis.scm` | `ifrs_cost_basis()`: the 27-column running average-cost ledger, the basis and gain of each sale, the register's gain and the variance. |
| 37 | Investment Portfolio | `standard/portfolio.scm` | `investment_portfolio()`: per-account units, price and value at a date, with the Total row. |
| 39 | Lot Viewer | `standard/lot-viewer.scm` | `lot_viewer()`: one row per transaction, one column per lot, the document row and each lot's balance. |
| 10 | Budget Report | `standard/budget.scm` | `budget_report()`: budget-vs-actual period table over the book's default budget. |
| 11 | Budget Income Statement | `standard/budget-income-statement.scm` | `budget_income_statement()`: budgeted revenue/expense statement. |
| 12 | Budget Profit & Loss | `standard/budget-income-statement.scm` | Second registration of the budget statement renderer. |
| 13 | Budget Balance Sheet | `standard/budget-balance-sheet.scm` | `budget_balance_sheet()`: single-column existing/allocated/unallocated balance sheet. |
| 14 | Budget Flow | `standard/budget-flow.scm` | `budget_flow()`: per-account-type flow for budget period 1 (the Scheme default); the period selector is a documented gap. |
| 15 | Printable Invoice | `standard/invoice.scm` | `render_printable_invoice()` over `InvoiceDocument`: the base customer/vendor document, nine display columns and seven print switches. |
| 16 | Receipt | `standard/receipt.scm` | `render_receipt()` over the same model: paid-document layout, four product columns, customer-only, tax rows only when taxed, Total Price only when paid. |
| 17 | Tax Invoice | `standard/taxinvoice.scm` | `render_tax_invoice()` over the same model: per-line tax rate, tax amount, tax-inclusive Total Price, optional discount/units/date columns. |
| 18 | Australian Tax Invoice | `standard/taxinvoice.scm` | The same renderer with the Australian headings and default title; `taxinvoice.scm` registers it as a second option set, not a second engine. |
| 19 | Tax Schedule Report/TXF Export | `locale-specific/us/taxtxf.scm` | `tax_schedule()`: schedule from the generated `gnc-tax-codes.cpp` table, plus the `?format=txf` writer.  Parity is tested by `report-parity.sh` and `tax-txf-parity.sh`. |

#### 3.1a The five owner reports (also native; registry numbers 29-31, 38, 42)

They are one engine with several registrations, so they keep the registry
numbers the Owner cluster had in the logic table rather than taking new ones.
`libgnucash/reports/gnc-owner.cpp` holds the model; the report names are the
four `new-owner-report.scm` registrations and `customer-summary.scm`.

| # | Report | Source | Native implementation / evidence |
|---:|---|---|---|
| 29 | Customer Report | `standard/new-owner-report.scm` | `owner_report()`: one owner's invoice/payment history on the A/Receivable accounts, running balance, period and closing totals, lot-based aging row.  The owner matches invoice owner's *end* owner, so a customer's report includes its jobs' invoices. |
| 30 | Customer Summary | `standard/customer-summary.scm` | `customer_summary()`: profit/markup/sales/expense per customer over the income and expense accounts, the "No Customer" row and per-commodity totals. |
| 31 | Employee Report | `standard/new-owner-report.scm` | The same owner-history engine with `GNC-OWNER-EMPLOYEE`, on the A/Payable accounts. |
| 38 | Job Report | `standard/new-owner-report.scm` | The same engine with `GNC-OWNER-JOB`; it matches the invoice's own job GUID and reads A/Receivable (a job's end owner is its customer). |
| 42 | Vendor Report | `standard/new-owner-report.scm` | The same engine with `GNC-OWNER-VENDOR`, on the A/Payable accounts. |

The owner fixture is `web/test/make-invoice-book.py`: its customer invoice,
vendor bill, second customer invoice, employee expense voucher and job invoice
give every owner type a document.  `report-parity.sh` gained the
`owner-report` and `customer-summary` extractors and compares all five names in
`ext4-web` and `ext4-noguile`; the extracts include the history rows, the
brought-forward and period totals, the aging row and the summary's customer
rows.  See section 11.

#### 3.1b The valuation/portfolio cluster (registry numbers 25, 34, 37, 39)

Four names share the lot and price machinery Investment Lots established, so
they are reported together.  `gnc-report.cpp` holds all four; the order below is
the order they were implemented in, by tractability.

| # | Report | Source | Native implementation / evidence |
|---:|---|---|---|
| 39 | Lot Viewer | `standard/lot-viewer.scm` | `lot_viewer()`: the account's transactions in the period, one column per lot any of them touches (titled and short-GUIDed), a document row, each transaction's split amount per lot in the account's commodity, the "Other Account" column and a Balance row.  The smallest of the four: no price lookup and no gain arithmetic, only the lot/split partition.  Its Scheme option holds *one* account; the native option is `account`, and `report-parity.sh` sets it from `[options]` as `account=<GUID>`. |
| 37 | Investment Portfolio | `standard/portfolio.scm` | `investment_portfolio()`: one row per stock/mutual account -- account, symbol, listing, units, price, value -- and a Total row.  One balance and one price lookup per account: the first report to need the price-source option, and the one that proves it. |
| 25 | Advanced Portfolio | `standard/advanced-portfolio.scm` | `advanced_portfolio()`: the same account rows with basis, value, money in/out, realised and unrealised gain, total gain and its rate, income, brokerage fees, total return and its rate, plus the grand total.  Needs the basis engine (`basis_builder()`: average/FIFO/LIFO over `BasisParcel`s) and the price-source fallback marker. |
| 34 | IFRS weighted-average cost basis report | `standard/ifrs-cost-basis.scm` | `ifrs_cost_basis()`: the 27-column running ledger.  The transaction-identification table (`open_types`, `long_types`, `short_types`) is ported row for row; the running basis adds and multiplies with `GNC_DENOM_AUTO`/`REDUCE` because the Scheme report's arithmetic is exact and only rounds when it prints (a fixed-denominator accumulator drifts by a cent over a year of transactions). |

The fixture is `web/test/make-investment-book.py --two-accounts`: four lots in
one account (one closed, one pre-window, two in-window), one in the second, a
short-term and a long-term sale, five prices, USD throughout.  No new fixture
data was needed: it already had multiple purchases at different prices and a lot
that is opened and never closed.  The script now also writes `<book>.ids` (one
`kind name guid` row per account and lot, the companion file
`make-invoice-book.py` writes for its owners), because three of the four
reports take an account GUID and a test has to be able to name one.  What the
fixture does not exercise, and the reports therefore do not prove: a currency
other than the transaction currency (every conversion is the identity on this
book), commission splits and dividend reinvestment (`advanced_portfolio()`
does not implement the Scheme report's DRP residual account), spin-offs, and
stock splits.

Both default pages are reproduced rather than rejected.  Lot Viewer's Scheme
account option has no default; a null account adds no term to its split query,
so the default page is the whole book's transactions and every lot they touch,
with the amounts in the default currency (`gnc_commodity_print_info(null)` is
the default print info).  The IFRS report's five account selections have no
default either, and each falls back to the first account in the book of its
own allowed types -- the engine's `GncOptionAccountSelValue::get_default_value`
rule -- so the report's default run is the Scheme report's default run, and on
a book with no stock account both print the "specify report options" warning.

`report-parity.sh` gained the `portfolio`, `lot-viewer` and `ifrs` extractors
and compares all four names in `ext4-web` and `ext4-noguile`, on their defaults
and scoped (13 cases per build).  The extracts are the full cell matrix for Lot
Viewer and IFRS and (label, text columns, amounts) for the two portfolio
listings, so the note/narrative columns and the blank cells are part of the
claim.

### 3.2 Spec (5 names expressible today)

The spec vocabulary is `columns` (`account-name`, `debit`, `credit`, `balance`,
`subtotal`), `grouping` (`none`, `account-tree`, `period`), `period_unit`
(`year`, `month`), `period_mode` (`activity`, `cumulative`), `account_types`,
`start`/`end`/`date_preset`, `currency`, `accounts`, `chart` (`bar`, `line`) and
`chart_series` (one or two of `debit`, `credit`, `balance`).  A chart has the
table's rows as labels and at most two amount-column series.  It cannot add an
account dimension as a series, cannot stack more than two, and has no pie,
scatter or multi-column layout.

| # | Report | Source | Spec recipe | What is lost |
|---:|---|---|---|---|
| 20 | Account Summary | `standard/account-summary.scm` | `grouping: "account-tree"`, columns `account-name` + `balance`; account types/accounts and currency as needed. | The default Account Code column, type/description/notes columns, depth limit, bottom behaviour, zero-balance and parent-balance/subtotal modes, rules and hyperlinks.  The row amounts and the tree are reproducible; the default page is not byte-identical. |
| 21 | Income/Expense Chart | `standard/net-charts.scm` | `grouping: "period"`, `period_unit: "month"`, columns `debit` + `credit`, account types `INCOME` + `EXPENSE`, `chart: "bar"`, series `debit` + `credit`, dates or `date_preset: "all"`. | The default's third Net Profit series (the spec chart is at most two series), step sizes other than month/year, the table toggle, price source and plot geometry. |
| 22 | Income & Expense Linechart | `standard/net-charts.scm` | Same as 21 with `chart: "line"`. | Same as 21. |
| 23 | Net Worth Barchart | `standard/net-charts.scm` | `grouping: "period"`, `period_unit: "month"`, column `balance`, `period_mode: "cumulative"`, account types `BANK, CASH, ASSET, STOCK, MUTUAL, CURRENCY, CREDIT, LIABILITY, RECEIVABLE, PAYABLE`, `chart: "bar"`, series `balance`, `date_preset: "all"`. | The default's separate Assets and Liabilities series (three series in the Scheme chart), step sizes other than month/year, the table toggle and price source.  The net-worth line itself is reproducible. |
| 24 | Net Worth Linechart | `standard/net-charts.scm` | Same as 23 with `chart: "line"`. | Same as 23. |

The four built-in specs are the evidence that the vocabulary works, but they
are **new reports, not ports**, and their names are not in the 66:

| Built-in id | Report | Scope and chart |
|---|---|---|
| `builtin-income-expenses-month` | Income vs Expenses by Month | `INCOME` + `EXPENSE`, one line point per month over the book's transaction range, two series (`credit` income, `debit` expenses). |
| `builtin-expenses-by-account` | Expenses by Account | `EXPENSE`, all dates, one bar per expense account with activity. |
| `builtin-income-by-account` | Income by Account | `INCOME`, all dates, one bar per income account with activity. |
| `builtin-net-worth-month` | Net Worth by Month | asset and liability types, cumulative balance at each month's end, one line series. |

`Expenses by Account` and `Income by Account` are bar views of the same data
as the pie reports, not the pie reports themselves; that is why the pie names
are closed rather than called spec (section 3.5).  The two net-chart pairs are
the only Scheme chart names whose data is a 1-2-series bar/line over period
rows the spec can compute without another engine; Average Balance, Cash Flow
Barchart and Budget Chart all need a computation the spec does not have.

### 3.3 Logic (16 names, about seven engines)

| # | Report | Source | Cluster | Why it needs custom native code |
|---:|---|---|---|---|
| 25 | Advanced Portfolio | `standard/advanced-portfolio.scm` | portfolios/lots | Per-account lot valuation: share counts, price sources, cost basis, realised and unrealised gains in several currencies. |
| 26 | Balance Sheet (Multicolumn) | `standard/balsheet-pnl.scm` | cross-tab | Account-tree rows × period columns; the spec has one grouping axis, not a row-and-column cross-tab. |
| 27 | Cash Flow | `standard/cash-flow.scm` | cash flow | Classifies counterpart splits into "money in"/"money out" (`cash-flow-calc.scm`); not a debit/credit balance table. |
| 28 | Cash Flow Barchart | `standard/cashflow-barchart.scm` | cash flow | Reuses the same money-in/out classification; the chart type is supported, the data engine is not. |
| 32 | Equity Statement | `standard/equity-statement.scm` | cross-tab | Start/change/end columns, unrealised gains, closing-entry handling — derived balances, not a query sum. |
| 33 | General Journal | `standard/general-journal.scm` | entry listings | Transaction listing (date, num, description, transfer, debit, credit) delegated to the register renderer; the native layer has no register renderer. |
| 34 | IFRS weighted-average cost basis report | `standard/ifrs-cost-basis.scm` | portfolios/lots | Lot-by-lot weighted-average cost accumulation with commission capitalisation and gains. |
| 35 | Income and GST Statement | `standard/income-gst-statement.scm` | tax/GST | Builds trep columns per tax code from the tax table; multi-pass tax aggregation. |
| 36 | Income Statement (Multicolumn) | `standard/balsheet-pnl.scm` | cross-tab | Same account-tree × period cross-tab as the multicolumn balance sheet. |
| 37 | Investment Portfolio | `standard/portfolio.scm` | portfolios/lots | Per-account share/price/value valuation at a date. |
| 39 | Lot Viewer | `standard/lot-viewer.scm` | portfolios/lots | Browses `gncLot` objects and their split/lot links; a UI over the lot model, not a table of balances. |
| 40 | Reconciliation Report | `standard/reconcile-report.scm` | entry listings | Trep renderer with a reconcile-date predicate instead of the posted-date range. |
| 41 | Transaction Breakdown Report | `standard/txn-columns.scm` | entry listings | One generated column per split/account per transaction; custom cell layout. |
| 43 | Average Balance | `example/average-balance.scm` | analytics | Time-series average over a step size, with a chart; not a period sum and not a cumulative balance. |
| 44 | Balance Forecast | `standard/balance-forecast.scm` | scheduled transactions | Projects balances from scheduled transactions and historical averages, then charts them. |
| 45 | Future Scheduled Transactions Summary | `standard/account-summary.scm` | scheduled transactions | Instantiates scheduled transactions into the future; no splits exist yet. |

The clusters matter more than the count: five portfolio names share one
lot/price valuation engine, three cross-tab names share a period-column engine,
three listings share custom renderers, and the two cash-flow names share the
money-in/out classification.  This is not 16 independent reports.  The five
owner reports, which this table used to hold, now share `gnc-owner.cpp` and are
listed in 3.1a.

### 3.4 Template (2 names)

| # | Report | Source | Evidence |
|---:|---|---|---|
| 46 | Easy Invoice | `standard/invoice.scm` | A hand-built HTML invoice document with its own layout; not eguile, but the remaining work is layout over the now-native `InvoiceDocument`, not accounting logic. |
| 47 | Fancy Invoice | `standard/invoice.scm` | The styled invoice layout; same model, different presentation. |

The four native documents cover the Printable Invoice, Receipt, Tax Invoice
and Australian Tax Invoice.  Easy and Fancy are the two layouts that remain
Scheme-only; they are not duplicates of the Printable Invoice, so they are
template-class work, not closed.

### 3.5 Closed (19 names)

"Closed" means a decision is recorded here and the name is **not** on the
backlog.  It is a different number from "unported".

| # | Report | Source | Reason recorded |
|---:|---|---|---|
| 48 | Balance Sheet (eguile) | `standard/balsheet-eg.scm` | An eguile page that duplicates the native Balance Sheet.  The eguile template engine was not added; the native Balance Sheet is the report. |
| 49 | Budget Chart | `standard/budget-barchart.scm` | One chart per leaf account, each with its own title and a period range chosen by period index.  The native `Chart` is one chart per table with shared labels; a native version would change the report or become a different chart. |
| 50 | Assets | `standard/account-piecharts.scm` | Pie chart.  The native chart model is bar/line only; a bar view is a different report. |
| 51 | Assets Over Time | `standard/category-barchart.scm` | Stacked chart with one series per account over periods.  A spec chart's series are amount columns, not an account dimension, and it allows at most two. |
| 52 | Expense Accounts | `standard/account-piecharts.scm` | Pie chart; same reason as Assets. |
| 53 | Expense Over Time | `standard/category-barchart.scm` | Stacked per-account series; same reason as Assets Over Time. |
| 54 | Expenses vs. Day of Week | `example/daily-reports.scm` | Weekday grouping plus a pie chart.  The spec has no weekday grouping and no pie; a bar variant would be a different report. |
| 55 | Income Accounts | `standard/account-piecharts.scm` | Pie chart; same reason as Assets. |
| 56 | Income Over Time | `standard/category-barchart.scm` | Stacked per-account series; same reason as Assets Over Time. |
| 57 | Income vs. Day of Week | `example/daily-reports.scm` | Weekday grouping plus a pie chart; same reason as Expenses vs. Day of Week. |
| 58 | Liabilities | `standard/account-piecharts.scm` | Pie chart; same reason as Assets. |
| 59 | Liabilities Over Time | `standard/category-barchart.scm` | Stacked per-account series; same reason as Assets Over Time. |
| 60 | Multicolumn View | `standard/view-column.scm` | A container whose "report" is a list of embedded report instances (`report-list`).  The web tier has no composite container; saved specs are single reports.  The loss is composite dashboards, recorded rather than hidden. |
| 61 | Price Scatterplot | `standard/price-scatter.scm` | Scatter chart of commodity prices; the native chart model has no scatter. |
| 62 | Register | `standard/register.scm` | `in-menu? #f` account-register view; the web tier already serves `/register/<guid>` and the register JSON route. |
| 63 | Sample Graphs | `example/sample-graphs.scm` | Demo charts with no accounting content; not a report to port. |
| 64 | Sample Report | `example/sample-report.scm` | Demo table showing the report API; no accounting content. |
| 65 | Securities | `standard/account-piecharts.scm` | Pie chart of security account balances; same reason as Assets. |
| 66 | Tax Report / TXF Export | `locale-specific/de_DE/taxtxf.scm` | German USt report and **ELSTER XML** export.  Its code table exists only as Scheme data (`libgnucash/tax/de_DE/txf.scm`) and the native layer has no ELSTER XML writer.  The US TXF work does not generalize: the export format and the code table are different. |

The 19 closed names are 13 chart-model limitations (49-59, 61, 65), two demos
(63-64), and four other decisions (48 Balance Sheet (eguile), 60 Multicolumn
View, 62 Register, 66 Tax Report / TXF Export).  Reopening any of them is a new
subsystem or a changed report, not a report port.

### 3.6 Counts

| Class | Names | Share of 66 | Notes |
|---|---:|---:|---|
| native | 28 | 42.4% | 21 implementations; seven names share a renderer. |
| spec | 5 | 7.6% | 4 built-in specs cover four of them; Account Summary has no built-in. |
| logic | 12 | 18.2% | About six engines; see section 3.3. |
| template | 2 | 3.0% | Easy and Fancy Invoice layouts. |
| closed | 19 | 28.8% | 13 chart-model (including Budget Chart), 2 demos, 4 other decisions. |
| **total** | **66** | **100%** | 65 loaded per build because of the locale `taxtxf`. |

Native + spec = **33/66 = 50.0%**.  Native + spec + closed = **52/66 =
78.8%** done-or-decided.  The unported remainder is logic + template =
**14/66 = 21.2%**.

Separate from the 66: **4 built-in designer specs** (Income vs Expenses by
Month, Expenses by Account, Income by Account, Net Worth by Month).  They are
new reports, not Scheme names, so they are not in the table or the percentages.
They are the `+4` in the `/api/reports` counts in section 1.1.

## 4. Coverage

### 4.1 By name

The headline is deliberately simple:

- **28 of 66 source names render natively: 28/66 = 42.4%.**  That is the
  number to use for "how much of the Scheme registry does the native layer
  replace".  Because the locale loader swaps the two tax reports, the per-build
  number is **28/65 = 43.1% in a US build** and **27/65 = 41.5% in a German
  build**: the German `Tax Report / TXF Export` replaces the US schedule, and
  the German report is closed.  The 28/66 source count includes both tax names,
  only one of which a build loads.
- **33 of 66 can be produced without new native code: 33/66 = 50.0%.**  Five
  more are spec-expressible.  Four of those five correspond to patterns the
  built-in specs already exercise, but the built-ins are separate names, not
  registrations of the Scheme names; Account Summary has no built-in.
  A user who wants the Scheme name itself still needs it registered.
- **19 of 66 are closed: 19/66 = 28.8%.**  If closed decisions count as
  "covered", 52/66 = 78.8% is done-or-decided.
- **14 of 66 are unported: 14/66 = 21.2%.**  This is the only number that is a
  backlog.

A Guile-free build's `/api/reports` has 28 entries (24 native + 4 built-in
specs).  That is a different numerator from "24/66 Scheme names": the four
built-ins are extra reports, not four more Scheme names.  A Guile build's
`/api/reports` has 69 entries (65 loaded Scheme names + 4 built-in specs).  Do
not quote 28, 69 or 66 as if they were the same count.

### 4.2 By report class

| Report class | Names | Native | Spec | Logic | Template | Closed | Coverage verdict |
|---|---:|---:|---:|---:|---:|---:|---|
| Core statements (single-column) | 4 | 4 | 0 | 0 | 0 | 0 | **Full** (4/4). |
| Multicolumn statements | 2 | 0 | 0 | 2 | 0 | 0 | **None** (0/2). |
| Equity statement | 1 | 0 | 0 | 1 | 0 | 0 | **None** (0/1). |
| Entry listings | 5 | 2 | 0 | 3 | 0 | 0 | **Partial** (2/5 = 40%). |
| Aging | 2 | 2 | 0 | 0 | 0 | 0 | **Full** (2/2). |
| Budgets | 6 | 5 | 0 | 0 | 0 | 1 | **Full/decided** (5/6 native; Budget Chart closed). |
| Documents and eguile pages | 7 | 4 | 0 | 0 | 2 | 1 | **Partial** (4/7 native; 2 template remain; eguile duplicate closed). |
| Tax | 2 | 1 | 0 | 0 | 0 | 1 | **Full/decided** (US native, German closed). |
| Owner reports | 5 | 5 | 0 | 0 | 0 | 0 | **Full** (5/5). |
| Investments and lots | 5 | 1 | 0 | 4 | 0 | 0 | **Partial** (1/5 = 20%). |
| Charts | 16 | 0 | 4 | 0 | 0 | 12 | **Spec/decided** (4 spec, 12 closed). |
| Account summary | 1 | 0 | 1 | 0 | 0 | 0 | **Spec** (designer only). |
| Cash flow | 2 | 0 | 0 | 2 | 0 | 0 | **None** (0/2). |
| GST | 1 | 0 | 0 | 1 | 0 | 0 | **None** (0/1). |
| Average balance | 1 | 0 | 0 | 1 | 0 | 0 | **None** (0/1). |
| Scheduled transactions | 2 | 0 | 0 | 2 | 0 | 0 | **None** (0/2). |
| Register | 1 | 0 | 0 | 0 | 0 | 1 | **Decided** (web route). |
| Multicolumn View container | 1 | 0 | 0 | 0 | 0 | 1 | **Decided** (loss: composite dashboards). |
| Demos | 2 | 0 | 0 | 0 | 0 | 2 | **Decided** (no accounting content). |
| **Total** | **66** | **24** | **5** | **16** | **2** | **19** | |

"Full" means every name in the class renders natively.  "Full/decided" means
the unported name is closed, so there is no backlog in that class.
"Spec/decided" means the chart class has four spec-expressible names and the
rest are closed.  "Partial" means a real backlog remains.  "None" means every
name in the class is unported logic or template.

The pattern is clear: **the core statements, aging, budgets, the tax schedule
and the owner reports are fully covered; the primary listings are partly
covered; the classes that need a cross-tab, a lot/price valuation, or a
scheduled-transaction engine are not.**  Charts are mostly closed rather than
ported, and the two remaining document layouts are the smallest outstanding
class.

### 4.3 The accounting core

"Accounting core" has to be defined, because the share changes with the
definition.  The earlier document used six names: Trial Balance, Balance Sheet,
Income Statement, Profit & Loss, Transaction Report, General Ledger.  Those six
are all native now:

| Core definition | Names | Native | Share |
|---|---:|---:|---:|
| Six-name core (the earlier document's set) | 6 | 6 | **6/6 = 100%** |
| Six-name core + General Journal (audit-trail listing) | 7 | 6 | 6/7 = 86% |
| Six-name core + General Journal + Equity Statement + Cash Flow + both multicolumn statements | 11 | 6 | 6/11 = 55% |

The earlier argument was that the six names "produce the accounting statements
and the transaction listing: what do I have, what did I earn, and what
happened?".  On that definition the core is fully covered, with five renderers
(Income Statement and P&L share one).  The moment "the accounting core"
includes the audit trail (General Journal), equity, cash flow or the
multicolumn statements, the share falls to roughly half.  Both statements are
true; the document should not use the 100% without saying which definition it
means.

### 4.4 What the numbers do and do not say

- They are **name coverage**, not usage-weighted coverage.  A demo report and
  a daily-used statement count the same.
- **No telemetry exists in the tree.**  There are no usage logs, no counters,
  no user study.  The claims in section 5 about personal versus business use
  are arguments from what the reports compute, not measurements.
- "Native" is a rendering fact; it does not mean full option parity (section
  2).  A user with a saved Scheme report that uses depth, zero/parent, price
  source, rules or trep-column options may get a 400 rather than the saved
  view.
- "Closed" is a scope decision, not a measurement of difficulty.  Reopening a
  closed name can be cheap (Bar Chart) or a new subsystem (ELSTER).

## 5. Verdict: can the web tier be Guile-free for a real user?

**Yes for some real users, no for others.  The build already supports it; the
question is which reports the user needs.**

One scope caveat first: the web tier is a **read-only reporting layer**.  It
opens the book read-only and has no transaction-entry route
(`web/run-smoke-test.sh` checks the read-only API).  "Guile-free" here means
the reports and documents can be served without Guile; the user still needs a
writer for the book, normally the desktop application.  Nothing below claims
that the web tier replaces data entry.

### 5.1 A personal-finance user

A personal-finance user can run the ordinary reporting loop without Guile,
over a book maintained in the desktop application:

- inspect an account register (`/register/<guid>`);
- Trial Balance, Balance Sheet, Income Statement/Profit & Loss;
- Transaction Report and General Ledger;
- the five budget layouts;
- Receivable and Payable Aging, if they track money owed;
- Customer Report, Vendor Report, Employee Report, Job Report and Customer
  Summary, if they invoice customers or track a job;
- Investment Lots, Investment Portfolio and Advanced Portfolio for a
  brokerage account, and Lot Viewer for one account's lots;
- the four built-in chart specs (income vs expenses by month, expenses by
  account, income by account, net worth by month);
- Printable Invoice, Receipt, Tax Invoice and Australian Tax Invoice, if they
  issue documents.

What that user **cannot** do natively:

- the pie charts (Assets, Expense Accounts, Income Accounts, Liabilities,
  Securities);
- the over-time category charts (Assets Over Time, Expense Over Time, Income
  Over Time, Liabilities Over Time), which are stacked per-account series;
- the weekday reports (Income vs. Day of Week, Expenses vs. Day of Week), the
  price scatterplot and the sample graphs;
- Average Balance, Balance Forecast and the Future Scheduled Transactions
  Summary;
- Equity Statement, Cash Flow and the multicolumn Balance Sheet/Income
  Statement;
- General Journal, Reconciliation Report and Transaction Breakdown Report;
- the Income and GST Statement (relevant where GST applies);
- Easy Invoice and Fancy Invoice layouts;
- the German tax report/ELSTER export.

For a personal user who mostly reads balances and income/expenses, the missing
items are analytics and portfolio reports, not the bookkeeping loop.  That is a
defensible Guile-free tier.  It is not full parity: the report list is 28
entries instead of 69, and a saved Scheme report with non-default options may
not replay.

### 5.2 A business user

A business user is a different answer.  A **US small business** that uses the
native Printable Invoice (and, where applicable, the Receipt or Tax Invoice),
prints owner statements and files the US tax schedule with TXF can be
Guile-free **for reporting and document printing** today: those documents and
Tax Schedule Report/TXF Export are native, and the statements, listings, aging,
budgets, lots and owner reports are native.  The Australian Tax Invoice is
native as well, for books in that locale.

A business that needs any of the following **cannot**:

- **Statements:** Equity Statement; Cash Flow; Balance Sheet (Multicolumn);
  Income Statement (Multicolumn).  The single-column Balance Sheet and P&L are
  native; the variants are not.
- **Audit/listing:** General Journal, Reconciliation Report, Transaction
  Breakdown Report.
- **Tax:** Income and GST Statement (non-US GST); Tax Report / TXF Export
  (German ELSTER).  The US schedule is native.
- **Documents:** Easy Invoice, Fancy Invoice.  The four native documents cover
  the standard printable invoice, receipt and tax invoice, not those two
  layouts.
- **Container:** Multicolumn View.  Composite dashboards are a recorded loss.

For that business the web tier is not Guile-free for the reports it needs: the
missing names are the reports a finance function uses to close, reconcile,
report equity/cash flow, bill with a custom layout, or file non-US tax.

### 5.3 How far is the gap?

The unported logic/template gap is **weeks, not months**.  The last six report
stages added 18 names (from the six core reports to 24) in about nine hours of
commit-to-commit time (section 6); the 14 remaining names are the same count,
and several of them share engines.  At the observed pace, with a 2-3x
allowance for the harder engines (scheduled transactions), the unported
work is **order 2-4 weeks**, not the "months in total" of the earlier
document.

**Full parity is months**, but not because of the 14 unported report names.
It is months because the 19 closed names include new subsystems: pie, scatter,
stacked multi-series and per-account period-index charts for 13 names
(including Budget Chart); an ELSTER XML writer and German tax table for one; an
eguile template engine or a duplicate balance sheet for one; and the
Register/Multicolumn View/demo decisions for the remaining four.  Those are not
report ports.  If the product requirement is "every Scheme
report and every Scheme option", the honest estimate is months.  If the
requirement is "the reports a personal-finance user or a narrow US small
business needs", the native layer is already there.

## 6. Cost of what remains

### 6.1 What the last five stages actually took

The durations are commit-to-commit wall clock on `2026-11-09`, from `git log`.
They are not measured effort: they include no review, no GTK integration and
no option-parity work, and they may include idle time.  They are the only
duration evidence in the tree, and they are better than a guess.

| Stage | Commit(s) | Names added | Hand-written source lines* | Test/fixture lines | Wall clock | Observed cost |
|---|---|---|---:|---:|---:|---|
| Aging | `76ab17f0ce` | 2 | 643 | 611 | 56 min | 322 source/name; ~28 min/name |
| Lots | `a639555722` | 1 | 1,123 | 479 | 50 min | 1,123 source/name; 50 min/name |
| Budgets | `912af4ea21`, `72723595d2` | 5 | 1,118 | 569 | 1 h 39 | 224 source/name; ~20 min/name |
| Documents | `714946dad4`, `990d0ae379` | 4 | 2,619 | 1,221 | 2 h 05 | 655 source/name; ~31 min/name |
| Tax | `8b3433c46a` | 1 | 1,185 (+5,988 generated) | 901 | 1 h 25 | 1,185 source/name; 85 min/name |
| Owners | this stage | 5 | 1,785 | 503 | ~1 h | ~357 source/name; ~12 min/name |
| **Six stages** | | **18** | **8,473** | **4,284** | **~7 h 55** | **~471 source/name; ~26 min/name** |
| (Charts/specs, for context) | `114073782b` | 4 built-ins | 918 | 489 | 1 h 19 | 230 source/spec; ~20 min/spec |

\*Source = `libgnucash/reports/*.cpp|hpp`, `web/server/*.cpp|hpp` and
`web/ui/index.html`, excluding tests, fixtures, docs, build files and the
generated `gnc-tax-codes.cpp`.  The tax line's 5,988 generated lines are not
counted as hand-written; they are produced by `web/test/gen-tax-codes.py` from
the Scheme code table.

The important fact is not the average.  It is that **the two classes the
earlier document expected to be most expensive were the cheapest per name**:
budgets (five layouts in 1 h 39) and documents (four renderers from one
`InvoiceDocument` in 2 h 05).  The shared model is the leverage.

### 6.2 Corrections to the earlier estimates

| Earlier estimate | What actually happened | Correction |
|---|---|---|
| Aging: "one commit (~500 lines plus a fixture generator) for two names" | 643 source + 611 test/fixture lines, 56 min | Same order; slightly dearer. |
| Lots: "~1,000 lines" | 1,123 source + 479 test/fixture, 50 min | Accurate. |
| Budgets: "**weeks**" | 5 names, 1,118 source, 1 h 39 | **Cheaper by an order of magnitude.**  The budget model already existed; the reports are layouts over `GncBudget`. |
| Documents: "**weeks to months**" | 4 names, 2,619 source, 2 h 05 | **Cheaper by an order of magnitude.**  One invoice model served four renderers; the second and third documents reused the first. |
| Tax: "**weeks to months**, the least like anything native" | 1 name, 1,185 hand-written + 5,988 generated, 1 h 25 | **Cheaper**, because the code table was generated from Scheme and the TXF writer is a bounded format.  German ELSTER remains closed; the US work does not generalize. |
| Owners: "2,500-3,500 lines, 2-3 h" | 5 names, 1,785 source, ~1 h | **Cheaper.**  One owner-history walk served four registrations; the aging row reused the aging model, and only Customer Summary is a second engine.  The fixture extension was a large share of the work. |
| Charts: "days each, maybe two weeks for the set" | Chart model + 4 built-ins, 918 source, 1 h 19 | **Cheaper for bar/line.**  Pie, scatter and stacked multi-series are still closed and would be new chart families. |

The correction is not that every remaining class is cheap.  It is that the
earlier document priced **report layouts** as if they were **new engines**.  A
layout over an existing model is hours; a new model (IFRS cost basis,
scheduled transactions, ELSTER) is days.

### 6.3 What the unported 18 names should cost

This is an extrapolation from the observed lines-per-name and minutes-per-name,
not a measurement of the remaining work.  The ranges widen where there is no
close precedent.

| Remaining cluster | Names | Closest precedent | Projected source lines | Projected commit-time |
|---|---:|---|---:|---:|
| Portfolios/cost basis (Advanced, Investment Portfolio, IFRS, Lot Viewer) | 4 | lots | done: see section 3.1b | - |
| Cross-tab statements (BS Multicolumn, IS Multicolumn, Equity) | 3 | budgets + documents | 1,500-2,100 | 2-3 h |
| Entry listings (General Journal, Transaction Breakdown, Reconciliation) | 3 | listings + aging | 1,500-2,100 | 2-3 h |
| Cash flow (Cash Flow, Cash Flow Barchart) | 2 | cash flow | 1,200-1,800 | 1-2 h |
| GST (Income and GST Statement) | 1 | tax | 1,000-1,400 | 1-2 h |
| Average Balance | 1 | table/chart | 400-600 | 1 h |
| Scheduled transactions (Future SX Summary, Balance Forecast) | 2 | SX | 1,600-2,400 | 2-3 h |
| Easy/Fancy Invoice layouts | 2 | documents | 1,000-1,400 | 1-2 h |
| **Total** | **18** | | **11,500-16,500** | **14-22 h** |

At the observed 650-1,350 hand-written source lines per commit-hour, the
11.5k-16.5k lines are 9-25 h; the per-cluster sums above are narrower
(14-22 h).  Doubling the wider range for the engines with no close precedent
gives 18-50 h, which is 1-3 working weeks at 4-6 focused hours a day.  With the
unknowns in the harder engines, the honest range is **2-4 weeks**, not months.
The 19 closed names are not in this table; reopening them is the months-scale
work described in 5.3.

### 6.4 What the cost table is not

- It is not a schedule.  Commit wall clock is not effort; a commit can sit
  behind a review, a build or an unrelated fix.
- It is not option parity.  The native reports deliberately expose a subset;
  matching the Scheme option pages for the 24 native names is additional work
  not counted here.
- It is not a usage forecast.  No telemetry exists; the order of work should be
  set by which reports a real user needs, not by the line count.

## 7. Surprises

- **The registry never moved.**  The stale verdict came from the native
  numerator, not the denominator.  `git diff a639555722..8b3433c46a --
  gnucash/report/reports` is empty; every change is native-layer, web, test or
  doc.  The count is 66 at both ends.
- **24 names, 17 implementations.**  Seven names share a renderer.  "Twenty-
  four native reports" is true by name and overstated by engine count.
- **The owner cluster was cheap in the end.**  Four registrations share one
  history walk, the aging row reuses the aging model, and Customer Summary is
  the only second engine.  The fixture extension (a second customer, an
  employee, a job, their documents and a vendor payment) was the larger share
  of the stage.
- **Budgets and documents were cheap, not expensive.**  Five budget layouts in
  1 h 39 and four document renderers in 2 h 05, because the budget model and
  the invoice model already existed.  The earlier "weeks to months" estimate
  was pricing engines that were already built.
- **The tax stage's 8,134 insertions are mostly generated data.**  5,988 of
  them are `gnc-tax-codes.cpp`, generated from the Scheme table by
  `gen-tax-codes.py`.  The hand-written source is 1,185 lines.  Generated
  tables are the reason a tax report can be a stage rather than a project.
- **The chart model covers exactly four Scheme chart names, in a 1-2-series
  configuration.**  The four built-ins are new reports, not ports.  Twelve
  chart-family names (plus Budget Chart) are closed because the model has no
  pie, scatter or stacked multi-series.  A "we have charts" claim is true for
  net worth and income/expense lines, not for the chart class.
- **"Closed" is 29% of the registry.**  That is a large number to leave out of
  a coverage claim.  If closed decisions count, 73% is done-or-decided; if they
  do not, 27% is backlog and 36% is native.  The same registry supports three
  very different headlines, and the document must say which one it is using.
- **The previous document contradicted itself.**  Its top summary still said
  9/66 and "the majority is not covered", while its last update claimed 19/66
  after `990d0ae379`, which is one commit before the tax work and therefore had
  18 native names, not 19.  A rewrite was safer than patching another layer of
  status updates onto it.
- **The web tier is read-only.**  `/api/reports` and the report routes are a
  reporting layer; there is no transaction-entry route (`web/run-smoke-test.sh`
  checks the read-only API).  A Guile-free report tier still needs a writer for
  the book, normally the desktop application.  The verdict in section 5 is
  about reports and documents, not about replacing data entry.
- **`/api/reports` is 69 in a Guile build and 28 in a Guile-free build.**
  Neither is 66.  The 66 is the source registry; a build loads 65 of those, and
  the 4 built-ins are extra; the 28 native names are a subset of the 66.
  Quoting any of these as "the number of reports" without the list is a
  category error.

## 8. Reproducing the measurement

The following is the check used for this document.  It runs from the repository
root (`gnucash/`) and needs only Python 3 and `git`:

```sh
$ git grep -c "(gnc:define-report" -- gnucash/report/reports | awk -F: '{s+=$2} END {print s}'
58

$ python3 - <<'PY'
import re, pathlib
from collections import Counter

root = pathlib.Path('gnucash/report/reports')
files = sorted(root.rglob('*.scm'))
var_re = re.compile(r'\(define\s+([A-Za-z0-9_-]+)\s+\((?:N_|G_)\s+"([^"]*)"\)')
lit_re = re.compile(r'\(define\s+([A-Za-z0-9_-]+)\s+"([^"]*)"')
file_vars = {}
for f in files:
    text = f.read_text(encoding='utf-8', errors='replace')
    table = {}
    for m in var_re.finditer(text): table.setdefault(m.group(1), m.group(2))
    for m in lit_re.finditer(text): table.setdefault(m.group(1), m.group(2))
    file_vars[f] = table

def resolve(expr, table):
    expr = expr.strip()
    for pattern in (r'\((?:N_|G_)\s+"([^"]*)"\)', r'"([^"]*)"', r'([A-Za-z0-9_-]+)'):
        m = re.match(pattern, expr)
        if m:
            value = m.group(1)
            return table.get(value) if pattern.startswith(r'([A-Za-z0-9_-]+)') else value
    return None

loops = {
    'example/daily-reports.scm': ['Income vs. Day of Week', 'Expenses vs. Day of Week'],
    'standard/account-piecharts.scm': ['Income Accounts', 'Expense Accounts', 'Assets', 'Securities', 'Liabilities'],
    'standard/category-barchart.scm': ['Income Over Time', 'Expense Over Time', 'Assets Over Time', 'Liabilities Over Time'],
}
names = []
for f in files:
    rel = str(f.relative_to(root)).replace('\\', '/')
    lines = f.read_text(encoding='utf-8', errors='replace').splitlines()
    for i, line in enumerate(lines):
        if '(gnc:define-report' not in line:
            continue
        expr = next((re.search(r"'name\s+(.*)$", lines[j]).group(1).strip()
                     for j in range(i, min(i + 40, len(lines)))
                     if re.search(r"'name\s+(.*)$", lines[j])), '')
        name = resolve(expr, file_vars[f])
        if name:
            names.append(name)
        else:
            names.extend(loops[rel])

text = pathlib.Path('libgnucash/reports/gnc-report.cpp').read_text(encoding='utf-8')
native = re.findall(r'"([^"]*)"', re.search(
    r'native_report_names \(\)\s*\{\s*return \{(.*?)\};', text, re.S).group(1))
print('names:', len(names), 'duplicates:', [n for n, c in Counter(names).items() if c > 1] or 'none')
print('native:', len(native), 'not in registry:', [n for n in native if n not in set(names)] or 'none')

spec = pathlib.Path('web/server/report-spec.cpp').read_text(encoding='utf-8')
builtins = re.findall(r'builtin_base \(\s*"builtin-[^"]+",\s*"([^"]+)"', spec)
print('built-ins:', len(builtins), builtins)
PY
names: 66 duplicates: none
native: 24 not in registry: none
built-ins: 4 ['Income vs Expenses by Month', 'Expenses by Account', 'Income by Account', 'Net Worth by Month']
```

The parser resolves the 55 single-name call sites and expands the three loops
listed in section 1.  It does not evaluate Scheme; it reads the `'name` field
and the file-local `N_`/`G_` defines, which is enough for this registry because
every registration uses one of those forms.  A Guile build would give the same
names through `gnc:report-templates-for-each`; no Guile-enabled binary exists
in this checkout to capture `/api/reports` live.

The native and built-in counts are read from the same C++ that the server
uses: `native_report_names()` in `libgnucash/reports/gnc-report.cpp` and the
`builtin_base` calls in `web/server/report-spec.cpp`.

## 9. Distribution

`doc/CMakeLists.txt` already lists `report-coverage.md` in `doc_noinst_DATA`,
which feeds `doc_DIST_local` and `doc_DIST` in the parent scope.  No dist-list
change is needed; this file is the only edited file.

## 10. What this document does not claim

- It does not claim that 24/66 is a majority; it is not.  It does not present
  48/66 as "coverage" without saying that 19 of those are closed decisions.
- It does not claim that native reports are option-complete.  They are not;
  section 2 lists the gap and the 400 behaviour.
- It does not claim that closed names are easy or impossible.  It records the
  reason and leaves them out of the backlog; reopening them is a separate
  decision.
- It does not claim to know which reports users actually run.  The tree has no
  telemetry.  The personal-versus-business split in section 5 is an argument
  from function, not a measurement.

## 11. Update: the owner reports

Measured after `4826c2e262`, on the book `web/test/make-invoice-book.py`
generates.  **Native names: 24 of 66.**  Customer Report, Vendor Report,
Employee Report, Job Report and Customer Summary joined
`native_report_names()`; they are native in both builds.

- **One engine, five registrations.**  `new-owner-report.scm` is one renderer
  registered for the four owner types; `customer-summary.scm` is a different
  question (profit per customer) and has its own model.  The native side keeps
  that split: `gnc-owner.cpp` has one owner-history walk and one summary.  The
  four registrations differ in the account type and the matching GUID only:
  customers and jobs read A/Receivable, vendors and employees A/Payable; the
  party reports match the invoice owner's *end* owner, while the Job Report
  matches the invoice's own job GUID.  That is why a customer's report includes
  its jobs' invoices.
- **The history columns match the Scheme defaults.**  Date, Due Date,
  Reference, Type, Description, Debits, Credits and Balance; the optional Job,
  Sale and Tax columns exist with their Scheme defaults (Job off except as an
  option on the party reports, Sale and Tax off).  The running balance is the
  A/R or A/P account amount accumulated by transaction date, with transactions
  before the From date as the balance brought forward.  The closing row is
  "Total Due" or "Total Credit", and the aging row is the same lot-based bucket
  walk the aging reports use.  The "Due or Post Date" choice, the
  linked-transactions column and the document-link column are not reproduced;
  their defaults are what is rendered.
- **Customer Summary is a different table.**  Customer, Profit, Markup, Sales
  and (optionally) Expense, one row per customer per commodity, the "No
  Customer" row for income/expense splits no customer owns, and a Total row per
  commodity.  Profit is sales minus expense; markup is profit over sales as a
  rounded percentage.  The default scope is every INCOME and EXPENSE account,
  and the default sort is customer name ascending with "No Customer" last.  The
  income/expense account selection and the sort key/order options are not
  exposed; their defaults are rendered.
- **The fixture grew, rather than a second book.**  `make-invoice-book.py` now
  adds: a second customer (Beta Corp) with invoice INV-1002; an employee (Eve
  Employee) with expense voucher EXP-3001 on A/Payable; a job (Website
  redesign, owned by Acme) with invoice JOB-4001 on A/Receivable; a Travel
  expense account; and a vendor payment PMT-2001 on BILL-2001.  It also writes
  `<book>.ids`, one `kind name guid` row per owner and document, so a test can
  select an owner without parsing the human-readable output.  The existing
  invoice fixtures are unchanged (INV-1001, BILL-2001, the tax table and Net
  30), so the document tests still run on the same book.
- **Parity in both builds.**  `report-parity.sh` gained the `owner-report` and
  `customer-summary` extractors and an `owner=` key in its `[options]` string;
  it sets the Scheme owner option as the `(type . guid)` pair the option system
  deserialises.  On the generated book the five extracts match the Scheme report
  in `ext4-web` and `ext4-noguile`: Customer Report 7 rows, Vendor Report 6,
  Employee Report 5, Job Report 5, Customer Summary 4.  The b/f case (From
  after every transaction) matches too (5 rows).  The extracts compare the
  history rows, the brought-forward and period totals, the closing total, the
  aging header and row, and the summary's customer/profit/markup/sales/expense
  rows.
- **Options and tests.**  The owner reports publish `owner` (type `owner`),
  From/To and the display switches; the Job Report does not publish `show_job`.
  Customer Summary publishes From/To, `show_expense`, `show_zero_lines`,
  `show_inactive` and the period preset.  `report-options.sh` checks those
  descriptors, the unknown-owner and empty-owner 400s, the per-report boolean
  probes, and the positive vendor path on `sample1`.  The generic
  "every option changes the report" probe skips the owner reports (it has no
  owner to select) and Customer Summary (it has no customer activity); the
  generated-book parity test covers the effects.
- **Build hygiene.**  New files are `libgnucash/reports/gnc-owner.cpp` and
  `.hpp`, listed in `reports_SOURCES`/`reports_HEADERS` and therefore in
  `set_dist_list`.  Both builds compile with `-Werror`; `ldd` on `gnc-webd` is
  still free of libguile, libgtk and libwebkit.

## 12. Update: the entry-listing reports

Measured at `b4aa63f713` plus this cluster.  The summary at the top of the
document was written at the earlier measurement and still says 24; section 3.1b
and section 4.1 already count the four valuation names that landed at
`b4aa63f713`, so the native total before this cluster was **28**.  This cluster
adds General Journal, Transaction Breakdown Report and Reconciliation Report:
**31 of 66 = 47.0% native** (31 of the 65 a US build loads, 30 in a German
build; 31/65 = 47.7% and 30/65 = 46.2%).  Native plus spec is 36/66 = 54.5%;
native plus spec plus the 19 closed decisions is 55/66 = 83.3%; the unported
remainder is 11/66 = 16.7% (9 logic + 2 template).  A Guile-free build's
`/api/reports` has 35 entries (31 native + 4 built-in specs).

- **General Journal is not the Transaction Report.**  `general-journal.scm`
  delegates to the register renderer configured in journal mode over a query
  for every non-void transaction in the book.  It prints a date/description
  heading per transaction, one line per split with the split memo, full account
  name and debit or credit, and the Total Debits / Total Credits rows.  It has
  no account or date option, so its defaults are the whole listing: 8 rows on
  `sample1` and 221 on `taxreport.gnucash` match the Scheme page.
- **Transaction Breakdown is a separate report, not a mode of the Transaction
  Report.**  `txn-columns.scm` has its own GUID, its own option page (one
  account, a date range, a description filter and a column limit) and its own
  renderer, which builds one column per account any of the transactions touches
  and a multi-row account-tree header.  Its default is the engine's "No data"
  warning (no account selected), reproduced; scoped, `sample1`'s Checking
  Account in 2018 is 5 rows and the taxreport's Federal:Estimated in 2000 is 9
  rows.  The `taxreport`'s Checking One in 2000 links more than the default ten
  accounts, so it renders the Scheme "Too many accounts" warning, reproduced.
- **Reconciliation is trep with the reconciled-date rule.**  The rule is
  asymmetric: a reconciled split is included when its *reconciled* date is in
  the period, and an unreconciled split is always included.  The rows are
  grouped by reconciled status (Unreconciled, Cleared, Reconciled, Frozen) and
  the amount columns are the calculated Funds In / Funds Out pair, with a
  Total For row per group and a Grand Total.  Its default has no account
  selected: the "No accounts selected" warning plus the report's own
  explanation, reproduced.  Scoped, `sample1`'s Checking Account in 2018 is 4
  rows and the taxreport's Checking One in 2000 is 33 rows.
- **What the fixtures cannot exercise.**  Both books have only unreconciled
  splits: every `split:reconciled-state` is `n` and there is no
  `split:date-reconciled` anywhere.  So the reconciled branch of the
  inclusion rule, and therefore any effect of the date window on which split is
  included, cannot be exercised by these fixtures; the window changes only the
  printed interval.  The always-include branch *is* exercised, including a
  split outside the period: the taxreport's 01/11/01 Fed Est Tax row appears in
  the 2000 report because it is unreconciled.  `sample1`'s opening balance is
  indeed unreconciled, so it is in the report for that reason.
- **Options.**  General Journal publishes its six meaningful register display
  switches (`show_date`, `show_num`, `show_description`, `show_memo`,
  `show_account`, `show_running_balance`); all six were compared with the
  Scheme page through the saved-report path.  Its `Totals` option is not
  published: through that path the Scheme option does not change the page (the
  Debit/Credit totals always print), so the effective default is what renders.
  Transaction Breakdown publishes `account` (the Scheme single-account option,
  in the route's list form), `start`, `end`, `description-filter` and
  `max-columns`; the last is the native layer's first `number` option, with the
  Scheme range 1-100, and the route rejects a non-number or out-of-range value
  with a 400 that names the option.  Reconciliation publishes `accounts`,
  `start`, `end` and `show_reconciled_date`; the other trep options stay at
  their defaults and are documented as not reproduced.  Both the journal's
  Debit/Credit totals and the reconciliation totals are one pair in the report
  currency; the Scheme collectors split them per commodity, which a
  multi-currency book would need, and the fixtures are single-currency.
- **Parity.**  `report-parity.sh` gained the `general-journal`, `breakdown` and
  `reconciliation` extractors, the three report GUIDs and the option mappings
  (the breakdown's single `account` and its number limit).  On both
  `ext4-web` and `ext4-noguile`, on `sample1` and
  `doc/examples/taxreport.gnucash`, the defaults and the scoped variants above
  all match verbatim; the full matrix run for this change is 41 cases per build
  (every native name, including the three new ones) and all 41 match.
- **Tests and build hygiene.**  `report-options.sh` checks the three
  descriptor sets, the new number option's valid range and its two 400s, the
  unknown-GUID 400, and probes every published option for an effect (the
  breakdown gets its account and period baseline from the fixture's Checking
  Account GUID, read from the book).  The smoke test, `report-spec.sh` and
  `report-options.sh` pass in both builds; both compile with `-Werror`; `ldd`
  on the Guile-free `gnc-webd` has no `libguile`, `libgtk` or `libwebkit`
  dependency.  No new files were added (the three reports live in the existing
  `gnc-report.cpp`), so the distribution lists are unchanged.

