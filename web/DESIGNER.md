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

A `period`-grouped report needs concrete bounds: literal `start` and `end`,
a preset that resolves to them (`this-year` or `last-year`), or
`date_preset: "all"`. With `all` the renderer takes the range from the scoped
transactions, so a built-in period report needs no dates that fit only one
book.

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

## Built-in specs

`gnc-webd` defines six report specs in code, so a fresh build — with or
without Guile — offers reports beyond the native reports:

| id | report | scope and chart |
|---|---|---|
| `builtin-income-expenses-month` | Income vs Expenses by Month | `INCOME` + `EXPENSE`, one line point per month over the book's transaction range; two series, `credit` (income) and `debit` (expenses) |
| `builtin-expenses-by-account` | Expenses by Account | `EXPENSE`, all dates, one bar per expense account with activity |
| `builtin-income-by-account` | Income by Account | `INCOME`, all dates, one bar per income account with activity |
| `builtin-net-worth-month` | Net Worth by Month | asset and liability account types, cumulative balance at each month's end (so the line is net worth, not the month's change) |
| `builtin-net-worth-barchart` | Net Worth Barchart | asset and liability account types, cumulative balance at each month's end; three series, `debit` (the positive-balance side), `credit` (the negative-balance side) and `balance` (net worth) |
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
| `/api/spec/options` | GET | columns, groupings, period units/modes, chart types, chart series, account types and the date/currency option descriptors the designer form is generated from |
| `/api/spec/preview` | POST | validate and render an unsaved spec; returns the same HTML a saved report produces |
| `/api/specs` | GET | list saved specs under `specs`, and the built-ins under `builtin`; `?tag=` and `?q=` filter both lists |
| `/api/specs` | POST | validate, assign an id and save; `400` names the invalid field, `201` returns the saved spec |
| `/api/specs/<id>` | GET | one saved or built-in spec |
| `/api/specs/<id>` | DELETE | delete a saved spec; a built-in is a `400` |
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
trimmed tags come back after the store is re-read.

`web/test/ui-test.mjs` builds a spec in the browser equivalent to an explicit
`/report/Trial Balance?end=2018-12-31` request, previews it, saves it, reloads
the page and opens it again, and compares the rendered **table text cell by
cell** with the explicit request. It opens a built-in chart spec, asserts the
canvas has non-blank pixels and two series, then renders the same spec with a
different period and requires both the chart data and the canvas pixels to
change — a static chart image cannot pass.

## Builds without Guile

The designer is part of the native report layer and needs no Guile: the same
routes, storage and rendering work in the `WITH_GUILE=OFF` build. The six
built-in specs are named in `/api/reports` in both builds; alongside the
Scheme templates in a Guile build, and as the reports beyond the native
thirty-one without one. The 501 for a report the native layer does not know and this
build cannot run stays exactly as it was: no built-in makes a Scheme report
look native.
