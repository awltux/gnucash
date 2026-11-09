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
      "columns": ["account-name", "balance"],
      "grouping": "period",
      "period_unit": "month",
      "start": "2018-01-01",
      "end": "2018-12-31",
      "date_preset": "",
      "currency": "USD",
      "accounts": [],
      "chart": "bar"
    }
  ]
}
```

## Spec fields

| field | meaning |
|---|---|
| `name` | required, unique, at most 80 characters; this is the report-list label |
| `title` | optional, at most 120 characters; defaults to `name` |
| `columns` | required non-empty array from `account-name`, `debit`, `credit`, `balance`, `subtotal`; no duplicates |
| `grouping` | `none` (one flat row per account), `account-tree` (rollups plus a subtotal per parent) or `period` (one row per year or month) |
| `period_unit` | `year` or `month`; used by `grouping: "period"` |
| `start`, `end` | optional `YYYY-MM-DD`; blank means unbounded on that side |
| `date_preset` | `this-year`, `last-year` or `all`; mutually exclusive with `start`/`end` |
| `currency` | optional commodity mnemonic; blank means the report default |
| `accounts` | array of account GUIDs; each means that account and its subtree; empty means the whole book |
| `chart` | `""`/absent, or `bar`; the chart section is rendered below the table, and the data is embedded in the HTML |

A `period`-grouped report needs concrete bounds: either literal `start` and
`end`, or a `date_preset` other than `all`.

## HTTP surface

Every route is under the server's random token, like the rest of the web UI.

| route | method | purpose |
|---|---|---|
| `/api/spec/options` | GET | columns, groupings, period units and the date/currency option descriptors the designer form is generated from |
| `/api/spec/preview` | POST | validate and render an unsaved spec; returns the same HTML a saved report produces |
| `/api/specs` | GET | list saved specs (full documents) |
| `/api/specs` | POST | validate, assign an id and save; `400` names the invalid field, `201` returns the saved spec |
| `/api/specs/<id>` | GET | one saved spec |
| `/api/specs/<id>` | DELETE | delete it |
| `/report/spec/<id>` | GET | render it through the native report layer |

Creation validates every field against the open book: unknown columns,
groupings, currencies, account GUIDs, spec fields, malformed dates, duplicate
names and a start after the end are `400` responses whose body names the
problem. The preview and the saved report run the same validator and the same
renderer, so the preview cannot disagree with what Save produces.

## Correctness gate

`web/test/ui-test.mjs` builds a spec in the browser equivalent to an explicit
`/report/Trial Balance?end=2018-12-31` request, previews it, saves it, reloads
the page and opens it again, and compares the rendered **table text cell by
cell** with the explicit request. A designer that rendered a plausible but
different report fails that check.

## Builds without Guile

The designer is part of the native report layer and needs no Guile: the same
routes, storage and rendering work in the `WITH_GUILE=OFF` build.
