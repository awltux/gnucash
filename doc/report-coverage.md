# Scheme report coverage under the native report layer

Status: measurement for `doc/report-engine-migration.md`
Branch: `webui/engine-server`
Measured at: `8b3433c46a` (working tree clean before this edit)
Scope: registry enumeration, classification, coverage and cost. No code, tests
or report behaviour were changed; the only edited file is this one.

> **What changed since the first measurement.**  The first version of this
> document was measured at `a639555722`, when nine Scheme report names rendered
> natively, and its verdict was "the majority is not covered".  At
> `8b3433c46a` the native layer names **19** of the **66** source names (19 of
> the 65 a US build loads; 18 in a German build), and the document has been
> re-derived rather than patched.  The registry itself has not changed: no file
> under `gnucash/report/reports/` changed between
> `a639555722` and `8b3433c46a`.  All the movement is in the native layer.

## Summary

- The Scheme engine registers **66 report names** from
  `gnucash/report/reports/**`: 58 `(gnc:define-report ...)` call sites, of
  which three are loops that register 5, 4 and 2 names, so
  `55 + 5 + 4 + 2 = 66`.  A build loads **65** because the locale loader picks
  one of the two `taxtxf` reports.  The count is unchanged from the earlier
  measurement; the plan's "63 report definitions" is still stale.
- **Native: 19 names in 15 implementations.**  The six original core reports,
  aging and lots, the five budget layouts, the four native documents, and the
  US tax schedule with TXF export.
- **Spec: 5 names** are expressible in the designer as it stands today
  (Account Summary, Income/Expense Chart, Income & Expense Linechart, Net Worth
  Barchart, Net Worth Linechart).  The **4 built-in specs** are extra reports,
  not registry names; they are listed separately below and do not change the
  19/66 numerator.
- **Logic: 21 names**, about eight engines.  **Template: 2 names** (the two
  remaining invoice layouts).  **Closed: 19 names**, deliberately out of scope
  with the reason recorded.
- **By name: 19/66 = 28.8% native.**  Native plus spec-expressible is
  **24/66 = 36.4%**.  Native plus spec plus closed is **43/66 = 65.2%**
  done-or-decided; the genuinely unported remainder is **23/66 = 34.8%**
  (21 logic + 2 template).
- **Accounting core: 6/6 = 100%** on the six names the earlier document called
  the accounting core (Trial Balance, Balance Sheet, Income Statement,
  Profit & Loss, Transaction Report, General Ledger); 6/7 = 86% if the General
  Journal is added, and 6/11 = 55% if the equity, cash-flow and multicolumn
  statements are added too.  The definition is stated with each number.
- **Verdict:** the web tier can be Guile-free for a real user, but not for
  every real user.  A personal-finance user can run the reporting side of the
  bookkeeping loop natively; a business that needs owner statements,
  equity/cash-flow/multicolumn statements, GST, portfolios or the two styled
  invoice layouts cannot.  The unported gap is **weeks (order 2-4 at the
  observed pace), not months**; full parity including the deliberately closed
  chart families and ELSTER would be months, because those are new subsystems.
- **Basis:** counts are derived from sources at `8b3433c46a`.  There is no
  Guile build in this checkout, so the registry is the static one the loader
  would build, and there is **no telemetry in the tree**, so every usage claim
  below is an argument from function, not a measurement.

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
`66` names, none duplicated, and all 19 native names are members of that set.
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
| `GET /api/reports` | 69 | 23 | The report list the server actually offers |
| Native names | 19 | 19 | Names `render_report()` handles without Guile |

In a Guile build, `/api/reports` enumerates the Scheme templates that build
actually loaded (`65`, one locale `taxtxf`) and then appends the four built-in
designer specs (`+4 = 69`).  In a Guile-free build it enumerates
`native_report_names()` (`19`) and appends the same four (`+4 = 23`).  The four
built-ins are not Scheme names and are not counted in the 66.  A Guile-free
build therefore offers **23 report-list entries**, but the by-name coverage of
the Scheme registry is **19/66**, not 23/66.  The task's "designer with four
built-in chart specs" is the `+4`, not four more registry names.

The 65-name build is not identical across locales: a US build loads `Tax
Schedule Report/TXF Export` (native), a German build loads `Tax Report / TXF
Export` (closed).  Both builds have 65 Scheme names; the intersection of the
native names with the loaded Scheme names is 19 in the US build and 18 in the
German one.  The Guile-free list is locale-independent: it is always the 19
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
        "Australian Tax Invoice", "Tax Schedule Report/TXF Export"};
```

That is 19 names in 15 implementations.  Income Statement and Profit & Loss
share one renderer; Budget Income Statement and Budget Profit & Loss share one;
Tax Invoice and Australian Tax Invoice share one; Receivable and Payable Aging
share one.  Counting "reports ported" as 19 overstates the engine work by four.

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
hidden by the 19/66 number.

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

### 3.1 Native (19 names, 15 implementations)

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

### 3.3 Logic (21 names, about eight engines)

| # | Report | Source | Cluster | Why it needs custom native code |
|---:|---|---|---|---|
| 25 | Advanced Portfolio | `standard/advanced-portfolio.scm` | portfolios/lots | Per-account lot valuation: share counts, price sources, cost basis, realised and unrealised gains in several currencies. |
| 26 | Balance Sheet (Multicolumn) | `standard/balsheet-pnl.scm` | cross-tab | Account-tree rows × period columns; the spec has one grouping axis, not a row-and-column cross-tab. |
| 27 | Cash Flow | `standard/cash-flow.scm` | cash flow | Classifies counterpart splits into "money in"/"money out" (`cash-flow-calc.scm`); not a debit/credit balance table. |
| 28 | Cash Flow Barchart | `standard/cashflow-barchart.scm` | cash flow | Reuses the same money-in/out classification; the chart type is supported, the data engine is not. |
| 29 | Customer Report | `standard/new-owner-report.scm` | owner documents | Owner report over `gncOwner` documents (invoices, payments), not splits alone. |
| 30 | Customer Summary | `standard/customer-summary.scm` | owner documents | Walks `gncOwner` customers and their posted documents; owner objects and invoice links. |
| 31 | Employee Report | `standard/new-owner-report.scm` | owner documents | Same owner-report engine with `GNC-OWNER-EMPLOYEE`. |
| 32 | Equity Statement | `standard/equity-statement.scm` | cross-tab | Start/change/end columns, unrealised gains, closing-entry handling — derived balances, not a query sum. |
| 33 | General Journal | `standard/general-journal.scm` | entry listings | Transaction listing (date, num, description, transfer, debit, credit) delegated to the register renderer; the native layer has no register renderer. |
| 34 | IFRS weighted-average cost basis report | `standard/ifrs-cost-basis.scm` | portfolios/lots | Lot-by-lot weighted-average cost accumulation with commission capitalisation and gains. |
| 35 | Income and GST Statement | `standard/income-gst-statement.scm` | tax/GST | Builds trep columns per tax code from the tax table; multi-pass tax aggregation. |
| 36 | Income Statement (Multicolumn) | `standard/balsheet-pnl.scm` | cross-tab | Same account-tree × period cross-tab as the multicolumn balance sheet. |
| 37 | Investment Portfolio | `standard/portfolio.scm` | portfolios/lots | Per-account share/price/value valuation at a date. |
| 38 | Job Report | `standard/new-owner-report.scm` | owner documents | Owner report with `GNC-OWNER-JOB`. |
| 39 | Lot Viewer | `standard/lot-viewer.scm` | portfolios/lots | Browses `gncLot` objects and their split/lot links; a UI over the lot model, not a table of balances. |
| 40 | Reconciliation Report | `standard/reconcile-report.scm` | entry listings | Trep renderer with a reconcile-date predicate instead of the posted-date range. |
| 41 | Transaction Breakdown Report | `standard/txn-columns.scm` | entry listings | One generated column per split/account per transaction; custom cell layout. |
| 42 | Vendor Report | `standard/new-owner-report.scm` | owner documents | Owner report with `GNC-OWNER-VENDOR`. |
| 43 | Average Balance | `example/average-balance.scm` | analytics | Time-series average over a step size, with a chart; not a period sum and not a cumulative balance. |
| 44 | Balance Forecast | `standard/balance-forecast.scm` | scheduled transactions | Projects balances from scheduled transactions and historical averages, then charts them. |
| 45 | Future Scheduled Transactions Summary | `standard/account-summary.scm` | scheduled transactions | Instantiates scheduled transactions into the future; no splits exist yet. |

The clusters matter more than the count: five owner reports share one
`gncOwner`/document engine, four portfolio names share one lot/price valuation
engine, three cross-tab names share a period-column engine, three listings
share custom renderers, and the two cash-flow names share the money-in/out
classification.  This is not 21 independent reports.

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
| native | 19 | 28.8% | 15 implementations; four pairs share a renderer. |
| spec | 5 | 7.6% | 4 built-in specs cover four of them; Account Summary has no built-in. |
| logic | 21 | 31.8% | About eight engines; see section 3.3. |
| template | 2 | 3.0% | Easy and Fancy Invoice layouts. |
| closed | 19 | 28.8% | 13 chart-model (including Budget Chart), 2 demos, 4 other decisions. |
| **total** | **66** | **100%** | 65 loaded per build because of the locale `taxtxf`. |

Native + spec = **24/66 = 36.4%**.  Native + spec + closed = **43/66 =
65.2%** done-or-decided.  The unported remainder is logic + template =
**23/66 = 34.8%**.

Separate from the 66: **4 built-in designer specs** (Income vs Expenses by
Month, Expenses by Account, Income by Account, Net Worth by Month).  They are
new reports, not Scheme names, so they are not in the table or the percentages.
They are the `+4` in the `/api/reports` counts in section 1.1.

## 4. Coverage

### 4.1 By name

The headline is deliberately simple:

- **19 of 66 source names render natively: 19/66 = 28.8%.**  That is the
  number to use for "how much of the Scheme registry does the native layer
  replace".  Because the locale loader swaps the two tax reports, the per-build
  number is **19/65 = 29.2% in a US build** and **18/65 = 27.7% in a German
  build**: the German `Tax Report / TXF Export` replaces the US schedule, and
  the German report is closed.  The 19/66 source count includes both tax names,
  only one of which a build loads.
- **24 of 66 can be produced without new native code: 24/66 = 36.4%.**  Five
  more are spec-expressible.  Four of those five correspond to patterns the
  built-in specs already exercise, but the built-ins are separate names, not
  registrations of the Scheme names; Account Summary has no built-in.
  A user who wants the Scheme name itself still needs it registered.
- **19 of 66 are closed: 19/66 = 28.8%.**  If closed decisions count as
  "covered", 43/66 = 65.2% is done-or-decided.
- **23 of 66 are unported: 23/66 = 34.8%.**  This is the only number that is a
  backlog.

A Guile-free build's `/api/reports` has 23 entries (19 native + 4 built-in
specs).  That is a different numerator from "19/66 Scheme names": the four
built-ins are extra reports, not four more Scheme names.  A Guile build's
`/api/reports` has 69 entries (65 loaded Scheme names + 4 built-in specs).  Do
not quote 23, 69 or 66 as if they were the same count.

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
| Owner reports | 5 | 0 | 0 | 5 | 0 | 0 | **None** (0/5). |
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
| **Total** | **66** | **19** | **5** | **21** | **2** | **19** | |

"Full" means every name in the class renders natively.  "Full/decided" means
the unported name is closed, so there is no backlog in that class.
"Spec/decided" means the chart class has four spec-expressible names and the
rest are closed.  "Partial" means a real backlog remains.  "None" means every
name in the class is unported logic or template.

The pattern is clear: **the core statements, aging and budgets are fully
covered; the primary listings are partly covered; the classes that need an
owner/document model, a cross-tab, a lot/price valuation, or a scheduled-
transaction engine are not.**  Charts are mostly closed rather than ported,
and the two remaining document layouts are the smallest outstanding class.

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
- Investment Lots for a brokerage account;
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
- the customer/vendor/employee/job reports and Customer Summary;
- Equity Statement, Cash Flow and the multicolumn Balance Sheet/Income
  Statement;
- General Journal, Reconciliation Report and Transaction Breakdown Report;
- the Income and GST Statement (relevant where GST applies);
- the portfolio/cost-basis reports (Advanced Portfolio, Investment Portfolio,
  IFRS) and Lot Viewer;
- Easy Invoice and Fancy Invoice layouts;
- the German tax report/ELSTER export.

For a personal user who mostly reads balances and income/expenses, the missing
items are analytics and owner/portfolio reports, not the bookkeeping loop.
That is a defensible Guile-free tier.  It is not full parity: the report list
is 23 entries instead of 69, and a saved Scheme report with non-default options
may not replay.

### 5.2 A business user

A business user is a different answer.  A **US small business** that uses the
native Printable Invoice (and, where applicable, the Receipt or Tax Invoice)
and files the US tax schedule with TXF can be Guile-free **for reporting and
document printing** today: those documents and Tax Schedule Report/TXF Export
are native, and the statements, listings, aging, budgets and lots are native.
The Australian Tax Invoice is native as well, for books in that locale.

A business that needs any of the following **cannot**:

- **Owner documents:** Customer Report, Vendor Report, Employee Report, Job
  Report, Customer Summary.  Aging answers "how old", not "what does this
  customer owe" or "what is this job's position".
- **Statements:** Equity Statement; Cash Flow; Balance Sheet (Multicolumn);
  Income Statement (Multicolumn).  The single-column Balance Sheet and P&L are
  native; the variants are not.
- **Audit/listing:** General Journal, Reconciliation Report, Transaction
  Breakdown Report.
- **Tax:** Income and GST Statement (non-US GST); Tax Report / TXF Export
  (German ELSTER).  The US schedule is native.
- **Investments:** Advanced Portfolio, Investment Portfolio, IFRS
  weighted-average cost basis report, Lot Viewer.  Investment Lots covers the
  lot table, not the portfolio summaries or IFRS cost basis.
- **Documents:** Easy Invoice, Fancy Invoice.  The four native documents cover
  the standard printable invoice, receipt and tax invoice, not those two
  layouts.
- **Container:** Multicolumn View.  Composite dashboards are a recorded loss.

For that business the web tier is not Guile-free for the reports it needs: the
missing names are the reports a finance function uses to close, reconcile,
report equity/cash flow, bill with a custom layout, or file non-US tax.

### 5.3 How far is the gap?

The unported logic/template gap is **weeks, not months**.  The last five report
stages added 13 names (from the six core reports to 19) in about seven hours of
commit-to-commit time (section 6); the 23 remaining names are less than twice
that count, and several of them share engines.  At the observed pace, with a 2-3x
allowance for the harder engines (IFRS, scheduled transactions, owner
documents), the unported work is **order 2-4 weeks**, not the "months in total"
of the earlier document.

**Full parity is months**, but not because of the 23 unported report names.
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
| **Five stages** | | **13** | **6,688** | **3,781** | **~6 h 55** | **~515 source/name; ~32 min/name** |
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
| Charts: "days each, maybe two weeks for the set" | Chart model + 4 built-ins, 918 source, 1 h 19 | **Cheaper for bar/line.**  Pie, scatter and stacked multi-series are still closed and would be new chart families. |

The correction is not that every remaining class is cheap.  It is that the
earlier document priced **report layouts** as if they were **new engines**.  A
layout over an existing model is hours; a new model (owner documents, IFRS cost
basis, scheduled transactions, ELSTER) is days.

### 6.3 What the unported 23 names should cost

This is an extrapolation from the observed lines-per-name and minutes-per-name,
not a measurement of the remaining work.  The ranges widen where there is no
close precedent.

| Remaining cluster | Names | Closest precedent | Projected source lines | Projected commit-time |
|---|---:|---|---:|---:|
| Owner documents (Customer, Vendor, Employee, Job, Customer Summary) | 5 | aging + documents | 2,500-3,500 | 2-3 h |
| Portfolios/cost basis (Advanced, Investment Portfolio, IFRS, Lot Viewer) | 4 | lots | 3,200-4,800 | 4-6 h |
| Cross-tab statements (BS Multicolumn, IS Multicolumn, Equity) | 3 | budgets + documents | 1,500-2,100 | 2-3 h |
| Entry listings (General Journal, Transaction Breakdown, Reconciliation) | 3 | listings + aging | 1,500-2,100 | 2-3 h |
| Cash flow (Cash Flow, Cash Flow Barchart) | 2 | cash flow | 1,200-1,800 | 1-2 h |
| GST (Income and GST Statement) | 1 | tax | 1,000-1,400 | 1-2 h |
| Average Balance | 1 | table/chart | 400-600 | 1 h |
| Scheduled transactions (Future SX Summary, Balance Forecast) | 2 | SX | 1,600-2,400 | 2-3 h |
| Easy/Fancy Invoice layouts | 2 | documents | 1,000-1,400 | 1-2 h |
| **Total** | **23** | | **14,000-20,000** | **16-25 h** |

At the observed 650-1,350 hand-written source lines per commit-hour, the
14k-20k lines are 10-31 h; the per-cluster sums above are narrower (16-25 h).
Doubling the wider range for the engines with no close precedent gives 20-62 h,
which is 1-3 working weeks at 4-6 focused hours a day.  With the unknowns in
the harder engines, the honest range is **2-4 weeks**, not months.  The 19
closed names are not in this table; reopening them is the months-scale work
described in 5.3.

### 6.4 What the cost table is not

- It is not a schedule.  Commit wall clock is not effort; a commit can sit
  behind a review, a build or an unrelated fix.
- It is not option parity.  The native reports deliberately expose a subset;
  matching the Scheme option pages for the 19 native names is additional work
  not counted here.
- It is not a usage forecast.  No telemetry exists; the order of work should be
  set by which reports a real user needs, not by the line count.

## 7. Surprises

- **The registry never moved.**  The stale verdict came from the native
  numerator, not the denominator.  `git diff a639555722..8b3433c46a --
  gnucash/report/reports` is empty; every change is native-layer, web, test or
  doc.  The count is 66 at both ends.
- **19 names, 15 implementations.**  Four pairs share a renderer.  "Nineteen
  native reports" is true by name and overstated by engine count.
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
  a coverage claim.  If closed decisions count, 65% is done-or-decided; if they
  do not, 35% is backlog and 29% is native.  The same registry supports three
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
- **`/api/reports` is 69 in a Guile build and 23 in a Guile-free build.**
  Neither is 66.  The 66 is the source registry; a build loads 65 of those, and
  the 4 built-ins are extra; the 19 native names are a subset of the 66.
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
native: 19 not in registry: none
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

- It does not claim that 19/66 is a majority; it is not.  It does not present
  43/66 as "coverage" without saying that 19 of those are closed decisions.
- It does not claim that native reports are option-complete.  They are not;
  section 2 lists the gap and the 400 behaviour.
- It does not claim that closed names are easy or impossible.  It records the
  reason and leaves them out of the backlog; reopening them is a separate
  decision.
- It does not claim to know which reports users actually run.  The tree has no
  telemetry.  The personal-versus-business split in section 5 is an argument
  from function, not a measurement.
