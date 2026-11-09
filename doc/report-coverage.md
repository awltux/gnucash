# Scheme report coverage under the native report layer

Status: measurement for `doc/report-engine-migration.md`
Branch: `webui/engine-server`
Measured at: `a639555722`
Scope: registry enumeration and classification only. No code, tests or report
behaviour were changed.

## Summary

- The Scheme engine registers **66 report names** from `gnucash/report/reports/**`
  (65 in any one build, because the loader picks one locale's `taxtxf`).  The
  migration plan's "63 report definitions" is stale.
- The native layer implements **9 of the 66 names** (13.6%), which is **8
  implementations**: `Income Statement` and `Profit & Loss` are two
  registrations over one renderer.
- **One** more name, `Account Summary`, is close to a designer `TableSpec`
  (one row per account, balance column) but its default also prints the
  account code and lays the accounts out as a tree; depth, zero-balance and
  parent-balance modes have no spec field.
- The remaining **56 names** are 17 logic reports, 9 template reports and 30
  that fit none of the other four classes (budgets, charts, scheduled
  transactions, a container, a register view).
- **By name the coverage is 10/66 (15%).**  The nine native names include the
  reports that produce the accounting statements and the transaction listing
  (Trial Balance, Balance Sheet, Income Statement/Profit & Loss, Transaction
  Report, General Ledger), which is the strongest argument that routine report
  *use* is covered; that argument is made in "Is the numeric majority
  covered?", and it is an argument from function, not from measured usage —
  the tree has no telemetry.

## 1. How the registry was counted

There is no Guile build in this checkout (`.build/` holds only logs, no
`gnucash-cli`), so the registry was derived statically from the sources, which
the task names as ground truth.  The scan used `git grep` plus a small parser
for the `'/name` field of every `(gnc:define-report ...)` form.

There are **58 registration call sites**.  Three of them sit inside loops and
register more than one name:

| file | call sites | names registered |
|---|---:|---:|
| `standard/account-piecharts.scm` | 1 | 5 |
| `standard/category-barchart.scm` | 1 | 4 |
| `example/daily-reports.scm` | 1 | 2 |

So the count is `58 - 3 + (5 + 4 + 2) = 66`.

Two names need care:

- `standard/dashboard.scm` is **not** a registration.  It is a helper
  (`gnc:make-dashboard`) that instantiates a `Multicolumn View` and four other
  reports; it has no `gnc:define-report` form and adds no name.
- `standard/register.scm` **is** registered (`Register`, `in-menu? #f`).  It is
  reachable through `gnc:register-report-create` rather than the Reports menu,
  but it is in the registry and is counted.

`reports.scm` loads the `standard`, `example` and one `locale-specific`
prefix.  The US and German `taxtxf` files each register one differently named
tax report, so a US build has the 65 names containing `Tax Schedule
Report/TXF Export`, and a German build has the 65 containing `Tax Report / TXF
Export`.  All 66 names are distinct.

All paths below are relative to `gnucash/report/`.

## 2. Classification

The classes are the task's, applied as follows:

- **native** — the name is returned by `native_report_names()` in
  `libgnucash/reports/gnc-report.cpp` and served by `render_report()`.
- **spec** — the report's accounting table (rows, accounts/dates scope,
  amounts) can be produced today by a JSON `TableSpec`: query over splits plus
  the five columns `account-name`, `debit`, `credit`, `balance`, `subtotal`
  plus grouping `none` / `account-tree` / `period`, with a bar chart optional.
  Presentation-only losses (a code column, depth limit) are noted, because the
  spec has no field for them.
- **logic** — needs bespoke C++: state across rows, owner/invoice/lot/budget
  objects, valuation, a cross-tab, or a second pass.  These are the reports
  that a fifth column cannot rescue.
- **template** — a document layout (invoice, receipt, tax form, eguile page)
  rather than a table of account balances.
- **other** — budgets, charts, scheduled transactions, demo reports, a
  container, and the register view.

### The table

| # | Report name | Source file | Class | Evidence |
|---:|---|---|---|---|
| 1 | Balance Sheet | `reports/standard/balance-sheet.scm` | native | `native_report_names()`; single-column default with Assets/Liabilities/Equity sections. |
| 2 | General Ledger | `reports/standard/general-ledger.scm` | native | `native_report_names()`; trep defaults rendered as a `CellTable`, grouped by account code. |
| 3 | Income Statement | `reports/standard/income-statement.scm` | native | `native_report_names()`; revenue/expense/nets, shares the P&L renderer. |
| 4 | Investment Lots | `reports/standard/investment-lots.scm` | native | `native_report_names()`; lot state, prices, 20 columns, realised/unrealised gains. |
| 5 | Payable Aging | `reports/standard/new-aging.scm` | native | `native_report_names()`; owner/lot aging with six buckets, amounts negated. |
| 6 | Profit & Loss | `reports/standard/income-statement.scm` | native | `native_report_names()`; second registration of the Income Statement renderer. |
| 7 | Receivable Aging | `reports/standard/new-aging.scm` | native | `native_report_names()`; same aging engine on A/Receivable accounts. |
| 8 | Transaction Report | `reports/standard/transaction.scm` | native | `native_report_names()`; trep default columns, grouped by account full name. |
| 9 | Trial Balance | `reports/standard/trial-balance.scm` | native | `native_report_names()`; the "current" variant. |
| 10 | Account Summary | `reports/standard/account-summary.scm` | spec | A flat `TableSpec` (grouping `none`, columns `account-name` + `balance`) reproduces the row amounts; the default also prints Account Code and indents the account tree, and its depth/zero/parent modes have no spec field. |
| 11 | Advanced Portfolio | `reports/standard/advanced-portfolio.scm` | logic | Per-account lot valuation: share counts, price sources, cost basis, realised and unrealised gains in several currencies. |
| 12 | Balance Sheet (Multicolumn) | `reports/standard/balsheet-pnl.scm` | logic | Account-tree rows x period columns (a cross-tab the spec's one grouping axis cannot express). |
| 13 | Cash Flow | `reports/standard/cash-flow.scm` | logic | Classifies counterpart splits into "money in" / "money out" (`cash-flow-calc.scm`), which is not a debit/credit balance table. |
| 14 | Customer Report | `reports/standard/new-owner-report.scm` | logic | Owner report over `gncOwner` documents (invoices, payments), not splits alone. |
| 15 | Customer Summary | `reports/standard/customer-summary.scm` | logic | Walks `gncOwner` customers and their posted documents; owner objects and invoice links. |
| 16 | Employee Report | `reports/standard/new-owner-report.scm` | logic | Same owner-report engine with `GNC-OWNER-EMPLOYEE`. |
| 17 | Equity Statement | `reports/standard/equity-statement.scm` | logic | Start/change/end columns, unrealised gains, closing-entry handling — derived balances, not a query sum. |
| 18 | General Journal | `reports/standard/general-journal.scm` | logic | Transaction listing (date, num, description, transfer, debit, credit) delegated to the register renderer. |
| 19 | IFRS weighted-average cost basis report | `reports/standard/ifrs-cost-basis.scm` | logic | Lot-by-lot weighted-average cost accumulation with commission capitalisation and gains. |
| 20 | Income and GST Statement | `reports/standard/income-gst-statement.scm` | logic | Builds trep columns per tax code from the tax table; multi-pass tax aggregation. |
| 21 | Income Statement (Multicolumn) | `reports/standard/balsheet-pnl.scm` | logic | Same account-tree x period cross-tab as the multicolumn balance sheet. |
| 22 | Investment Portfolio | `reports/standard/portfolio.scm` | logic | Per-account share/price/value valuation at a date. |
| 23 | Job Report | `reports/standard/new-owner-report.scm` | logic | Owner report with `GNC-OWNER-JOB`. |
| 24 | Lot Viewer | `reports/standard/lot-viewer.scm` | logic | Browses `gncLot` objects and their split/lot links. |
| 25 | Reconciliation Report | `reports/standard/reconcile-report.scm` | logic | Trep renderer with a reconcile-date predicate instead of the posted-date range. |
| 26 | Transaction Breakdown Report | `reports/standard/txn-columns.scm` | logic | One generated column per split/account per transaction; custom cell layout. |
| 27 | Vendor Report | `reports/standard/new-owner-report.scm` | logic | Owner report with `GNC-OWNER-VENDOR`. |
| 28 | Australian Tax Invoice | `reports/standard/taxinvoice.scm` | template | Eguile invoice template with Australian tax fields (`support/taxinvoice.eguile.scm`). |
| 29 | Balance Sheet (eguile) | `reports/standard/balsheet-eg.scm` | template | Eguile page over balance-sheet data (`support/balsheet-eg.eguile.scm`). |
| 30 | Easy Invoice | `reports/standard/invoice.scm` | template | HTML invoice document built from invoice/owner objects. |
| 31 | Fancy Invoice | `reports/standard/invoice.scm` | template | Same invoice document with the styled layout. |
| 32 | Printable Invoice | `reports/standard/invoice.scm` | template | Same invoice document with the print layout. |
| 33 | Receipt | `reports/standard/receipt.scm` | template | Eguile receipt over an invoice (`support/receipt.eguile.scm`). |
| 34 | Tax Invoice | `reports/standard/taxinvoice.scm` | template | Eguile invoice template with tax columns. |
| 35 | Tax Report / TXF Export | `reports/locale-specific/de_DE/taxtxf.scm` | template | Locale tax-line table plus TXF/XML export; needs the German tax code table and writers. |
| 36 | Tax Schedule Report/TXF Export | `reports/locale-specific/us/taxtxf.scm` | template | Same machinery for the US schedule; hand-built HTML, not eguile. |
| 37 | Assets | `reports/standard/account-piecharts.scm` | other | Pie chart of asset account balances. |
| 38 | Assets Over Time | `reports/standard/category-barchart.scm` | other | Bar chart of asset balances by period. |
| 39 | Average Balance | `reports/example/average-balance.scm` | other | Time-series average over a step size, with a chart; not a period sum. |
| 40 | Balance Forecast | `reports/standard/balance-forecast.scm` | other | Projects balances from scheduled transactions and historical averages, then charts them. |
| 41 | Budget Balance Sheet | `reports/standard/budget-balance-sheet.scm` | other | Budget-vs-actual balance sheet; needs the budget model. |
| 42 | Budget Chart | `reports/standard/budget-barchart.scm` | other | Bar chart of budget vs actual. |
| 43 | Budget Flow | `reports/standard/budget-flow.scm` | other | Budget flow table over a budget's periods. |
| 44 | Budget Income Statement | `reports/standard/budget-income-statement.scm` | other | Budget-vs-actual income statement. |
| 45 | Budget Profit & Loss | `reports/standard/budget-income-statement.scm` | other | Same budget renderer registered as P&L. |
| 46 | Budget Report | `reports/standard/budget.scm` | other | Budget-vs-actual account table. |
| 47 | Cash Flow Barchart | `reports/standard/cashflow-barchart.scm` | other | Chart of the cash-flow series. |
| 48 | Expense Accounts | `reports/standard/account-piecharts.scm` | other | Pie chart of expense account balances. |
| 49 | Expense Over Time | `reports/standard/category-barchart.scm` | other | Bar chart of expenses by period. |
| 50 | Expenses vs. Day of Week | `reports/example/daily-reports.scm` | other | Weekday statistics with a pie chart. |
| 51 | Future Scheduled Transactions Summary | `reports/standard/account-summary.scm` | other | Instantiates scheduled transactions into the future; no splits exist yet. |
| 52 | Income Accounts | `reports/standard/account-piecharts.scm` | other | Pie chart of income account balances. |
| 53 | Income Over Time | `reports/standard/category-barchart.scm` | other | Bar chart of income by period. |
| 54 | Income & Expense Linechart | `reports/standard/net-charts.scm` | other | Line chart of income/expense over time. |
| 55 | Income/Expense Chart | `reports/standard/net-charts.scm` | other | Bar chart of income/expense over time. |
| 56 | Income vs. Day of Week | `reports/example/daily-reports.scm` | other | Weekday statistics with a pie chart. |
| 57 | Liabilities | `reports/standard/account-piecharts.scm` | other | Pie chart of liability account balances. |
| 58 | Liabilities Over Time | `reports/standard/category-barchart.scm` | other | Bar chart of liabilities by period. |
| 59 | Multicolumn View | `reports/standard/view-column.scm` | other | Container whose "report" is a list of embedded report instances (`report-list` option). |
| 60 | Net Worth Barchart | `reports/standard/net-charts.scm` | other | Bar chart of net worth over time. |
| 61 | Net Worth Linechart | `reports/standard/net-charts.scm` | other | Line chart of net worth over time. |
| 62 | Price Scatterplot | `reports/standard/price-scatter.scm` | other | Scatter chart of commodity prices. |
| 63 | Register | `reports/standard/register.scm` | other | `in-menu? #f` account-register HTML view; the web tier already serves `/register/<guid>`. |
| 64 | Sample Graphs | `reports/example/sample-graphs.scm` | other | Demo charts, no accounting content. |
| 65 | Sample Report | `reports/example/sample-report.scm` | other | Demo table showing the report API. |
| 66 | Securities | `reports/standard/account-piecharts.scm` | other | Pie chart of security account balances. |

## 3. Counts

| Class | Names | Share of 66 | Notes |
|---|---:|---:|---|
| native | 9 | 13.6% | 8 implementations; Income Statement and P&L share one. |
| spec | 1 | 1.5% | Account Summary, accounting table only. |
| logic | 17 | 25.8% | About 7 distinct engines; see the verdict. |
| template | 9 | 13.6% | 4 invoice/receipt documents, 2 tax invoices, 2 tax reports, 1 eguile balance sheet. |
| other | 30 | 45.5% | 19 charts (including Budget Chart), 5 other budget reports, 1 SX summary, 2 that mix a chart with a table/forecast, 1 demo table, 1 register, 1 container. |
| **total** | **66** | **100%** | 65 loaded per build (one locale `taxtxf`). |

Native + spec = **10/66 = 15.2%** of registered names.  If a byte-identical
default is required, the spec entry drops out and native-only is 9/66 =
13.6%.

## 4. Verdict: what stage 7 requires

### By class, in work terms

**native (9 names, already implemented).**  Stage 7 can delete the Guile
bridge for these today; the remaining work is option parity and the
saved-report id map, not new report engines.  The plan's stage 3 already
accepts that the native reports expose a subset (see the sanity check).
**Days to weeks**, dominated by deciding which Scheme options may be dropped
rather than by coding.

**spec (1 name).**  Registering Account Summary as a built-in spec, or leaving
it to the designer, is **1-2 days**.  Reproducing its default code column,
depth limit, zero-balance and parent-balance modes needs spec fields the
designer does not have, so a faithful Account Summary is **a few days more**,
not free.

**logic (17 names).**  This is the real stage-6 cost.  The names cluster into
about seven engines:

- owner reports (Customer, Vendor, Employee, Job, Customer Summary — 5 names):
  one `gncOwner` + document engine.  Aging already shows the unit cost: one
  commit (~500 lines plus a fixture generator) for two names.
- portfolios and cost basis (Advanced Portfolio, Investment Portfolio, IFRS
  cost basis, Lot Viewer — 4 names): one lot/price valuation engine, of which
  Investment Lots is a partial precedent (~1,000 lines).
- cross-tab statements (Balance Sheet (Multicolumn), Income Statement
  (Multicolumn), Equity Statement — 3 names): one period-column engine plus
  equity's closing/unrealised-gain rules.
- entry listings (General Journal, Transaction Breakdown, Reconciliation — 3
  names): custom `CellTable` renderers, one of which needs a reconcile-date
  query predicate.
- Cash Flow (1 name): money-in/out classification over counterpart splits.
- Income and GST (1 name): tax-table columns and a second aggregation pass.

**Weeks per cluster, months in total.**  This is not 17 independent reports,
but it is not one engine either.

**template (9 names).**  The four eguile pages (Balance Sheet (eguile),
Receipt, Tax Invoice, Australian Tax Invoice) need either a small template
engine or four bespoke renderers.  The three invoice reports are already
hand-built HTML and need the invoice/owner model plus print CSS.  The two
`taxtxf` reports are not eguile pages at all: they need locale tax-code tables
and TXF/XML writers, which are a separate export subsystem.  **Weeks to
months**, and the tax side is the least like anything already native.

**other (30 names).**  Budgets (6, one of which is the budget chart) need a
budget model in the native layer (**weeks**); charts (19) need chart types
beyond the single `Bar` type and series plumbing, but Chart.js is already
vendored (**days each**, maybe two weeks for the set); the scheduled-transaction
reports (Future Scheduled Transactions Summary, Balance Forecast — 2) need SX
instantiation (**weeks**); Register is already served by the web UI and
Multicolumn View is a container that saved specs could replace (**days**); the
five reports under `reports/example/` can simply be dropped (**zero**), though
Average Balance is a real analytic that happens to live in the example tree.

**Stage 7 itself** is therefore two different projects depending on the
answer to "what must the web tier keep":

- Web tier keeps only the statements, the transaction listing and a custom
  report designer: **days of deleting the bridge**, plus the saved-report map.
  This is the plan's "small in code".
- Web tier keeps invoices, tax forms or budgets: **months**, because those are
  new subsystems (documents, tax tables, budget model), not report ports.

### What could be declared out of scope, and what a user loses

| Candidate | Names | Loss |
|---|---:|---|
| Example reports | 5 | Four demos lose nothing; Average Balance is a usable analytic that would need a home. |
| Chart-only reports | 19 | Visual trends, net-worth and price comparisons.  Some numbers remain reachable through account balances; category/price breakdowns do not.  Budget Chart is also counted under budgets. |
| Register | 1 | Nothing; `/register/<guid>` already exists in the web tier. |
| Multicolumn View | 1 | Composite dashboards; saved specs partially replace them. |
| Budget reports | 6 | Budget-vs-actual and budget statements.  The budget data stays in the book but becomes unreportable. |
| Owner reports | 5 | Customer/vendor statements and employee/job reports.  Aging still answers "how old", not "what does this customer owe". |
| Portfolio / cost-basis / lot viewer | 4 | Cost basis and realised/unrealised gains; Investment Lots covers the lot table but not the portfolio summaries. |
| Invoices and receipts | 5 | Printable customer documents — the billing output itself. |
| Tax invoices and tax reports | 4 | Compliant invoice layouts and the US/DE tax schedule + TXF/XML export. |
| Cash Flow, Equity, GST, multicolumn statements | 5 | Statement variants and tax reporting; the single-column Balance Sheet/P&L remain. |

The honest split: examples, charts, Register and Multicolumn are defensible
out-of-scope declarations.  Invoices, tax forms and budgets are not, if the web
tier is meant to serve businesses rather than only people reading their books.

### Is the numeric majority covered?

**By count, no: 10 of 66 names (15%).**  That number is not close to a
majority and should not be presented as one.

**By routine use, plausibly yes — with two caveats.**  The nine native names
include the reports that produce the accounting statements and the listing:
Trial Balance, Balance Sheet, Income Statement, Profit & Loss, Transaction
Report and General Ledger.  These are the reports every set of books needs to
answer "what do I have, what did I earn, and what happened?"; the other 57 are
account summaries, documents (invoices, receipts, tax), analytics (portfolios,
equity, cash flow), budgets and charts.  If usage is concentrated where the
accounting function is concentrated, the majority of report *views* is already
covered.

The caveats are material:

1. There is no usage telemetry in the tree, so this is an argument from
   function, not a measurement.  It should be labelled as such wherever it is
   used to justify stage 7.
2. "Majority of views" is not "majority of users".  A business that issues
   invoices or files the US/DE tax reports hits the template class on day one;
   for that user the 56 non-native names include the blocking ones, and no
   amount of statement coverage compensates.

The defensible claim is: **the native layer covers the accounting core, not
the document/analytics tail.**  Whether that is "the numeric majority" depends
on a usage distribution nobody has measured; the plan should not assert it
without saying which claim it is making.

## 5. Sanity check: the five most-used reports

The five were read against their Scheme option generators and the native
implementation in `libgnucash/reports/gnc-report.cpp`.  Counts are option
registration calls in the Scheme source (helpers such as
`gnc:options-add-subtotal-view!` register more than one option, so they are
lower bounds on the user-visible options).

| Report | Scheme options | Native descriptors | Complete for its own descriptors? |
|---|---:|---|---|
| Trial Balance | 18 | `start`, `end`, `currency`, `period` | Yes, but `start` is a declared no-op. |
| Balance Sheet | 22 | `start`, `end`, `currency`, `period` | Yes, but `start` is a declared no-op. |
| Income Statement / P&L | 25 | `start`, `end`, `currency`, `period` | Yes. |
| Transaction Report | 45 (trep) | `start`, `end`, `accounts`, `period` | Yes; empty selection renders the Scheme warning. |
| General Ledger | 45 (trep) | `start`, `end`, `accounts`, `period` | Yes; empty selection renders the Scheme warning. |

### Trial Balance

- Scheme (`reports/standard/trial-balance.scm`): report title; start and end
  dates; **report variation** (`current`, `pre-adj`, `work-sheet`); account
  selection; levels of subaccounts (default 1); merchandising gross-adjustment
  and income-summary accounts; report currency; price source; show foreign
  currencies and exchange rates; adjusting-entry pattern/case/regexp;
  closing-entry pattern/case/regexp; account hyperlinks.  The zero-balance
  toggle is commented out in the source.
- Native (`trial_balance()`): the `current` variant only, every account of a
  reportable type, no rollup, debit/credit split and totals, optional currency
  conversion.
- **Verdict:** complete against its four descriptors.  `start` is accepted but
  has no effect, and the descriptor says so; the Scheme `current` variant also
  ignores it (it passes only an end date to the account table), so this is a
  faithful no-op rather than a regression.
- **Gaps, all rejected rather than silently ignored:** `pre-adj`/`work-sheet`,
  account selection, depth, merchandising accounts, price source, foreign
  amounts/rates, adjusting/closing patterns and hyperlinks.  A caller sending
  them gets a 400 naming the option, not a report that quietly differs.

### Balance Sheet

- Scheme (`reports/standard/balance-sheet.scm`): title; balance-sheet date;
  report form; standard order; account selection; depth (default 3); bottom
  behaviour; currency; price source (nearest); foreign amounts/rates; show
  zero accounts; omit zero balances; parent-balance mode; parent subtotals;
  account hyperlinks; rules; and six label/total toggles for the three
  sections.
- Native (`balance_sheet()`): single-column default, Assets → Liabilities →
  Equity, retained earnings from income/expense, section and grand totals, a
  bar chart of the three totals.
- **Verdict:** complete against its descriptors; `start` is another declared
  no-op.  The hardcoded 3-level display (`display_depth_limit = 2`) matches the
  Scheme default, and the default depth was a parity issue on
  `taxreport.gnucash` that the code explicitly handles.
- **Gaps:** multicolumn form, account selection, depth, zero-balance modes,
  parent-balance/subtotal modes, price source, foreign amounts/rates, rules,
  hyperlinks and the six section toggles.  The chart is a native addition, not
  a Scheme option.

### Income Statement / Profit & Loss

- Scheme (`reports/standard/income-statement.scm`): title; start/end; account
  selection (income + expense by default); depth (3); bottom behaviour;
  currency; price source; foreign amounts/rates; zero-balance modes; parent
  modes; hyperlinks; rules; label/total toggles for revenue, trading and
  expense; two-column form; standard order; closing-entry pattern/case/regexp.
- Native (`income_statement()`): revenue and expense accounts over the period,
  closing entries excluded by fixed pattern, sign-reversed revenue, net
  income/loss, optional currency conversion.
- **Verdict:** complete against its descriptors.
- **Gaps:** account selection, depth, zero/parent modes, price source,
  foreign amounts/rates, rules, hyperlinks, two-column form, trading section and
  configurable closing pattern.  One content note: the engine has a
  `Bucket::Trading`, but the income statement never renders it.  The Scheme
  default selection is income + expense only, so defaults agree; the native
  side simply has no way to add trading accounts, because it has no account
  selection at all.

### Transaction Report

- Scheme (`reports/standard/transaction.scm` → `trep-engine.scm`):
  the full trep surface — date range; style; account selection/filter type;
  display columns (date, num, description, memo, account, transfer, shares,
  price, amount, running balance, totals, sign reversal); detail level; sort
  keys and orders; primary/secondary subtotals; filters (void, closing,
  reconcile, date, amount, description, account); currency/price source;
  export.
- Native (`transaction_report()`): fixed columns (date, num, description,
  memo/notes, transfer, debit, credit), grouped by account full name in
  register order, subtotal per account and a grand total; voided and closing
  transactions excluded, matching the Scheme defaults.
- **Verdict:** complete against its descriptors.  An empty `accounts` produces
  the Scheme "No accounts selected" warning; the route expands a GUID to the
  account and its subtree, as its descriptor says.
- **Gaps:** every display/filter/sort/subtotal option is absent (rejected, not
  ignored), as are currency, style and export.  A saved Scheme Transaction
  Report with a custom column set or sort cannot be replayed; the native
  renderer will show its fixed layout unless the compatibility layer treats
  the mismatch as unsupported.

### General Ledger

- Scheme (`reports/standard/general-ledger.scm`): the trep surface with
  defaults changed to account-code primary key, running balance on, single
  detail level, account-code display on and subtotals on.
- Native (`general_ledger()`): fixed account-code grouping (accounts sharing a
  code share a group), date/num/description/transfer/debit/credit plus running
  balance, "Balance b/f" and "Total For" rows.
- **Verdict:** complete against its descriptors.
- **Gaps:** the same trep display/sort/filter surface, including the ability to
  change the primary key away from account code, turn running balance off, or
  select a different detail level.

### What is ignored, and what is silent

The HTTP layer is strict: an option not in `report_options()` is a 400 that
names it (`web/server/main.cpp`), so the large gaps above are *rejected*, not
silently mis-rendered.  Three cases were different from a clean rejection; the
last two were fixed in the audit that followed this analysis:

1. **Trial Balance and Balance Sheet `start`.**  Both declare it, accept it,
   and ignore it.  The descriptor text explains why, so it is documented
   rather than hidden, but a caller can still send a start date and receive a
   report that does not use it.  This remains a documented no-op.
2. **`period` on the reports that cannot honour it.**  Investment Lots
   advertised `period=all` and ignored it, and the aging reports accepted
   `period` even though they never advertised it, because the route resolved
   `period` before checking the descriptors.  Fixed: the route now checks
   `period` with `report_has_option()`, the lots descriptor no longer
   publishes it (its end date is also the valuation date, so "all dates"
   cannot mean an unbounded end), and aging and lots both answer 400.
3. **Inverted date ranges.**  An end date before the report's default start
   (`Income Statement?end=2009-12-31`) made the engine's split iterator run
   `std::for_each(first, last)` with first > last and segfaulted `gnc-webd`.
   `Query::sum()` now treats an inverted range as empty rather than a crash.

`web/test/report-options.sh` now walks every native report and every option
its descriptor advertises: each must change the rendered document or be
refused with a 400 that names it, except the two documented as-of no-ops.
Currency effectiveness is the one gap the test records: the fixture has only
USD, so there is no second currency to convert into.

## 6. Surprises

- **The count is 66, not 63.**  The plan's line 27 undercounts by three.  The
  "63" may predate the loop registrations or exclude three names; the sources
  are unambiguous and give 66.
- **Nine names, eight implementations.**  Income Statement and Profit & Loss
  are the same renderer registered twice.  Counting "reports ported" as nine
  overstates the engine work by one.
- **Almost half the registry is not report-shaped.**  30 of 66 names are
  budgets, charts, scheduled transactions, demos, a container or the register
  view.  "Port all the reports" is really "port the statements, then decide
  how much of the document/analytics tail matters".
- **The designer's vocabulary is the Trial Balance's, not the report engine's.**
  Five columns and one grouping axis generalize to exactly one non-native
  report (Account Summary) and then stop.  The gap is not 56 independent
  reports; it is a handful of missing *renderer families*: transaction
  listings, owner documents, lot/price valuation, cross-tabs, budgets, and
  template pages.
- **The tax reports are locale registrations.**  A US and a German build have
  different 65-name registries; any count that says 66 is counting sources,
  not a running engine.
- **The hidden `Register`.**  It is a registered report (`in-menu? #f`) but is
  not in the Reports menu and is already replaced by the web register page.
  Registry counts and menu counts are not the same number.

## 7. Reproducing the measurement

```sh
# registration call sites
git grep -c "gnc:define-report" -- gnucash/report/reports

# names, including the three loop sites
# (account-piecharts 5, category-barchart 4, daily-reports 2)
rg "'name" gnucash/report/reports --glob '*.scm'
```

The arithmetic and the per-file counts are in the table above.  No Guile
binary was available to capture `/api/reports` live, so the registry is the
static one the loader would build; `reports.scm` shows which module prefixes
are loaded, and the locale loader explains the 66-vs-65 difference.

## 8. Distribution

`doc/CMakeLists.txt` adds this file to `doc_noinst_DATA`, which feeds
`doc_DIST_local` and therefore `doc_DIST` in the parent scope, so the analysis
ships with the documentation.  No other file changed.
