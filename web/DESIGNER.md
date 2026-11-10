# Report designer — saved report specs

The designer builds a report from the same query, column and grouping
vocabulary the native reports use, previews it, and saves it under a name.
Saved reports live **outside the book**: the web tier opens the `.gnucash`
file read-only by design, so a saved report is a JSON document kept beside the
web UI's data.

## Where saved reports live

`gnc-webd` resolves the store in this order:

1. `$GNC_WEBUI_REPORTS`, when set. The tests point it at a temporary file.
2. `$XDG_DATA_HOME/gnucash/webui/reports.json`.
3. `$HOME/.local/share/gnucash/webui/reports.json`.

The file is created on the first save. It is written atomically (a temporary
file in the same directory, then `rename`), so a crash cannot leave a half
written store. An empty file is an empty list; malformed JSON is an error and
is never silently discarded.

```json
{
  "version": "1",
  "specs": [
    {
      "id": "8c3a…",
      "name": "Expenses by month",
      "title": "Expenses by month",
      "description": "Scoped to Expense accounts; one bar per month.",
      "source": "accounts",
      "columns": ["account-name", "debit"],
      "grouping": "period",
      "period_unit": "month",
      "period_mode": "activity",
      "start": "2018-01-01",
      "end": "2018-12-31",
      "date_preset": "",
      "currency": "USD",
      "account_types": ["EXPENSE"],
      "accounts": [],
      "chart": "bar",
      "chart_series": ["debit"]
    }
  ]
}
```

## Spec fields

| field | meaning |
|---|---|
| `name` | required, unique, at most 80 characters; this is the report-list label |
| `title` | optional, at most 120 characters; defaults to `name` |
| `description` | optional, at most 240 characters; shown in the report list and when a built-in is opened |
| `tags` | optional array of up to 12 slugs: `[a-z0-9-]`, 1–24 characters each. Leading/trailing whitespace is trimmed and duplicates are dropped, in order. Tags are stored with the spec (so every browser on the book sees them) and drive the `?tag=`/`?q=` list filters. |
| `columns` | required non-empty array from `account-name`, `debit`, `credit`, `balance`, `subtotal`; no duplicates |
| `grouping` | `none` (one flat row per account), `account-tree` (rollups plus a subtotal per parent) or `period` (one row per year or month) |
| `period_unit` | `year` or `month`; used by `grouping: "period"` |
| `period_mode` | `activity` (the default: the period's debits and credits) or `cumulative` (the balance at the period's end, accumulated from the start of the book); `cumulative` needs `grouping: "period"` |
| `start`, `end` | optional `YYYY-MM-DD`; blank means unbounded on that side |
| `date_preset` | `this-year`, `last-year` or `all`; mutually exclusive with `start`/`end` |
| `currency` | optional commodity mnemonic; blank means the report default |
| `accounts` | array of account GUIDs; each means that account and its subtree; empty means the whole book |
| `account_types` | array of `BANK`, `CASH`, `CREDIT`, `ASSET`, `LIABILITY`, `STOCK`, `MUTUAL`, `CURRENCY`, `INCOME`, `EXPENSE`, `EQUITY`, `RECEIVABLE`, `PAYABLE` or `TRADING`; empty means every type. This is what makes a spec portable between books: a GUID means one book, a type means every book. When both are set, an account must match both. |
| `chart` | `""`/absent, `bar` or `line`; the chart section is rendered below the table, and the data is embedded in the HTML |
| `chart_series` | one or two of `debit`, `credit` and `balance`, in series order; each must also appear in `columns`. Absent means the columns decide: `debit` and `credit` when both are shown, otherwise `balance` or the one amount column. |

That is the `accounts` source's field set.  `columns`, `grouping`,
`period_unit`, `period_mode`, `account_types`, `chart` and `chart_series` are
the account vocabulary; the other sources accept only the fields their row set
has, and the form hides the rest instead of sending a field the server would
refuse (see **The source-aware form** below).  A spec's `source` is part of its
document: it is what makes a balance-sheet spec a balance sheet rather than an
account list, and a fork keeps it.

A `period`-grouped report needs concrete bounds: literal `start` and `end`,
a preset that resolves to them (`this-year` or `last-year`), or
`date_preset: "all"`. With `all` the renderer takes the range from the scoped
transactions, so a built-in period report needs no dates that fit only one
book.

## Sources

`source` says where a spec's rows come from. The default (`accounts`) is the
account/period table the fields above describe; the other sources are the
native reports whose own table a spec can be, so a spec can express a report
the account vocabulary cannot. Each source publishes its own column vocabulary
and option set from `/api/spec/options`, and the validator enforces the same
lists: a fixed source's columns are the report's fields, not a choice, while a
split listing chooses from the split vocabulary. A field of another source is
refused by name.

| source | rows | columns | options |
|---|---|---|---|
| `accounts` | one per account or period, from `columns`, `grouping` and the date range | `account-name`, `debit`, `credit`, `balance`, `subtotal` | all of the fields above |
| `splits` | one per split of the scoped accounts; `split_grouping` orders and sections them (Transaction Report, General Ledger, General Journal) | `date`, `num`, `description`, `memo`, `transfer`, `account`, `reconcile`, `debit`, `credit`, `amount`, `running-balance` | `split_grouping`, dates, `accounts` |
| `balance-sheet` | the Balance Sheet's own: Assets, Liabilities, Equity, one row per account with the report's signs and its Retained Earnings line | `account-name`, `balance` | `end`, `currency` |
| `invoices` | the aging reports' owner rows (the six buckets and the row total), or one row per document the buckets are made of | the owner set or the document set | `end`, `invoice_owner`, `invoice_grouping`, `bucket_basis` |
| `lots` | the Lot Viewer's: one row per transaction, one column per lot it touches | `date`, `description`, `type`, `lots` (the book's lot columns), `other-account` | `start`, `end`, `accounts` (one), `description_filter` |
| `budgets` | the Budget Report's: the account tree and one Bgt/Act pair per budget period | `account`, `budget`, `actual` (the repeating pair) | `accounts` |
| `tax-codes` | the US Tax Schedule's: one row per tax code with the amount the schedule totals for it | `form`, `line`, `code`, `amount` | `start`, `end`, `accounts`, `currency` |
| `cash-flow` | the Cash Flow statement's: its two sections, one row per contributing account, Money In, Money Out and Difference | `account`, `money-in`, `money-out`, `net` | `start`, `end`, `accounts`, `currency` |
| `document` | none: a document is not a table | — | `document`, `invoice`, `document_options` |

The tax-codes rows are the schedule's own code totals: `gnc-tax.cpp` collects
the tax-related accounts' entries once and both the report and
`tax_codes_spec_table()` group them by the same (form, line, code) key, so the
spec cannot disagree with the schedule about a code's amount. It does not
carry the report's per-account rows or its form/line headings, which is why the
designer calls it approximate.

The cash-flow rows are the statement's own layout, factored into one
`cash_flow_rows()` list that both `cash_flow()` (which formats the cells) and
`cash_flow_spec_table()` (which builds typed ones) project, over the same
`cash_flow_flows()` classification the Cash Flow Barchart plots. A row carries
one figure, in the column for its side, which is the one amount the statement
prints; the "Selected Accounts" list above the table is page furniture, not a
cell. An empty date is the report's default period, not "no bound".

A **document spec** is not a table spec: it holds `document` (one of Printable
Invoice, Easy Invoice, Fancy Invoice, Receipt, Tax Invoice or Australian Tax
Invoice), `invoice` (the document to print, by id or GUID; empty is the
report's "no invoice selected" page) and `document_options` (that report's own
print options by the names its descriptors publish, e.g. `title`, `css`,
`show_date`, `minimum_entries`). It renders through the document renderer —
`main.cpp` turns the fields into the route's query parameters and calls the
same `build_report_request()`/`render_report()` the `/report/<document>` route
uses — so the same invoice and options print the same bytes. A name the report
does not publish, a value of the wrong type and an unknown invoice are 400s;
`columns`, `grouping`, dates, `currency`, `accounts`, `account_types` and
`chart` are refused by name.

## The source-aware form

The designer form begins with a **Source** picker built from the `sources` list
in `/api/spec/options`.  Choosing a source shows only the controls that source
accepts and clears the ones it does not, so a value from the previous source is
never sent; the server's named 400 for a foreign field remains the backstop for
a hand-written request.  A source whose columns are the report's own shows no
column group at all: the spec omits `columns` and the validator fills in the
report's list.

| source | fields the form shows |
|---|---|
| `accounts` | name, title, tags, columns, grouping, period length/rows, accounts, account types, dates or preset, currency, chart and chart series |
| `splits` | split columns, split grouping, accounts and the two dates; with a `transaction` grouping the listing is the General Journal's whole book, so the form also hides the dates and the account selection |
| `balance-sheet` | end date and currency |
| `invoices` | owner (receivable/payable), rows (owner/document), bucket basis and end date; the columns are the row set's own and the form supplies them because the server requires a non-empty list |
| `lots` | one account (or none, the whole book), the two dates and the description filter |
| `budgets` | accounts |
| `tax-codes` | dates, accounts and currency |
| `cash-flow` | dates, accounts and currency |
| `document` | the layout, the invoice id or GUID and that report's own print options; a print option left at its descriptor's default is omitted, so the report's own default document prints |

Every source brings its own defaults when it is chosen: the account source a
flat `account-name` column, the split source Date/Description/Account/Amount,
the invoice source the aging report's owner rows, the document source the first
document layout and every print option at its default.  That is what makes
Preview work from a cold form for every source, including the fixed-column
sources the old form could not preview because no account column was ticked.

A populated report moves the picker to the source its spec carries, then fills
only that source's controls.  A fork therefore keeps the source, and a document
fork keeps its layout, invoice and print options.  A `source` omitted from a
document reads as `accounts`, which is what keeps a store written before this
stage loadable.

## Tags and the list filters

Tags are part of the spec document, not the UI's `localStorage` saved views:
a saved report's tags are in the same file as its query, so they survive a
reload and every browser pointed at that book sees the same tags. An invalid
tag is a `400` whose body names it (`invalid tag 'Net Worth'`); trimming
happens before validation, so `" spending "` is the tag `spending`.

`GET /api/specs` accepts two optional filters, which compose (AND) and apply
to the saved and the built-in specs alike:

| query | meaning |
|---|---|
| `tag=<slug>` | keep the specs carrying that tag (exact, case-insensitive); an unknown tag is `{"specs":[],"builtin":[]}`, not an error |
| `q=<text>` | keep the specs whose `name`, `description` or one of whose tags contains the text (case-insensitive) |

The report list in the browser mirrors this client-side: the search box
narrows the rendered list by name or tag as you type, and each tag on a
built-in or saved entry is a chip that filters the list to that tag (clicking
it again clears the filter). The designer's tag field is comma or space
separated and offers the tags already in use as suggestions.

## How a chart consumes the table

The labels are the table's own rows: the periods of a `period` report, the
top-level section subtotals of an `account-tree` report, or the non-zero
accounts of a flat report (at most twelve). A chart series is an amount
column of those rows: `balance` is the row's signed amount, `debit` its
positive side and `credit` its negative side shown as a positive number.
Because a period row keeps its two sides separately, the Debit and Credit
series agree with the period table's Debit and Credit columns even when a
period contains both income and expenses; a net amount alone could not say
that.

The combinations that cannot mean anything are 400s before rendering, each
naming the field: a chart with no amount column, a series that is not in
`columns`, `account-name` or `subtotal` as a series, a repeated series, more
than three series, `balance` mixed with only one of `debit`/`credit` (a net
and its own part), the three-series chart on a flat or account-tree report (a
row's balance is not a period net), `chart_series` without `chart`, and
`period_mode: "cumulative"` without period grouping. An unknown chart type,
account type, grouping or period unit is rejected the same way.

On a `period` report the three amount columns are one period's two sides and
its net, so `debit`, `credit` and `balance` may be drawn together.  That is
the Income / Expense / Net Profit shape, and on a cumulative asset-and-
liability scope it is the Assets / Liabilities / Net Worth shape.  The series
still carry the amount-column names ("Debit", "Credit", "Balance"), not the
Scheme report's labels.

## What the designer shows when a report is selected

Selecting a report fills the designer with **what the report actually is**: the
source picker moves to the spec's `source` and only that source's controls are
populated, and the banner above the form says which case is on screen:

- **Built-in and saved specs are definitions.** Every designer field is
  populated from the spec `GET /api/specs/<id>` serves (name, title, tags,
  source, columns in their printed order, grouping, period unit and mode,
  accounts, account types, chart and chart series in series order, dates or
  preset, and currency). The button reads **Save as…** and the form refuses to
  save under the selected report's own name: saving always forks a copy under a
  new name. A built-in is code, so it can be neither edited nor deleted this
  way. The copy is the same document as the original: `report-spec.sh` and
  `ui-test.mjs` compare the fork's render with the built-in's byte for byte
  (cell by cell as the UI harness's fallback). The banner also carries the
  second line `Source: <name> — <label>.`
- **Native reports are C++ and are never presented as specs.** The panel above
  the designer shows the implementation file and function, read-only, from
  `GET /api/report/<name>/designer`. That route also says whether a spec can
  stand in for the report:
  - `equivalence: "exact"` — the spec renders the report's own cells. That is
    **Trial Balance** (a flat account list with Debit and Credit columns over
    the same as-of date), and the reports a *source* serves: the **Balance
    Sheet** (balance-sheet source), the **Receivable** and **Payable Aging**
    (invoices source), the **Lot Viewer** (lots source), the **Budget Report**
    (budgets source), the **Cash Flow** statement (cash-flow source) and all
    six **documents** (document source: the spec holds the document's name,
    invoice and print options and renders through the document renderer, so
    the same invoice and options print the same bytes). The banner reads: "This
    report is implemented in C++; this is an equivalent starting point, not its
    definition.  Exact equivalence: the starting spec renders the report's own
    cells."  The gate harnesses post the populated fields and compare the
    preview with the report's own document.
  - `equivalence: "approximate"` — a useful starting point whose differences
    are named. **Income Statement** and **Profit & Loss** list the same Income
    and Expense accounts over the report's default period, but as a flat
    Debit/Credit list; the note says the C++ report prints one sign-reversed
    Balance column, groups the rows under Revenues and Expenses, adds parent
    subtotals and a Net income line. The **Tax Schedule Report/TXF Export**
    (tax-codes source) lists the same tax codes over the report's period with
    the same per-code totals, but the report also prints one row per account
    with that account's total, which the spec's code table does not. The banner
    reads "This report is implemented in C++; this is an approximate starting
    point, not its definition.  Approximate equivalence: <how it differs>".
  - `equivalence: "none"` — no designer fields are populated and the form is
    hidden, so nothing can be mistaken for a definition. The banner says
    plainly that there is no spec: "No spec: this report is implemented in C++
    and has no designer equivalent; a `<source>` source would be needed, not a
    field on an existing one."  The source name is derived from the report's
    own name as a proposal (**Transaction Breakdown** therefore proposes
    `transaction-breakdown`, whose dynamic account columns a fixed spec column
    list cannot describe).  Where an existing source reproduces the cells but
    has no portable starting point, the banner says that instead:
    **Transaction Report**, **General Ledger** and **General Journal** are the
    `splits` listing, but the designer cannot start one because the report's
    account selection is this book's GUIDs.  The note then names the
    family's reason: portfolios and gains analysis, budget statements, owner
    documents, the GST statement, the Cash Flow Barchart (its axis is a
    date-interval series, not the statement's account rows) and the two
    multicolumn statements (no period column axis) all have rows or columns the
    spec vocabulary cannot express. **Balance Sheet (Multicolumn)**,
    **Income Statement (Multicolumn)** and the **Equity Statement** are in this
    group too: their column axis or roll-forward rows are not a table spec.

Switching reports always clears the previous report's fields first, and
switching the source picker clears every field the new source does not accept;
a report with no equivalent cannot leave stale values behind. **New report**
clears the designer and returns it to a blank account-source spec with the
normal Save button.

## Built-in specs

`gnc-webd` defines six report specs in code, so a fresh build — with or
without Guile — offers reports beyond the native reports:

| id | report | scope and chart |
|---|---|---|
| `builtin-income-expenses-month` | Income vs Expenses by Month | `INCOME` + `EXPENSE`, one line point per month over the book's transaction range; two series, `credit` (income) and `debit` (expenses) |
| `builtin-expenses-by-account` | Expenses by Account | `EXPENSE`, all dates, one bar per expense account with activity |
| `builtin-income-by-account` | Income by Account | `INCOME`, all dates, one bar per income account with activity |
| `builtin-net-worth-month` | Net Worth by Month | asset and liability account types, one point per month stepped from the book's first transaction date (the Scheme charts' date list, with the range end appended), each point the cumulative balance at that date's end (so the line is net worth, not the month's change) |
| `builtin-net-worth-barchart` | Net Worth Barchart | asset and liability account types, the same points, cumulative balance; three series, `debit` (the positive-balance side), `credit` (the negative-balance side) and `balance` (net worth) |
| `builtin-net-worth-linechart` | Net Worth Linechart | the same three series as a line |

Their tags are defined in code too: `income`, `spending`, `trend`, `period`
(income vs expenses); `spending`, `breakdown` (expenses); `income`,
`breakdown` (income); `net-worth`, `trend` (net worth by month); `net-worth`
with `bar-chart` or `line-chart` (the two net-worth charts). A fresh store can
therefore be filtered usefully before anything has been saved.

They are served by `GET /api/specs` under `builtin` (with `"builtin": true`),
are named in `GET /api/reports` beside the native and Scheme reports, and
render at `/report/spec/<id>`. They are not written to the saved store, and
`DELETE /api/specs/<built-in id>` is a 400 saying so. Their descriptions say
which accounts they are scoped to, because a period report over an unscoped
double-entry book nets to zero every period: the scoping is what makes the
numbers mean something, and an unscoped net-worth report would not exist.

## The Scheme chart names

Of the four Scheme names built from `standard/net-charts.scm`, two are
shipped as the built-ins above and two are closed:

| Scheme name | Decision |
|---|---|
| Net Worth Barchart | Shipped.  Assets / Liabilities / Net Worth are the cumulative period's debit / credit / balance.  On `sample1.gnucash` the three series match the Scheme chart's 13 month-end values exactly (see `doc/report-coverage.md`). |
| Net Worth Linechart | Shipped, the same data as a line. |
| Income/Expense Chart | Closed.  The Scheme report's third series is Net Profit = income − expense.  The spec's `balance` is debit − credit = expense − income, so the third series would be plotted with the opposite sign; the spec has no negated-balance series. |
| Income & Expense Linechart | Closed for the same reason. |

The two shipped reports draw the amount columns' names ("Debit", "Credit",
"Balance"), not the Scheme labels ("Assets", "Liabilities", "Net Worth"), and
they cover the book's transaction range rather than the Scheme report's
current-fiscal-year default.  Their `debit`/`credit` are the balance signs,
not the account types; an overdrawn asset (a negative asset balance) lands on
the credit side where the Scheme report keeps it under Assets as a negative
value.  A book whose balances all have their normal sign (the usual case)
produces the Scheme numbers.

**Account Summary** is not a spec at all: the Scheme default sign-reverses
credit-normal accounts (A/P +585, Equity +10,000), prints own-balance parent
rows with no subtotal rows, offers an Account Code column, and reports as of
today.  The spec's `balance` is the raw signed amount, `account-tree` prints
subtree rollups plus a subtotal row per parent, has no code column, and its
date presets only reach `all`/`this-year`.  It stays a Scheme-only report and
is closed in `doc/report-coverage.md`, not approximated with a built-in.

## Reports a spec cannot describe: the budget reports

A spec is a query over splits plus a column vocabulary.  The budget reports
(`/report/Budget%20Report`, `/report/Budget%20Income%20Statement`,
`/report/Budget%20Profit%20%26%20Loss`,
`/report/Budget%20Balance%20Sheet` and `/report/Budget%20Flow`, ported from
`reports/standard/budget*.scm`) are not specs: their amounts come from the
book's budget object, an engine entity stored beside the accounts, not from
splits in a date range.  They therefore have no built-in spec and no designer
form.  They are native reports in their own right, so they are named in
`/api/reports`, have option descriptors and are rendered in both builds, but
the designer's controls cannot express "budget value for this account in this
budget period".

The five deliberately expose only what the native option surface can honestly
carry: the account selection, plus the report currency for the three layouts
that convert into one.  The Scheme reports' dates are budget-period selectors,
their budget options select among the book's budgets, and their
accumulate/difference/total/notes switches are columns; none of those has a
native option type yet.  The defaults of all of them are what the native
reports render, and `web/test/report-parity.sh` compares each with the Scheme
report on a generated book (`web/test/make-budget-book.py`).

The sixth budget-class name, **Budget Chart**, is deliberately still Scheme
only.  It draws one chart per leaf account, each with its own title and a
period range chosen by period index; the native `Chart` model is one chart per
table with shared labels, and no report route can select its period range
honestly.  A native version would either change the report or be a chart with
one series per account, which is not the same report.


## HTTP surface

Every route is under the server's random token, like the rest of the web UI.

| route | method | purpose |
|---|---|---|
| `/api/spec/options` | GET | the `sources` list, the account columns/groupings/period units/modes/charts/chart series/account types and the date/currency descriptors, plus each source's own field list (`balance_sheet`, `invoices`, `lots`, `budgets`, `tax-codes`, `cash-flow`, `document`) and the split columns and groupings the designer form is generated from |
| `/api/spec/preview` | POST | validate and render an unsaved spec; returns the same HTML a saved report produces |
| `/api/specs` | GET | list saved specs under `specs`, and the built-ins under `builtin`; `?tag=` and `?q=` filter both lists |
| `/api/specs` | POST | validate, assign an id and save; `400` names the invalid field, `201` returns the saved spec |
| `/api/specs/<id>` | GET | one saved or built-in spec |
| `/api/specs/<id>` | DELETE | delete a saved spec; a built-in is a `400` |
| `/api/report/<name>/designer` | GET | a native report's implementation source and, only when it can be backed up, a starting-point spec with `equivalence` `exact`/`approximate`/`none` |
| `/report/spec/<id>` | GET | render it through the native report layer |

Creation validates every field against the open book: unknown columns,
groupings, chart types, account types, currencies, account GUIDs, spec fields,
malformed dates, invalid tags, duplicate names and a start after the end are
`400` responses whose body names the problem. The preview and the saved report
run the same validator and the same renderer, so the preview cannot disagree
with what Save produces.

## Correctness gate

`web/test/report-spec.sh` takes each built-in spec from `GET /api/specs`,
strips the `builtin` flag, posts it to `POST /api/spec/preview`, and compares
the preview with `/report/spec/<id>` **byte for byte**: a built-in that is
not exactly the spec a user could build by hand fails. It also asserts the
chart's data block is the period table's own Debit/Credit figures, that the
net-worth built-ins draw all three series, that a narrower scope and a wider
period change the three series, and that each nonsensical chart combination
is a 400 naming the field. The tag checks quote the filtered id lists: one
tag selects exactly one built-in, a shared tag selects all of them, an
unknown tag is the empty object, `?q=` matches a name and a tag, the two
filters compose, an invalid tag is a 400 naming it, and a saved spec's
trimmed tags come back after the store is re-read. Finally, it checks the
native designer route: every one of the 39 native reports publishes an
`equivalence`, Trial Balance's starting point previews the **native report's
cells**, the two income statements say `approximate` and say how, a listing
report has no spec at all, and an unknown report is a 404.

`web/test/ui-test.mjs` builds a spec in the browser equivalent to an explicit
`/report/Trial Balance?end=2018-12-31` request, previews it, saves it, reloads
the page and opens it again, and compares the rendered **table text cell by
cell** with the explicit request. It opens a built-in chart spec, asserts the
canvas has non-blank pixels and two series, then renders the same spec with a
different period and requires both the chart data and the canvas pixels to
change — a static chart image cannot pass. It also selects a built-in and
compares **every** populated field with `GET /api/specs/<id>`, refuses a
same-name Save as, forks the built-in, and requires the fork's document to be
identical to the built-in's render; it then switches built-ins and requires
the fields to change rather than go stale. For a native report it checks the
implementation panel and label, posts the Trial Balance starting point and
compares the preview's cells with the native URL, checks the income
statement's approximate label and note, and checks that a report with no
equivalent hides the form and leaves it empty before New report restores it.

Its final gate covers the source-aware form in the browser, not through the
API: it reads the nine sources from `/api/spec/options`, requires the picker to
list exactly them with their labels, and for **each** source selects it, fills
the minimum fields, previews (requiring a document whose visible text contains
that source's own title or invoice, and no error), saves, reloads the page,
reopens the saved spec, and checks that its source and tags survived.  It also
fills account-only fields and switches to the invoice source, requiring the
currency, start date and grouping to be cleared and hidden and the switched
form to preview without a rejection; and it checks the provenance banner's
plain words for an exact report (**Trial Balance**), an approximate one
(**Income Statement**, including how it differs) and an unexpressible one
(**Transaction Breakdown Report**, naming the `transaction-breakdown` source it
would need).  A fork of the **Balance Sheet** starting point is saved and
required to carry `source: "balance-sheet"`, and the **Printable Invoice**
starting point must carry its document layout.

## Builds without Guile

The designer is part of the native report layer and needs no Guile: the same
routes, storage and rendering work in the `WITH_GUILE=OFF` build. The six
built-in specs are named in `/api/reports` in both builds; alongside the
Scheme templates in a Guile build, and as the reports beyond the native
thirty-one without one. The 501 for a report the native layer does not know and this
build cannot run stays exactly as it was: no built-in makes a Scheme report
look native.
