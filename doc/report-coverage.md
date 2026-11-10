# Scheme report coverage under the native report layer

Status: measurement for `doc/report-engine-migration.md`
Branch: `webui/engine-server`
Measured at: the body at `0da4be5ca1`; the addendum at the top and section 15
carry the current counts (39 native, 0 unported, 0 template, 25 closed)
Scope: registry enumeration, classification, coverage and cost. No report
behaviour was changed by the measurement itself; the later stages edited
`libgnucash/reports/`, `web/`, tests and this document.

> **Addendum: this is the current measurement.**  The body of this document was
> first measured at `0da4be5ca1` with **34 native names**; later stages added
> `Cash Flow`, `Income and GST Statement`, `Easy Invoice`, `Fancy Invoice` and
> now `Cash Flow Barchart`.  The counts to quote are:
>
> - **Native: 39 names** (34 + `Cash Flow` + `Income and GST Statement` +
>   `Easy Invoice` + `Fancy Invoice` + `Cash Flow Barchart`).
> - **By name: 39/66 = 59.1% native**; native plus spec (the two net-worth
>   charts) is **41/66 = 62.1%**; native plus spec plus closed (25) is
>   **66/66 = 100%** done-or-decided.
> - **Unported: 0/66.**  The last four logic names resolved: `Cash Flow
>   Barchart` is native (section 15), and `Average Balance` (43), `Balance
>   Forecast` (44) and `Future Scheduled Transactions Summary` (45) are closed
>   with the reasons recorded in section 3.5.  **No template-class name
>   remains**: Easy and Fancy Invoice are native (section 14), and the only
>   other template name, Balance Sheet (eguile), is closed.
> - The per-report and per-class tables (sections 3.1-3.6, 4.2) have been
>   updated in place through section 15, so their numbers agree with the
>   counts above.  The one exception is a paragraph that explicitly calls
>   itself a stage record (sections 12-14): those keep the counts that were in
>   force when the stage landed, and a later stage's update supersedes them.
>   The cost estimates in section 6.3 are now all either done or closed; there
>   is no unported work left to price.
>
> **How the names closed.**  Each is a port or a recorded decision, not a stub.
> `Cash Flow` (`standard/cash-flow.scm` plus `cash-flow-calc.scm`) classifies
> the counterpart splits of the selected accounts into money in and money out
> and is implemented natively with its Scheme options (dates, report currency,
> account selection, full names, selected-account list, trading accounts and
> display depth); the parity harness's `cash-flow` extractor compares the
> section headings, every account row and the three totals, verbatim, on
> `sample1` and on `taxreport.gnucash` in both builds (including the 56-row
> taxreport period run and every display-switch variant).
> `Income and GST Statement` (`standard/income-gst-statement.scm`) is a
> trep-engine listing with calculated columns; the native report reproduces the
> engine's row selection (sales/purchases splits, non-void, non-closing, the
> report's NONE/INVOICE transaction filter, unique transactions by date then
> the engine's split order), the Date/Num/Description/Memo-Notes columns and
> every column definition of all three formats (default, Australia BAS, UK
> VAT, including the *EUVAT*/*EUGOODS* markers) with the six optional-column
> switches.  The parity harness's `gst` extractor compares the headings,
> every transaction row and the Grand Total, verbatim, on both fixtures in both
> builds, on all three formats and with the optional columns enabled.
> `Easy Invoice` and `Fancy Invoice` are the other two `standard/invoice.scm`
> registrations.  They are not different documents from the Printable Invoice:
> all three are `reg-renderer` over one options-generator, and the variant
> changes only the default CSS text and the "Row 3 Right" slot (the print date,
> which the Fancy variant leaves empty).  The native layer now renders the one
> document three ways from one `InvoiceDocument`, with the variant choosing
> those two defaults, and publishes the invoice report's full option surface
> (section 14).
> `Cash Flow Barchart` (`standard/cashflow-barchart.scm`) is not a second
> calculation: it is the Cash Flow report's `cash-flow-calc-money-in-out()`
> classification run once per date interval and plotted.  The native layer
> extracts that classification from `cash_flow()` into `cash_flow_flows()`, so
> the barchart and the Cash Flow statement cannot disagree, and reproduces
> `gnc:make-date-interval-list()` for all seven step sizes (day, week, two
> weeks, month, quarter, half year, year; a month-end start stays on month
> ends).  The parity harness gained the `chart` comparison for the name; the
> chart type, label count and every plotted value match the Scheme report's
> embedded chart data on `sample1` and `taxreport.gnucash` in both builds, for
> the default period and for every step size (section 15).
>
> The three remaining logic names are **closed**, with the reasons a later
> stage can act on in section 3.5: `Average Balance` (43), `Balance Forecast`
> (44) and `Future Scheduled Transactions Summary` (45).  The short form: the
> first needs the Scheme weighted-average price engine, which the native layer
> does not have, and the two scheduled-transaction reports need a
> scheduled-transaction data path the native layer has never had -- and no
> fixture in the tree has a scheduled transaction at all, so parity cannot be
> established for a port even where the primitive is reachable.
>
> One defect was fixed on the way, and it is worth recording because it would
> have undermined every other figure: the Advanced Portfolio's rate of gain and
> rate of return printed a cent low when the rate landed on a decimal half
> (`30.64` where the Scheme report prints `30.65` on `taxreport`).  The cause
> was not the arithmetic or the denominator -- `100.0 * gain / money_in` and
> `100.0 * (gain / money_in)` are bit-identical here -- but the formatter:
> Scheme's `(format #f "~,2f" ...)` rounds the *shortest round-trip decimal*
> representation half away from zero, while C's `%.2f` rounds the exact binary
> value, which sits just below the tie.  `gnc-report.cpp` now has a
> `format_fixed_2()` that reproduces Guile's rounding, and the Advanced
> Portfolio's `taxreport` parity case passes verbatim.
>
> Three empty-selection pages were also wrong and are now the pages the Scheme
> reports render: Investment Portfolio prints its headings with no rows,
> Advanced Portfolio prints its headings and its zero-valued Total row, and
> Customer Summary prints "No valid customer found." instead of a zero total.
>
> **Open defect: the two net-worth built-ins do not match the Scheme chart
> under an explicit date range.**  The comparison that exposes it is
> `report-parity.sh <qt> <build> <src> sample1.gnucash "Net Worth Barchart"
> 2018-01-01 2018-12-31` (the Linechart fails the same way), on both
> `ext4-web` and `ext4-noguile`: four differing rows every time.  The Scheme
> document plots 13 points, `01/01/18` … `12/01/18` plus the range end
> `12/31/18`, with `Assets [0,0,10000,…]`, `Liabilities [0,0,585,…]` and
> `Net Worth [0,0,9415,…]`; the built-in plots 12 points, `January 2018` …
> `December 2018`, with `Debit [0,10000,…]`, `Credit [0,585,…]` and
> `Balance [0,9415,…]`.  The amounts are the same; the axes are one step
> apart.  The cause is the step convention, not the range: `net-charts.scm`
> builds its dates with `gnc:make-date-list (end-day-time from) (end-day-time
> to) MonthDelta`, which steps from the from-date and appends the end date
> when it is not a step (`bindings/guile/date-utilities.scm:356`), while a
> period spec plots calendar month ends.  The two agree when the from-date is
> itself a month end -- `2017-12-31 .. 2018-12-31` matches, 6 rows, which is
> why the earlier measurement saw the same series -- and disagree by one step
> when it is not.  Fixing it means teaching the period axis to step from the
> from-date and append the end date (a change shared by every period report,
> and a new spec field), not a one-line correction to these two reports.  It
> is recorded as open; the matrix therefore reports **58/62** (29/31 per
> build), not 62/62, until it is fixed.  It is independent of the budgets
> work: the five budget reports pass the same matrix in both builds.

> **What changed since the first measurement.**  The first version of this
> document was measured at `a639555722`, when nine Scheme report names rendered
> natively, and its verdict was "the majority is not covered".  At
> `4826c2e262` plus the owner-report work the native layer named 24 of the
> **66** source names; the current measurement (this revision) names **31**.
> The registry itself has not changed: no file under
> `gnucash/report/reports/` changed between `a639555722` and this measurement.
> All the movement is in the native layer.

## Summary

- The Scheme engine registers **66 report names** from
  `gnucash/report/reports/**`: 58 `(gnc:define-report ...)` call sites, of
  which three are loops that register 5, 4 and 2 names, so
  `55 + 5 + 4 + 2 = 66`.  A build loads **65** because the locale loader picks
  one of the two `taxtxf` reports.  The count is unchanged from the earlier
  measurement; the plan's "63 report definitions" is still stale.
- **Native: 39 names.**  The six original core reports, the three entry
  listings, aging and lots, the five budget layouts, the two cash-flow/GST
  statements plus the Cash Flow Barchart, the **six** native documents
  (Printable, Easy and Fancy Invoice, Receipt, Tax Invoice and Australian Tax
  Invoice), the US tax schedule with TXF export, the four
  valuation/portfolio reports, the five owner reports (four registrations of
  one owner-history renderer plus Customer Summary), and the three cross-tab
  statements (the multicolumn Balance Sheet and Income Statement and the
  Equity Statement).
- **Spec: 2 names** are expressible in the designer as it stands today
  (Net Worth Barchart and Net Worth Linechart, as built-in specs).  The other
  three names the earlier measurement called spec-expressible — Account
  Summary, Income/Expense Chart and Income & Expense Linechart — are closed
  with the reasons in section 3.5.  The **6 built-in specs** are extra
  reports, not registry names; they are listed separately below and do not
  change the 39/66 numerator.
- **Logic: 0 names.**  The Cash Flow Barchart is native; the other three
  logic names (Average Balance, Balance Forecast and Future Scheduled
  Transactions Summary) are closed with the reasons in section 3.5.
  **Template: 0 names** — Easy and Fancy Invoice are native, and Balance
  Sheet (eguile) is closed.  **Closed: 25 names**, deliberately out of scope
  with the reason recorded.
- **By name: 39/66 = 59.1% native.**  Native plus spec is
  **41/66 = 62.1%**.  Native plus spec plus closed is **66/66 = 100%**
  done-or-decided; the unported remainder is **0/66**.
- **Accounting core: 6/6 = 100%** on the six names the earlier document called
  the accounting core (Trial Balance, Balance Sheet, Income Statement,
  Profit & Loss, Transaction Report, General Ledger); 7/7 = 100% if the General
  Journal is added, and 11/11 = 100% if the equity, cash-flow and multicolumn
  statements are added too.  The definition is stated with each number.
- **Verdict:** the web tier can be Guile-free for a real user, but not for
  every real user.  A small business can run the reporting side of the
  bookkeeping loop, including owner statements and all six invoice/receipt
  documents, natively; a business that needs equity/cash-flow/multicolumn
  statements, GST or portfolios cannot.  **No report name is unported any
  more**: every one of the 66 is native, spec-expressible or closed with a
  recorded decision.  The remaining work is the closed families, not a
  backlog: full parity including the deliberately closed chart families,
  ELSTER, the weighted-average price engine and the scheduled-transaction
  engine would be months, because those are new subsystems.
- **Basis:** counts are derived from the sources in this checkout, not from a
  telemetry sample.  The registry is the static one the loader would build,
  and there is **no telemetry in the tree**, so every usage claim below is an
  argument from function, not a measurement.

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
`66` names, none duplicated, and all 39 native names are members of that set.
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
| `GET /api/reports` | 69 | 45 | The report list the server actually offers |
| Native names | 39 | 39 | Names `render_report()` handles without Guile |

In a Guile build, `/api/reports` enumerates the Scheme templates that build
actually loaded (`65`, one locale `taxtxf`) and then appends the six built-in
designer specs, except that the two net-worth names are already in the loaded
list and the built-in replaces them (`+4 = 69`).  In a Guile-free build it
enumerates `native_report_names()` (`39`) and appends the same six
(`+6 = 45`).  The six
built-ins are not Scheme names and are not counted in the 66.  A Guile-free
build therefore offers **45 report-list entries**, but the by-name coverage of
the Scheme registry is **39/66**, not 45/66.  The "designer with four built-in
chart specs" was the old `+4`; the two net-worth charts now make it `+6`, and
none of them is an extra registry name.

The 65-name build is not identical across locales: a US build loads `Tax
Schedule Report/TXF Export` (native), a German build loads `Tax Report / TXF
Export` (closed).  Both builds have 65 Scheme names; the intersection of the
native names with the loaded Scheme names is 39 in the US build and 38 in the
German one.  The Guile-free list is locale-independent: it is always the 39
native names, including the US schedule, so a German-locale Guile-free build
still does not offer the German report.

## 2. What "native" means here

A name is native if it is returned by `native_report_names()` in
`libgnucash/reports/gnc-report.cpp` and handled by `render_report()`.  The
current list is:

```cpp
return {"Trial Balance", "Balance Sheet", "Income Statement",
        "Profit & Loss", "Balance Sheet (Multicolumn)",
        "Income Statement (Multicolumn)", "Equity Statement",
        "Transaction Report", "General Ledger",
        "General Journal", "Transaction Breakdown Report",
        "Reconciliation Report",
        "Receivable Aging", "Payable Aging", "Investment Lots",
        "Investment Portfolio", "Advanced Portfolio", "Lot Viewer",
        "IFRS weighted-average cost basis report",
        "Budget Report", "Budget Income Statement",
        "Budget Profit & Loss", "Budget Balance Sheet", "Budget Flow",
        "Printable Invoice", "Easy Invoice", "Fancy Invoice", "Receipt",
        "Tax Invoice",
        "Australian Tax Invoice", "Tax Schedule Report/TXF Export",
        "Customer Report", "Vendor Report", "Employee Report",
        "Job Report", "Customer Summary", "Cash Flow",
        "Cash Flow Barchart",
        "Income and GST Statement"};
```

That is 39 names.  Several share a renderer: Income Statement and Profit &
Loss share one; the two multicolumn statements share one; Budget Income
Statement and Budget Profit & Loss share one; Tax Invoice and Australian Tax
Invoice share one; Receivable and Payable Aging share one; Investment and
Advanced Portfolio share one; Customer Report, Vendor Report, Employee
Report and Job Report share one; Printable, Easy and Fancy Invoice share
one (invoice.scm registers all three from one renderer); and the Cash Flow
statements share the classification.  Counting "reports ported" as 39
overstates the engine work, and the three invoice layouts add one renderer, not
three.

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
hidden by the 39/66 number.

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

### 3.1 Native (this table, plus 3.1a, 3.1b, the entry listings in section 11, the cash-flow/GST statements in section 13 and the two invoice layouts in section 14)

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
| 15 | Printable Invoice | `standard/invoice.scm` | `render_invoice()` over `InvoiceDocument` with the Printable variant: the customer/vendor document, the six header slots, nine display columns, the layout page (CSS, picture) and the print switches. |
| 46 | Easy Invoice | `standard/invoice.scm` | The same renderer and option surface with the Easy variant's default stylesheet; the document is the Printable Invoice's, by design -- invoice.scm registers all three from one renderer. |
| 47 | Fancy Invoice | `standard/invoice.scm` | The same renderer with the Fancy variant's default stylesheet and its empty "Row 3 Right" (no print-date row).  The other two documents print that date. |
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

The three entry listings — General Journal, Transaction Breakdown Report and
Reconciliation Report — are native too; section 11 documents them, and they
are the three names that take the native total from 28 to 31.

#### 3.1c The Cash Flow Barchart (registry number 28)

`standard/cashflow-barchart.scm` is the Cash Flow statement's calculation
plotted over date intervals, not a second engine.  Its renderer calls
`cash-flow-calc-money-in-out()` once per interval with the interval's own
`from`/`to`, then sums each interval's money-in and money-out collectors into
the report currency and derives Net Flow = in − out.  The native layer
extracted that classification out of `cash_flow()` into `cash_flow_flows()`
(the same triggers, counterpart rule, per-transaction dedup, absolute-value
conversion and trading-account rule), so the barchart and the statement cannot
disagree by construction, and reimplemented `gnc:make-date-interval-list()`
for the report's seven step sizes.  The interval semantics are the Scheme
function's: a month/quarter/half-year/year step counts from the *start* date
and keeps a month-end start on month ends, a day/week/two-week step steps by
days, every interval ends one second before the next boundary except the last,
which is clamped to the end date, and the label is the interval start.

The chart has the three series the display switches ask for, in the Scheme
order (Money In, Money Out, Net Flow), one bar per interval, and a "no data"
warning instead of a chart when every value of all three lists is zero — the
Scheme `gnc:not-all-zeros` test runs over the three *computed* lists, not only
the plotted ones.  The optional table is the Scheme "Overview:" table: the
interval start, the plotted series, and a Total row.  The published options
are the date interval, the step size, the currency, the account selection, the
three series switches, "Show Table" and the trading-account switch; the price
source is not published for the same reason the Cash Flow statement does not
publish it (the native conversion is the Scheme option's default,
pricedb-nearest).

`report-parity.sh` gained the `chart` comparison for the name (GUID
`5426e4d987f6444387fe70880e5b28a0`), and the chart type, label count and every
plotted value match the Scheme report's embedded chart data on `sample1` and
`taxreport.gnucash` in `ext4-web` and `ext4-noguile` — 12 monthly bars by
default, and the same data re-bucketed for day, week, two-week, quarter,
half-year and year steps, plus a range that starts and ends mid-month.

### 3.2 Spec (2 names expressible today)

The spec vocabulary is `columns` (`account-name`, `debit`, `credit`, `balance`,
`subtotal`), `grouping` (`none`, `account-tree`, `period`), `period_unit`
(`year`, `month`), `period_mode` (`activity`, `cumulative`), `account_types`,
`start`/`end`/`date_preset`, `currency`, `accounts`, `chart` (`bar`, `line`) and
`chart_series` (one, two, or on a `period` report all three of `debit`, `credit`
and `balance`).  A chart has the table's rows as labels.  On a period report the
three amount columns are that period's two sides and its net, so the complete
trio is one plot rather than a total beside its own part; on a flat or
account-tree report `balance` is an account balance, not a period net, and the
trio stays a 400.  The chart cannot add an account dimension as a series,
cannot stack more than three series, and has no pie, scatter or multi-column
layout.

| # | Report | Source | Spec recipe | What is lost |
|---:|---|---|---|---|
| 23 | Net Worth Barchart | `standard/net-charts.scm` | `grouping: "period"`, `period_unit: "month"`, columns `debit` + `credit` + `balance`, `period_mode: "cumulative"`, account types `BANK, CASH, ASSET, STOCK, MUTUAL, CURRENCY, CREDIT, LIABILITY, RECEIVABLE, PAYABLE`, `chart: "bar"`, series `debit`, `credit` + `balance`, `date_preset: "all"`. | The Scheme report's default step dates run from the from-date; the built-in uses calendar month ends over the book's transaction range.  `debit`/`credit` are the balance signs, not the account types: an overdrawn asset lands on the credit side where the Scheme report keeps it under Assets as a negative value.  Step sizes other than month/year, the table toggle, price source and plot geometry. |
| 24 | Net Worth Linechart | `standard/net-charts.scm` | Same as 23 with `chart: "line"`. | Same as 23. |

The two are shipped as the built-ins `builtin-net-worth-barchart` and
`builtin-net-worth-linechart`, under the Scheme names.  On `sample1.gnucash`
over `2017-12-31..2018-12-31` the native chart's three 13-point series are
`Debit [0,0,10000,...]`, `Credit [0,0,585,...]`, `Balance [0,0,9415,...]`;
the Scheme chart's `Assets`, `Liabilities` and `Net Worth` are the same three
lists (see `report-parity.sh`, which compares the chart data directly).  The
series labels are the amount-column names ("Debit", "Credit", "Balance"), not
the Scheme wording, and the built-in covers the book's data range rather than
the Scheme report's current-fiscal-year default.

The other three names this section used to claim are **closed** (section 3.5),
not approximated:

- **Account Summary** — the Scheme default sign-reverses credit-normal
  accounts (A/P `+585`, Equity `+10,000`), shows own-balance parent rows with no
  subtotal rows, offers an Account Code column and reports as of today.  The
  spec's `balance` is the raw signed amount, `account-tree` prints subtree
  rollups plus a subtotal row per parent, there is no code column, and the date
  presets only reach `all`/`this-year`.
- **Income/Expense Chart** and **Income & Expense Linechart** — the Scheme
  third series is Net Profit = income − expense.  The spec's `balance` is
  debit − credit = expense − income, so the third series would be plotted with
  the opposite sign, and there is no negated-balance series.  The first two
  series (Income = credit, Expense = debit) do match; the report as a whole
  does not.

The six built-in specs are the evidence that the vocabulary works, but they
are **new reports, not ports**, and their names are not in the 66 except the
two net-worth names:

| Built-in id | Report | Scope and chart |
|---|---|---|
| `builtin-income-expenses-month` | Income vs Expenses by Month | `INCOME` + `EXPENSE`, one line point per month over the book's transaction range, two series (`credit` income, `debit` expenses). |
| `builtin-expenses-by-account` | Expenses by Account | `EXPENSE`, all dates, one bar per expense account with activity. |
| `builtin-income-by-account` | Income by Account | `INCOME`, all dates, one bar per income account with activity. |
| `builtin-net-worth-month` | Net Worth by Month | asset and liability types, cumulative balance at each month's end, one line series. |
| `builtin-net-worth-barchart` | Net Worth Barchart | asset and liability types, cumulative balance at each month's end, three series: assets, liabilities and net worth. |
| `builtin-net-worth-linechart` | Net Worth Linechart | the same three series as a line. |

`Expenses by Account` and `Income by Account` are bar views of the same data
as the pie reports, not the pie reports themselves; that is why the pie names
are closed rather than called spec (section 3.5).  The two net-worth charts
are the only Scheme chart names whose plotted data the spec can compute
without another engine; Average Balance, Cash Flow Barchart and Budget Chart
all need a computation the spec does not have.

### 3.3 Logic (0 names remain; the table records the 6 of the 34-name measurement)

`Cash Flow` (27) is native since section 13, `Cash Flow Barchart` (28) since
section 15, and `Income and GST Statement` (35) since section 13; they are
listed here only because this table is the 34-name measurement.  **Nothing in
this class remains unported.**  The other three names are closed in section
3.5; the table keeps them with their closing reason so the cluster analysis
below still reads.

| # | Report | Source | Cluster | Why it needs custom native code |
|---:|---|---|---|---|
| 27 | ~~Cash Flow~~ | `standard/cash-flow.scm` | cash flow | **Native since section 13.** |
| 28 | ~~Cash Flow Barchart~~ | `standard/cashflow-barchart.scm` | cash flow | **Native since section 15.**  Reuses the Cash Flow classification per date interval; the chart is the standard bar chart. |
| 35 | ~~Income and GST Statement~~ | `standard/income-gst-statement.scm` | tax/GST | **Native since section 13.** |
| 43 | ~~Average Balance~~ | `example/average-balance.scm` | analytics | **Closed (section 3.5).**  Time-series average over a step size, with a chart; its default price source is the Scheme weighted-average engine. |
| 44 | ~~Balance Forecast~~ | `standard/balance-forecast.scm` | scheduled transactions | **Closed (section 3.5).**  Projects balances from scheduled transactions and historical averages, then charts them. |
| 45 | ~~Future Scheduled Transactions Summary~~ | `standard/account-summary.scm` | scheduled transactions | **Closed (section 3.5).**  Instantiates scheduled transactions into the future; no splits exist yet. |

The clusters matter more than the count: the two cash-flow names share the
money-in/out classification (now one helper), and the two scheduled-transaction
names share an engine (closed together).  The general journal, transaction
breakdown and reconciliation listing, the five portfolio names, the five owner
reports and the three cross-tab statements (section 13) are no longer in this
table either: they are native now (sections 3.1, 3.1a and 3.1b).

### 3.4 Template (0 names)

The two names this class held -- Easy Invoice (46) and Fancy Invoice (47) --
are native since section 14.  invoice.scm registers them from the same
renderer and options-generator as the Printable Invoice, so they were layout
work over the common `InvoiceDocument`, not separate documents; the earlier
note that they were "not duplicates of the Printable Invoice" was wrong about
the document and right only about the stylesheet.  The class is now empty:
`Balance Sheet (eguile)` (48) is the only other template-shaped name and it is
closed because the native Balance Sheet covers it.

### 3.5 Closed (25 names)

"Closed" means a decision is recorded here and the name is **not** on the
backlog.  It is a different number from "unported".

| # | Report | Source | Reason recorded |
|---:|---|---|---|
| 20 | Account Summary | `standard/account-summary.scm` | The spec's `balance` is the raw signed amount, while the Scheme table sign-reverses credit-normal accounts (A/P `+585`, Equity `+10,000`); `account-tree` adds subtree rollups and a `Total X` row per parent; there is no Account Code column; the date presets only reach `all`/`this-year` instead of "today".  A built-in under the same name would show different amounts, so it is closed rather than approximated. |
| 21 | Income/Expense Chart | `standard/net-charts.scm` | The Scheme third series is Net Profit = income − expense.  The spec's `balance` is debit − credit = expense − income, the opposite sign, and there is no negated-balance series.  Income (credit) and Expense (debit) match, but the report as a whole does not. |
| 22 | Income & Expense Linechart | `standard/net-charts.scm` | Same Net Profit sign reason as 21. |
| 48 | Balance Sheet (eguile) | `standard/balsheet-eg.scm` | An eguile page that duplicates the native Balance Sheet.  The eguile template engine was not added; the native Balance Sheet is the report. |
| 43 | Average Balance | `example/average-balance.scm` | **The default price source has no native engine.**  The report is the mean of the *daily end-of-day balances* per interval (the balance at the next interval boundary is the previous interval's last day), with max/min over those daily balances and gain/loss/profit from the interval's splits converted at the next boundary; the chart plots that average (or profit, or the gain/loss pair).  That part is ordinary engine work.  What blocks a verbatim port is the report's default `weighted-average` exchange rate: `gnc:get-commoditylist-totalavg-prices()` builds a per-commodity cumulative `(date, price)` list from every split in the book whose transaction and account commodities differ, accumulating absolute `share`/`value` amounts and setting the price to the *exact rational* `tot-domestic/tot-foreign`; `gnc:exchange-by-pricealist-nearest` then multiplies the amount by the nearest date's price.  The native layer has only the pricedb latest/nearest conversions, no cumulative-average engine, and `gnc_numeric` cannot hold Scheme's exact arbitrary-precision intermediate ratio (it would round before the final multiply-and-round).  A port under another price source would print different multi-currency figures from the Scheme report; closed rather than approximated. |
| 44 | Balance Forecast | `standard/balance-forecast.scm` | **Needs a scheduled-transaction data path the native layer has never had, plus the price engine of 43.**  The renderer seeds an accumulator with `gnc-sx-all-instantiate-cashflow-all(earliest, from-date)`, then instantiates the SX cash flow again for each date interval and merges it with `gnc:account-get-balances-at-dates()`, then plots balance/minimum/target/reserve lines.  The only SX primitive that returns per-account cash flow is `gnc_sx_all_instantiate_cashflow_all()`, and it resolves the book through `gnc_get_current_book()` = `qof_session_get_book(gnc_get_current_session())`; `gnc-webd` opens its book with `qof_session_begin`/`qof_session_load` and never calls `gnc_set_current_session()`, so the helper sees a new empty book and returns no scheduled transactions.  The lower-level `gnc_sx_all_instantiate_cashflow(sx_list, ...)` *is* reachable and `gnc_book_get_schedxactions(book)->sx_list` is the right list, so this is a native-layer data-path gap that can be built, not an impossibility.  What makes it a closure today is that no book in the tree (`sample1`, `taxreport`, or any generated book) has a scheduled transaction, so a port could only ever be parity-tested on the empty case.  To reopen: add an SX fixture generator, then a native SX cash-flow path via the lower-level call. |
| 45 | Future Scheduled Transactions Summary | `standard/account-summary.scm` | **Same scheduled-transaction data-path gap as 44, plus an account-table engine the native layer does not have.**  The report is the Account Summary renderer with `sx? = #t`: each account's balance column is the SX cash-flow amount for that account over `[from, to]` (read by account GUID from the hash), and the layout is `gnc:html-acct-table` with its three depth-limit behaviours (summarize/flatten/truncate), parent-balance and subtotal modes, zero-balance modes, multi-commodity mode, rules and the optional code/type/description/notes columns.  The native `account_table()` has one rollup/subtotal mode and one balance function.  Same fixture gap as 44: no scheduled transaction exists in any book in the tree. |
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

The 25 closed names are 15 chart-model limitations (21-22, 49-59, 61, 65),
two demos (63-64), five other decisions (20 Account Summary, 48 Balance
Sheet (eguile), 60 Multicolumn View, 62 Register, 66 Tax Report / TXF Export),
and three engine gaps (43 Average Balance: the weighted-average price engine;
44 Balance Forecast and 45 Future Scheduled Transactions Summary: the
scheduled-transaction data path).  Reopening any of them is a new subsystem or
a changed report, not a report port.

### 3.6 Counts

| Class | Names | Share of 66 | Notes |
|---|---:|---:|---|
| native | 39 | 59.1% | Several names share a renderer; the three invoice layouts are one. |
| spec | 2 | 3.0% | The two net-worth charts, shipped as built-in specs. |
| logic | 0 | 0% | Cash Flow Barchart is native; see sections 3.1c and 15. |
| template | 0 | 0% | Easy and Fancy Invoice are native (section 14); the class is empty. |
| closed | 25 | 37.9% | 15 chart-model (the two income/expense names plus the 13 earlier), 2 demos, 5 other decisions (including Account Summary), 3 engine gaps (Average Balance, the two scheduled-transaction reports). |
| **total** | **66** | **100%** | 65 loaded per build because of the locale `taxtxf`. |

Native + spec = **41/66 = 62.1%**.  Native + spec + closed = **66/66 =
100%** done-or-decided.  The unported remainder is **0/66**.

Separate from the 66: **6 built-in designer specs** (Income vs Expenses by
Month, Expenses by Account, Income by Account, Net Worth by Month, Net Worth
Barchart, Net Worth Linechart).  The last two carry the Scheme names; the
first four are new reports.  None is in the 66 table or its percentages; they
are the `+6` in the `/api/reports` counts in section 1.1.

## 4. Coverage

### 4.1 By name

The headline is deliberately simple:

- **39 of 66 source names render natively: 39/66 = 59.1%.**  That is the
  number to use for "how much of the Scheme registry does the native layer
  replace".  Because the locale loader swaps the two tax reports, the per-build
  number is **39/65 = 60.0% in a US build** and **38/65 = 58.5% in a German
  build**: the German `Tax Report / TXF Export` replaces the US schedule, and
  the German report is closed.  The 39/66 source count includes both tax names,
  only one of which a build loads.
- **41 of 66 can be produced without new native code: 41/66 = 62.1%.**  Two
  more are spec-expressible, and they are the two net-worth charts (Net Worth
  Barchart and Net Worth Linechart), shipped as built-in specs under the Scheme
  names.  The three names the earlier measurement counted as spec-expressible
  (Account Summary, Income/Expense Chart, Income & Expense Linechart) are
  closed with the reasons in section 3.5, not approximated.
- **25 of 66 are closed: 25/66 = 37.9%.**  If closed decisions count as
  "covered", 66/66 = 100% is done-or-decided.
- **0 of 66 are unported.**  There is no backlog left: the last four logic
  names are the Cash Flow Barchart (native, section 15), Average Balance and
  the two scheduled-transaction reports (closed, section 3.5).

A Guile-free build's `/api/reports` has 45 entries (39 native + 6 built-in
specs).  That is a different numerator from "39/66 Scheme names": the six
built-ins are extra reports, not six more Scheme names (two of them do carry
Scheme names, but they are still not registry entries in the 66).  A Guile
build's `/api/reports` has 69 entries (65 loaded Scheme names + 4 built-in
specs; the other two built-ins replace their Scheme names in the list).  Do
not quote 44, 69 or 66 as if they were the same count.

### 4.2 By report class

| Report class | Names | Native | Spec | Logic | Template | Closed | Coverage verdict |
|---|---:|---:|---:|---:|---:|---:|---|
| Core statements (single-column) | 4 | 4 | 0 | 0 | 0 | 0 | **Full** (4/4). |
| Multicolumn statements | 2 | 2 | 0 | 0 | 0 | 0 | **Full** (2/2). |
| Equity statement | 1 | 1 | 0 | 0 | 0 | 0 | **Full** (1/1). |
| Entry listings | 5 | 5 | 0 | 0 | 0 | 0 | **Full** (5/5). |
| Aging | 2 | 2 | 0 | 0 | 0 | 0 | **Full** (2/2). |
| Budgets | 6 | 5 | 0 | 0 | 0 | 1 | **Full/decided** (5/6 native; Budget Chart closed). |
| Documents and eguile pages | 7 | 6 | 0 | 0 | 0 | 1 | **Full/decided** (6/7 native: the three invoice layouts, Receipt and both tax invoices; the eguile duplicate is closed). |
| Tax | 2 | 1 | 0 | 0 | 0 | 1 | **Full/decided** (US native, German closed). |
| Owner reports | 5 | 5 | 0 | 0 | 0 | 0 | **Full** (5/5). |
| Investments and lots | 5 | 5 | 0 | 0 | 0 | 0 | **Full** (5/5). |
| Charts | 16 | 0 | 2 | 0 | 0 | 14 | **Spec/decided** (2 spec: the net-worth pair; 14 closed). |
| Account summary | 1 | 0 | 0 | 0 | 0 | 1 | **Closed** (not expressible; see 3.5). |
| Cash flow | 2 | 2 | 0 | 0 | 0 | 0 | **Full** (2/2: Cash Flow and its barchart). |
| GST | 1 | 1 | 0 | 0 | 0 | 0 | **Full** (1/1; section 13). |
| Average balance | 1 | 0 | 0 | 0 | 0 | 1 | **Closed** (the weighted-average price engine; section 3.5). |
| Scheduled transactions | 2 | 0 | 0 | 0 | 0 | 2 | **Closed** (the SX data path; section 3.5). |
| Register | 1 | 0 | 0 | 0 | 0 | 1 | **Decided** (web route). |
| Multicolumn View container | 1 | 0 | 0 | 0 | 0 | 1 | **Decided** (loss: composite dashboards). |
| Demos | 2 | 0 | 0 | 0 | 0 | 2 | **Decided** (no accounting content). |
| **Total** | **66** | **39** | **2** | **0** | **0** | **25** | |

"Full" means every name in the class renders natively.  "Full/decided" means
the unported name is closed, so there is no backlog in that class.
"Spec/decided" means the chart class has two spec-expressible names (the
net-worth pair) and the rest are closed.  "Closed" means every name in the
class has a recorded decision.

The pattern is clear: **the core statements, the entry listings, aging,
budgets, the tax schedule, the owner reports, the six documents, the
investments/portfolio cluster and both cash-flow statements are fully covered;
the classes that still need a weighted-average price engine or a
scheduled-transaction engine are closed, not approximated.**  Charts are mostly
closed rather than ported, and after section 14 there is no document class left
outstanding at all.

### 4.3 The accounting core

"Accounting core" has to be defined, because the share changes with the
definition.  The earlier document used six names: Trial Balance, Balance Sheet,
Income Statement, Profit & Loss, Transaction Report, General Ledger.  Those six
are all native now:

| Core definition | Names | Native | Share |
|---|---:|---:|---:|
| Six-name core (the earlier document's set) | 6 | 6 | **6/6 = 100%** |
| Six-name core + General Journal (audit-trail listing) | 7 | 7 | **7/7 = 100%** |
| Six-name core + General Journal + Equity Statement + Cash Flow + both multicolumn statements | 11 | 11 | **11/11 = 100%** |

The earlier argument was that the six names "produce the accounting statements
and the transaction listing: what do I have, what did I earn, and what
happened?".  On that definition the core is fully covered, with five renderers
(Income Statement and P&L share one).  With this cluster the audit trail, the
two cross-tab statements and Cash Flow land too.  The document should still not
use the 100% without saying which definition it means.

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
- the Balance Sheet and Income Statement multicolumn forms and the Equity
  Statement, for a period-by-period read of the same books;
- Transaction Report and General Ledger;
- the five budget layouts;
- Receivable and Payable Aging, if they track money owed;
- Customer Report, Vendor Report, Employee Report, Job Report and Customer
  Summary, if they invoice customers or track a job;
- Investment Lots, Investment Portfolio and Advanced Portfolio for a
  brokerage account, and Lot Viewer for one account's lots;
- the six built-in chart specs (income vs expenses by month, expenses by
  account, income by account, net worth by month, net worth barchart, net
  worth linechart);
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
  Summary (all three closed with reasons: section 3.5);
- the German tax report/ELSTER export.

(Cash Flow, the Income and GST Statement, Easy and Fancy Invoice and the Cash
Flow Barchart were on this list at the 34-name measurement; all five are native
now -- see the addendum at the top and sections 14 and 15.)

For a personal user who mostly reads balances and income/expenses, the missing
items are analytics and portfolio reports, not the bookkeeping loop.  That is a
defensible Guile-free tier.  It is not full parity: the report list is 45
entries instead of 69, and a saved Scheme report with non-default options may
not replay.

### 5.2 A business user

A business user is a different answer.  A **US small business** that uses the
native Printable, Easy or Fancy Invoice (and, where applicable, the Receipt or
Tax Invoice), prints owner statements and files the US tax schedule with TXF
can be Guile-free **for reporting and document printing** today: those
documents and Tax Schedule Report/TXF Export are native, and the statements,
listings, aging, budgets, lots and owner reports are native.  The Australian
Tax Invoice is native as well, for books in that locale.

A business that needs any of the following **cannot**:

- **Statements:** none.  Cash Flow is native now, and so are the single-column
  Balance Sheet and P&L, their multicolumn forms and the Equity Statement.
- **Audit/listing:** none; General Journal, Reconciliation Report and Transaction
  Breakdown Report are native.
- **Tax:** Tax Report / TXF Export (German ELSTER).  The US schedule and the
  Income and GST Statement (all three of its formats) are native.
- **Documents:** none.  All six invoice/receipt documents are native; the three
  invoice.scm layouts are one renderer with three default stylesheets.
- **Container:** Multicolumn View.  Composite dashboards are a recorded loss.

For that business the web tier is not Guile-free for the reports it needs: the
only missing name is German tax.

### 5.3 How far is the gap?

**There is no unported name left.**  The last report stages added 31 names
(from the six core reports to 36, Easy/Fancy to 38, and the Cash Flow Barchart
to 39) in about eleven hours of commit-to-commit time (section 6); the three
names that would have been next are closed with the reasons in section 3.5
(Average Balance needs the weighted-average price engine; the two
scheduled-transaction reports need an SX data path, and no book in the tree has
a scheduled transaction to test it against).  The three closures are 2-4 days
of new subsystem work, not report ports.

**Full parity is still months**, but not because of a report backlog.  It is
months because the 25 closed names include new subsystems: pie, scatter,
stacked multi-series and per-account period-index charts for 15 names
(including Budget Chart); an ELSTER XML writer and German tax table for one; an
eguile template engine or a duplicate balance sheet for one; the
weighted-average price engine for Average Balance; the scheduled-transaction
engine for two names; and the Register/Multicolumn View/demo/Account Summary
decisions for the remaining five.  Those are not report ports.  If the product
requirement is "every Scheme report and every Scheme option", the honest
estimate is months.  If the requirement is "the reports a personal-finance user
or a narrow US small business needs", the native layer is already there.

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

### 6.3 What the closed names should cost

This is an extrapolation from the observed lines-per-name and minutes-per-name,
not a measurement of the remaining work.  The ranges widen where there is no
close precedent.

| Closed cluster | Names | Closest precedent | Projected source lines | Projected commit-time |
|---|---:|---|---:|---:|
| Cash flow barchart | 1 | cash flow | **done: section 15** | ~45 min |
| GST (Income and GST Statement) | 1 | tax | done: see section 13 | - |
| Easy/Fancy Invoice layouts | 2 | documents | done: see section 14 | - |
| Average Balance | 1 | table/chart + a price engine | 400-600 + price engine | 1 h + 1 day |
| Scheduled transactions (Future SX Summary, Balance Forecast) | 2 | SX | 1,600-2,400 + an SX fixture | 2-3 h + a fixture |
| **Total** | **0 unported** | | | |

The zero is the point: the two remaining clusters are **not report ports**.
Average Balance needs the Scheme weighted-average price engine built natively
(cumulative exact-rational prices over every split, then nearest-date
selection); the scheduled-transaction pair needs a native SX cash-flow path
(the lower-level `gnc_sx_all_instantiate_cashflow()` with the book's own
`sx_list`, because the `_all` wrapper resolves the wrong book in `gnc-webd`)
*and* an SX fixture generator, since no book in the tree has a scheduled
transaction.  Until those land, a port would render the empty case and
"pass" parity against nothing.  The 25 closed names are not in this table;
reopening the chart/template decisions is the months-scale work described in
5.3.

### 6.4 What the cost table is not

- It is not a schedule.  Commit wall clock is not effort; a commit can sit
  behind a review, a build or an unrelated fix.
- It is not option parity.  The native reports deliberately expose a subset;
  matching the Scheme option pages for the 39 native names is additional work
  not counted here (the invoice layouts are the exception: section 14 publishes
  the whole invoice.scm surface).
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
- **The chart model covers two Scheme chart names, in a three-series
  configuration.**  `builtin-net-worth-barchart` and
  `builtin-net-worth-linechart` are ports of the Scheme names; the other four
  built-ins are new reports.  The Income/Expense pair is closed on the Net
  Profit sign, and fifteen chart-family names (including Budget Chart) are
  closed because the model has no pie, scatter or stacked multi-series.  A
  "we have charts" claim is true for net worth and income/expense lines, not
  for the chart class.
- **"Closed" is 33% of the registry.**  That is a large number to leave out of
  a coverage claim.  If closed decisions count, 94% is done-or-decided; if they
  do not, 6% is backlog and 58% is native.  The same registry supports three
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
- **`/api/reports` is 69 in a Guile build and 44 in a Guile-free build.**
  Neither is 66.  The 66 is the source registry; a build loads 65 of those, and
  the 6 built-ins are extra; the 39 native names are a subset of the 66.
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

- It does not claim that 39/66 is a majority; it is (59.1%).  It does not present
  66/66 as "coverage" without saying that 25 of those are closed decisions.
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

Measured at `b4aa63f713` plus this cluster, which was 31 of 66; section 13
carries the next measurement (34) after the cross-tab cluster, and section 14
the current one (38) after the invoice layouts.  Section 3.1b
and section 4.1 count the four valuation names that landed at
`b4aa63f713`, and this cluster adds General Journal, Transaction Breakdown
Report and Reconciliation Report: **31 of 66 = 47.0% native** (31 of the 65 a
US build loads, 30 in a German build; 31/65 = 47.7% and 30/65 = 46.2%).
Native plus spec is 33/66 = 50.0%; native plus spec plus the 22 closed
decisions is 55/66 = 83.3%; the unported remainder is 11/66 = 16.7% (9 logic +
2 template).  A Guile-free build's `/api/reports` had 37 entries (31 native +
6 built-in specs) at that point.

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

## 13. Update: the multicolumn statements and the equity statement

Measured at `45b8a92ed3` plus this cluster.  The three cross-tab names are
native: `Balance Sheet (Multicolumn)` and `Income Statement (Multicolumn)`
(one renderer, `balsheet-pnl.scm`) and `Equity Statement`
(`equity-statement.scm`).  **34 of 66 = 51.5% native** (34 of the 65 a US build
loads, 33 in a German build; 34/65 = 52.3% and 33/65 = 50.8%).  Native plus
spec is 36/66 = 54.5%; native plus spec plus the 22 closed decisions is
58/66 = 87.9%; the unported remainder is 8/66 = 12.1% (6 logic + 2 template).
A Guile-free build's `/api/reports` has 40 entries (34 native + 6 built-in
specs).  *(These are this stage's numbers; sections 14 and 15 raise them to
39 native and 0 unported.)*

- **The shape of the Scheme reports.**  `balsheet-pnl.scm` is one renderer with
  a report-type switch.  Its columns are a date list: the balance sheet's
  columns are period *ends* (with "Period duration" disabled, the single end
  date; otherwise `gnc:make-date-list` from the start date's end of day, so a
  yearly run over 2018-2019 prints 01/01/18, 01/01/19 and 12/31/19), and the
  profit & loss's are period *intervals* between those dates (the activity is
  the difference of the two end-of-period balances).  Each cell is the running
  balance the Scheme engine's `gnc:account-accumulate-at-dates` reads: the
  balance sheet includes closing entries, the profit & loss leaves them out
  (`xaccSplitGetNoclosingBalance`).  Section titles, "Total For X" rows, a
  parent's subtree row plus its own row (the "Parent account amounts include
  children" default), and the income/expense balances as one `Retained
  Earnings` row inside equity make up the rows.  The equity statement is a
  different shape: an opening capital as of the end of the day before the
  start date, the period's net income, investments and withdrawals, the
  increase in capital, and a closing capital, with unrealised gains (which need
  the average-cost price source) omitted when zero.
- **What the native layer reuses.**  The account buckets are `bucket_of()`,
  the same mapping `gnc:decompose-accountlist` uses, so the sections and their
  signs are the existing machinery's.  The report's account list is
  `collect_accounts()` (the engine's sorted descendant order), the balances are
  `gnc_account_foreach_split_between_dates()` with the end date at the period
  end, the report currency and per-date conversion are `report_currency()` and
  `xaccAccountConvertBalanceToCurrencyAsOfDate()`, the date axis is the
  `GDate` helpers the spec's period code already uses, and the equity
  statement's closing-entry test is the same `xaccTransGetIsClosingTxn()` the
  pnl uses.  Nothing new was needed for the row model beyond the column axis
  (next point).
- **One model addition, not a special case.**  A `Table` can now carry a
  grouped amount axis: `Table::column_headings` (one heading per amount
  column) plus `Table::row_heading` (the "Date"/"Period" corner), and each `Row`
  carries one `values` entry per heading.  The renderer emits a heading row and
  one amount cell per heading, keeping the Scheme cell classes so the parity
  harness reads both documents the same way.  The single-amount form is
  untouched: a table with no column headings renders exactly as before.  The
  designers' spec renderer could use the axis as is, which is why it lives in
  the model and not in the two reports.
- **Parity, defaults and two period configurations.**  `report-parity.sh`
  gained the `multicolumn` extractor (label, then every non-empty cell, amounts
  as numbers and everything else as text, so the column headings are compared
  too), the three report GUIDs and the `period-duration` mapping (the route's
  name translated to the engine's `'YearDelta`/`'MonthDelta`/... symbol).  On
  `sample1` every configuration below matches verbatim in both builds:
  `Balance Sheet (Multicolumn)`, defaults: 21 rows matched; monthly 2018:
  22 rows matched; yearly 2018-2019: 22 rows matched.  `Income Statement
  (Multicolumn)`, defaults: 97 rows matched; monthly 2018: 98 rows matched;
  yearly 2018: 98 rows matched.  `Equity Statement`, defaults and 2018: 6 rows
  matched each.  The two multicolumn reports were also run quarterly and with
  `reverse-chrono=false` (both 22 rows matched), and the profit & loss with
  `include-overall-period=true` (98 rows matched).  On `taxreport.gnucash` the
  Equity Statement matches (2000: 6 rows).  The two multicolumn reports do
  **not** match on `taxreport`, and the reason is single and precise: that book
  has two income accounts denominated in fund commodities (`GE S&S HP`,
  `Fid Magellan Fund`), so the Scheme report prints one line per commodity
  inside a cell -- including a zero line for each fund -- while the native
  table prints one amount per cell, converted into the report currency.  The
  numbers agree: the three differing rows are
  `Retained Earnings 0. GE S&S HP0. Fid Magellan Fund$513.03` versus `$513.03`,
  `Total For Equity ... $3,613.03` versus `$3,613.03`, and `Total For Liability
  and Equity ... $3,613.03` versus `$3,613.03` (monthly 2000: `$699.03`,
  `$3,799.03`, `$3,799.03`).  The same conversion is what every other native
  report does, so making these two per-commodity would make them the only
  reports in the layer that do not agree with the report currency; the
  difference is recorded here rather than hidden.
- **Options.**  Both multicolumn reports publish `start`, `end`,
  `period-duration` (`disabled`/`year`/`half-year`/`quarter`/`month`/
  `two-weeks`/`week`, default `disabled`), `reverse-chrono`, `show-zb-accts`,
  `account-full-name`, `label-sections` and `total-sections`, plus
  `include-overall-period` on the profit & loss.  Not published, with reasons
  in the descriptors: the account selection (the Scheme default is every
  account in the book, which is what renders; a selection identical to the
  default would be an option with no effect), the dual-column layout (a
  presentation of the same rows in the same order, which the native renderer
  emits as one table), the amount indenting and omit-zero-balances switches,
  the non-recursive parent mode, the "Common Currency" page (the native table
  always converts), the exchange-rate rows, the chart and the links.  The
  equity statement publishes `start`, `end` and `report-title`; its currency
  page, accounting-style rules and closing-entry pattern (default "Closing
  Entries") are not published.  No accounting-period preset is published for
  any of the three: each needs an end date, so "all dates" has no meaning.
  The equity statement's title option is `report-title`, not `title`, because
  the route's `title` is the invoice documents' heading.
- **Tests.**  `report-options.sh` gained the three descriptor sets, their 400s
  (unknown `period-duration`, `accounts` on a multicolumn report, a bad
  boolean, an unknown option on the equity statement), the positive period-
  column checks (the "12/31/18"/"01/01/18" headings, the absent heading row in
  the single-column form), the overall-period comparison and the title.  Its
  generic "every advertised option changes the report" probe now gives the two
  multicolumn reports a monthly baseline, because their switches act on the
  column axis and a one-column page makes them look inert; that is an extension
  of the test, not a loosening of it -- each one is still compared against a
  rendered page.  The smoke test's "non-native report" example moved from
  `Income Statement (Multicolumn)`, which is native now, to `Cash Flow`, and
  then to `Average Balance`, because Cash Flow is native now too.
- **The matrix.**  56 cases per build: every native name the two fixture books
  can serve with its default options (28 names -- the six invoice documents
  and the four owner reports need their own fixtures and are exercised by the
  harness's document/owner runs), on `sample1` and `taxreport.gnucash`, in
  `ext4-web` and `ext4-noguile`.  **54 pass in both builds.**  The 2 that do not
  are the two `taxreport` multicolumn cases recorded above, where the Scheme
  report lays out one amount per commodity in a cell and the native table
  converts into the report currency -- the documented multi-currency gap, not a
  regression.  The five empty-book cases this section used to list are closed:
  `Investment Portfolio` now prints its headings with no rows, `Advanced
  Portfolio` its headings plus the zero-valued Total row, and `Customer Summary`
  says "No valid customer found.", each matching the Scheme page; the
  Advanced Portfolio's one-cent rate-of-return rounding (30.64 versus 30.65) is
  fixed by the Guile-compatible formatter described in the addendum at the top.
- **Build hygiene.**  No new files: the reports live in the existing
  `gnc-report.cpp`/`.hpp` (and the Customer Summary's empty-owner-list case in
  `gnc-owner.cpp`), so `set_dist_list` is unchanged.  Both builds compile with
  `-Werror`; `ldd` on the Guile-free `gnc-webd` has no `libguile`, `libgtk` or
  `libwebkit` dependency (the Guile build links `libguile` by design).

## 14. Update: Easy Invoice and Fancy Invoice, and the invoice option surface

*(Stage record: this section's counts are the ones in force when it was
written.  Section 15 supersedes them; the current counts are in the addendum at
the top and in section 15.)*

This stage closed the last two template-class names, and the class is now
**empty**.  **38 of 66 = 57.6% native** (38 of the 65 a US build loads, 37 in
a German build; 38/65 = 58.5% and 37/65 = 56.9%).  Native plus spec is
40/66 = 60.6%; native plus spec plus the 22 closed decisions is 62/66 = 93.9%;
the unported remainder is 4/66 = 6.1%, all logic.  A Guile-free build's
`/api/reports` has **44** entries (38 native + 6 built-in specs); a Guile build
still has 69.

- **What invoice.scm actually is.**  `standard/invoice.scm` defines three
  reports -- Printable, Easy and Fancy Invoice -- from **one** renderer
  (`reg-renderer`) and **one** options-generator.  Their `variant-list` entries
  differ in exactly two defaults: the "CSS" text and "Row 3 Right".  All three
  print the same document: the title row, the six header slots (Row 1/2/3 Left
  and Right), the entry table with its nine optional columns and the
  minimum-entry padding, the detailed-or-single tax row, the payment rows, the
  amount due, the notes, the payable-to and company-contact footers and the
  extra notes.  Easy sets Row 3 Right to `today` (the print date) and appends
  its layout rules to the CSS; Fancy keeps `today` off and appends the two
  `x-large` name rules.  So the answer to "layout only, or extra rows/columns?"
  is **layout only**: no logo row, no per-line notes, no comment block and no
  column exists in Easy or Fancy that the Printable Invoice lacks; the
  "Minimum # of entries" option the Fancy layout introduced is on the shared
  entry table, so it is published for all three.
- **One renderer, three entry points.**  `gnc-invoice-print.cpp` now renders
  the Scheme layout once (`render_invoice()`) and the three registrations are
  `render_printable_invoice()`, `render_easy_invoice()` and
  `render_fancy_invoice()`: each sets `InvoicePrintOptions::layout`, which
  selects the two variant defaults through `invoice_default_css()` and
  `invoice_default_slot()`.  That is the same structure as the Scheme file, so
  a variant cannot drift from the others.  No new file: the existing
  `gnc-invoice-print.cpp` grew the layout page, so `set_dist_list` in
  `libgnucash/reports/CMakeLists.txt` is unchanged.
- **What the model gained, and what stayed an option.**  `InvoiceDocument`
  gained `references` (the orders' references the client table prints as
  `REF <reference>`, built with the fold and reversal invoice.scm uses) and
  `DocumentOwner::tax_number` (the company table's company-tax-nr row, the
  book's Tax / Tax Number option).  Everything else the three layouts print was
  already in the model or is *report option state* in Scheme too -- the CSS,
  the picture location, the print date (the current date, not invoice data), the
  payable-to and contact phrases and the header slots -- so it lives in
  `InvoicePrintOptions`, not in the engine reader.
- **The published option surface.**  All three names publish invoice.scm's
  pages: `invoice`, `title`, `css`, `picture`, the six `row1_left` …
  `row3_right` choices (`none`, `client`, `company`, `invoice`, `today`,
  `picture`), `minimum_entries` (0-23), the nine `show_*` column switches, and
  the display switches `show_due_date`, `show_subtotal`, `show_billing_id`,
  `show_owner_id`, `show_notes`, `show_terms`, `show_payments`,
  `show_references`, `show_detailed_tax`, `show_job_details`, `show_payable_to`,
  `show_company_contact`, plus the three texts `payable_to_string`,
  `company_contact_string` and `extra_notes`.  The route validates names
  against those descriptors (400 "unknown option '<name>' for report '<name>'"),
  a choice against its list (400 "invalid row1_left 'zzz'") and a number
  against its range, so an unsupported name can never be silently ignored.
  The CSS default is the variant's own text, so `/api/report/Easy%20Invoice/options`
  and the Fancy one differ in that descriptor while the rest is identical.
- **Parity: 12 default cases plus 12 switch cases, both builds, verbatim.**
  `report-parity.sh` gained the two names (the `invoice` extractor and the two
  Scheme GUIDs, Easy `67112f318bef4fc496bdc27d106bbda4`, Fancy
  `3ce293441e894423a2425d7a22dd1ac6`), and each layout was compared on the
  customer invoice INV-1001 and the vendor bill BILL-2001 in `ext4-web` and
  `ext4-noguile`.  The extracts agree row for row; INV-1001 (8 rows, all three
  layouts):

  ```
  ('ITEM', 'Consulting', Decimal('2.00'), Decimal('150.00'), Decimal('300.00'))
  ('ITEM', 'Software licence', Decimal('1.00'), Decimal('99.99'), Decimal('99.99'))
  ('ITEM', 'Support', Decimal('3.00'), Decimal('20.00'), Decimal('54.00'))
  ('TOTAL', 'Net Price', (Decimal('453.99'),))
  ('TOTAL', 'Tax', (Decimal('35.40'),))
  ('TOTAL', 'Total Price', (Decimal('489.39'),))
  ('PAYMENT', (Decimal('-200.00'),))
  ('TOTAL', 'Amount Due', (Decimal('289.39'),))
  ```

  BILL-2001 (6 rows, all three layouts):

  ```
  ('ITEM', 'Office chairs', Decimal('4.00'), Decimal('120.00'), Decimal('480.00'))
  ('TOTAL', 'Net Price', (Decimal('480.00'),))
  ('TOTAL', 'Tax', (Decimal('0.00'),))
  ('TOTAL', 'Total Price', (Decimal('480.00'),))
  ('PAYMENT', (Decimal('-100.00'),))
  ('TOTAL', 'Amount Due', (Decimal('380.00'),))
  ```

  The extractor compares line items, payments and totals as numbers, so it does
  not by itself prove the layouts differ; the variant markers do, and they match
  the Scheme documents: Printable and Easy print one `invoice-print-date` row and
  Fancy prints none, the Easy page carries `min-width: 600px` and the base one
  does not, and the Fancy page carries `font-size: x-large` and the Easy one
  does not.  Checked on both engines' HTML for all three documents, on
  INV-1001.  Twelve further cases pin the display switches through
  `report-parity.sh`'s new invoice-option mapping: `show_total=false` (which
  also exercises the Scheme `display-subtotal` rule that hides the sign when the
  Total column is off) and `show_tax_amount=true&show_notes=true`, for each
  layout on INV-1001, in both builds -- all 12 match.  The mapping is the
  harness's, not the report's: it only translates the switch names onto
  invoice.scm's option pages; the layout-page choices, the CSS and the picture
  are exercised by `report-options.sh` instead, because their Scheme values need
  a symbol/path conversion the harness does not need for a switch.
- **Nothing was left unreproduced.**  There is no template language in the
  path: the six layout slots, the picture, the print date, the padding rows,
  the detailed tax summary, the references, the job rows and the three footers
  are all printed by `render_invoice()`.  The one thing that differs from the
  Scheme page is markup spelling (the Scheme HTML says `<br />` and wraps
  tables in `<tbody>`, the native page uses its own chrome stylesheet), which is
  not content and is not compared.
- **Tests and build hygiene.**  `report-options.sh` grew from 836 to **910
  checks** (74 new): the two new descriptor sets, their 400s (start/accounts/period/
  unknown name/bad boolean/bad layout slot/empty and unknown invoice), the two
  variant-defining checks (Easy's rule and print date, Fancy's rule and its
  missing print date), a custom-CSS replacement and a picture slot, and generic
  probes for the new choice, text and number options (the six layout slots,
  `css`, `picture`, `extra_notes`, `minimum_entries`), with the descriptor-
  documented no-ops (`show_terms`, `show_payments`, `show_references`,
  `show_job_details`, `show_payable_to`, `show_company_contact` and the two
  phrase texts) accepted only because their descriptors say they have no effect
  on that book.  The smoke test checks the two names in the report list, the
  Easy stylesheet and print date and the Fancy stylesheet and absent print date.
  `report-parity.sh`, `report-options.sh`, `report-spec.sh` and
  `run-smoke-test.sh` pass in both builds; the default parity matrix is 54 of 56
  per build, the same two documented `taxreport` multi-currency multicolumn
  cases as before this stage.  Both builds compile with `-Werror`.  `ldd` on
  `ext4-noguile`'s `gnc-webd` has no `libguile`, `libgtk` or `libwebkit`; the
  `ext4-web` binary links `libguile` because it is the Scheme-enabled build, and
  neither links GTK or WebKit.  No new files were added, so the distribution
  lists are unchanged.

## 15. Update: the Cash Flow Barchart, and the last three names closed

This stage implemented the last cash-flow name and closed the three names that
could not be ported verbatim on the native layer as it stands.  The current
counts are: **39 of 66 = 59.1% native** (39 of the 65 a US build loads, 38 in a
German build; 39/65 = 60.0% and 38/65 = 58.5%).  Native plus spec is
41/66 = 62.1%; native plus spec plus the **25** closed decisions is
66/66 = 100%; **the unported remainder is 0/66**.  A Guile-free build's
`/api/reports` has **45** entries (39 native + 6 built-in specs); a Guile build
still has 69.

- **The barchart is the Cash Flow calculation, not a second implementation.**
  `cashflow-barchart.scm` calls `cash-flow-calc-money-in-out()` once per date
  interval and sums the resulting collectors into the report currency.  The
  native layer extracted the classification out of `cash_flow()` into
  `cash_flow_flows()` (the same selected-account triggers, counterpart rule,
  per-transaction dedup, absolute value and transaction-date conversion,
  trading-account rule and bucket sort), and `cash_flow()` and
  `cash_flow_barchart()` both call it.  A divergence between the statement and
  the chart is now impossible by construction; the statement's own parity case
  is unchanged and still passes.
- **Interval semantics are `gnc:make-date-interval-list()`.**  The native
  `make_date_intervals()` reproduces all seven step sizes the Scheme "Step
  Size" option offers.  Month/quarter/half-year/year steps count from the start
  date through `incdate-months()` semantics (a month-end start keeps every
  boundary a month end), day/week/two-week steps step by days, each interval
  ends one second before the next boundary except the last, which is clamped
  to the end date, and the label is the interval start as `qof-print-date`.
  The default is one month.  A first interval at the start's time of day and a
  final partial interval are both covered by the parity runs.
- **The chart and the table.**  The chart is the Scheme's own: a bar chart with
  one bar per interval and the series the display switches ask for, in the
  Scheme order (Money In, Money Out, Net Flow), with the currency attached.
  The "no data" case is the Scheme test: `gnc:not-all-zeros` runs over all
  three computed lists, so a period whose visible series are empty but whose
  hidden series are not still prints the chart, and a wholly zero period
  prints the empty-data warning instead of a chart or table.  "Show Table"
  prints the Scheme "Overview:" table (interval start, plotted series, Total
  row).  The interval rows and the chart share the same `Table`, so the
  renderer emits the data block a reader or a test can parse without running
  JavaScript.
- **The option surface.**  `start`, `end`, `currency`, `accounts`,
  `step-size` (day/week/two-weeks/month/quarter/half-year/year), `show-in`,
  `show-out`, `show-net`, `show-table` and `include-trading`.  The price source
  is deliberately not published: the native conversion is the Scheme option's
  default (`pricedb-nearest`), exactly as the Cash Flow statement documents it.
  The route rejects a value outside the step-size list with a 400 that names
  it.
- **Parity: the chart extractor, both builds, both fixtures.**  The harness
  gained the name (GUID `5426e4d987f6444387fe70880e5b28a0`), the `chart`
  extractor and the `[options]` mapping for the step size and the display
  switches.  The Scheme embedded chart data and the native
  `report-chart-data` block agree on the chart type, the label count and every
  plotted value.  Cases actually run: `sample1` 2018 and `taxreport` 2000 with
  default options in `ext4-web` and `ext4-noguile` (4 cases), the six non-month
  step sizes on `sample1` 2018, a `taxreport` range running 2000-03-15 to
  2000-09-20 (partial first and last months) and a year step on `taxreport`
  2000.  All match verbatim.  The sample1 2018 series are real, not
  placeholders: Money In `(0, 10000, 0, …, 0)` (the February opening balance
  transaction), Money Out all zero, Net Flow the same positive series.
- **The three closures.**  `Average Balance` (43), `Balance Forecast` (44) and
  `Future Scheduled Transactions Summary` (45) are closed with the reasons in
  section 3.5.  In short: Average Balance's *default* price source is the
  Scheme weighted-average engine (`gnc:get-commoditylist-totalavg-prices()`
  builds cumulative exact-rational prices from every split in the book, and
  `gnc:exchange-by-pricealist-nearest` picks the nearest date's price); the
  native layer has only pricedb latest/nearest and `gnc_numeric` cannot hold
  the exact intermediate ratio.  The two scheduled-transaction reports need a
  native SX data path: the only per-account cash-flow primitive,
  `gnc_sx_all_instantiate_cashflow_all()`, resolves the book through the global
  current session, which `gnc-webd` never sets, so it sees a new empty book;
  the lower-level `gnc_sx_all_instantiate_cashflow(sx_list, …)` is reachable,
  but no book in the tree has a scheduled transaction, so a port could only be
  parity-tested on the empty case.  Both reasons name what to build next, not
  an impossibility.
- **Tests and build hygiene.**  `report-options.sh` grew from 910 to **941
  checks** (31 new): the descriptor set, the step-size choice list and default,
  the 400s (unknown step size, period preset), the default "no data" page, the
  12 monthly bars and the quarter re-bucketing on 2018, `show-in=false`
  dropping the series, `show-table=true` printing the Date/Money In headings
  and `include-trading` accepted.  The generic "every advertised option changes
  the report" probe skips the name for the same reason it skips Cash Flow: the
  default period is the accounting period, not this book's 2018 activity.  The
  default parity matrix is now **56 of 58 per build** (the previous 54 of 56
  plus the barchart's two fixture cases), and the step-size/partial-month cases
  above add 9 more.  No new files: the report lives in the existing
  `gnc-report.cpp`/`.hpp`, so `set_dist_list` is unchanged.  Both builds
  compile with `-Werror`; `ldd` on `ext4-noguile`'s `gnc-webd` still has no
  `libguile`, `libgtk` or `libwebkit`; `run-smoke-test.sh`, `report-spec.sh`
  and `report-options.sh` pass in both builds.
