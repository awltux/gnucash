# Report engine migration — removing Guile

Status: proposed
Branch: `webui/engine-server`

## Why

Guile is the last hard dependency standing between this project and a build
that runs anywhere the engine runs. It is not needed by the engine: accounts,
transactions, splits, backends, KVP, options, currency and the expression
parser are all C/C++. Guile is needed by exactly one subsystem — the report
engine — and by the GTK/Qt front ends, which boot through `scm_boot_guile` and
build their report menus in Scheme.

On Windows this is not theoretical: MSYS2 has no mingw64 Guile, Guile 3.0.11
aborts at init on 64-bit mingw, and Guile 2.2.7.3 fails deterministically while
compiling its own bootstrap. `web/WINDOWS.md` records the evidence.

The goal of this plan is that **the engine and the web UI build with no Guile
at all**, and that reports are produced by a native engine whose numbers are
proven equal to the Scheme engine's on the same books.

## What Guile provides today

| consumer | use |
|---|---|
| report engine (`gnucash/report`, ~39k lines Scheme) | 63 report definitions, HTML document model, options generator, `trep-engine`, stylesheets, eguile |
| report menus (`gnucash/gnome/report-menus.scm`, `report.scm`) | the GUI's Reports menu, saved reports |
| `gnc-report` | C++ ↔ Scheme bridge (`gnc_run_report_with_error_handling`, report ids) |
| `gnucash-cli -R run` | report rendering for scripts |
| GTK/Qt application startup | `scm_boot_guile`, plugin loading, custom reports |

## Target architecture

1. **`libgnucash/reports`** (new C++ library) — the shared engine:
   - a query layer over splits and transactions (account scope, date range,
     transaction/voided/reconciled predicates), reusing `QofQuery`;
   - grouping and aggregation with account-tree rollup, subtotal levels and
     running balances;
   - currency conversion at report date, using existing app-utils helpers;
   - a **column vocabulary** (`description`, `transfer`, `amount`,
     `running-balance`, `period-total`, `subtotal`, …) and a **report spec**
     (query + columns + grouping + sort + options) expressed as JSON.
2. **`gnc-report` rewritten in C++** — a registry of native report specs and a
   renderer producing the same HTML document model (and JSON for the browser).
3. **Options** — the existing `GncOptionDB` (already C++) supplies the schema;
   the native reports declare the options they use, and the same DB backs the
   web form and the designer.
4. **Designer** — a form over the report spec. Last, and the easy part.

## Stages and acceptance

Every stage is gated on numbers, not on code compiling. The oracle is the
Scheme engine running on the fixture book (`sample1.gnucash`), whose reports
can be captured as golden files before Guile goes away — Trial Balance is
29,538 bytes from `gnucash-cli -R run` today.

| stage | deliverable | acceptance |
|---|---|---|
| 0 | `WITH_GUILE=OFF` builds engine + `gnc-webd`; reports endpoint absent | configure requests no guile; smoke test passes without reports |
| 1 | query/aggregation library + **Trial Balance** | table cells equal the golden report for the fixture book; a second, larger book matches too |
| 2 | **Profit & Loss, Balance Sheet, Transaction Report, General Ledger** | same cell-by-cell comparison; sign conventions and rollups explicitly tested |
| 3 | options: date ranges, fiscal year, account selection, currency; `/api/report/<name>?…` | changing a date range changes the numbers as the Scheme report does |
| 4 | charts (vendored Chart.js), stylesheets | rendered HTML contains charts; browser test asserts canvas content exists |
| 5 | report designer over the spec | a user-created report renders and can be saved |
| 6 | long tail: aging, lots, invoices/tax | decide per report: port, or keep behind a Guile build |
| 7 | **drop Guile**: remove `bindings/guile`, SWIG report bindings, `gnc-report` bridge, `scm_boot_guile` startup, `gnucash-cli -R` | build and test with `pkg-config guile` absent |

## Compatibility

- **Saved reports** are stored as a report GUID plus option values. A reader
  maps known Scheme report ids onto native specs; unknown ids are reported as
  unsupported rather than silently rendered wrong.
- **Custom user reports** are Scheme by definition. They are the honest
  casualty of stage 7 unless a compatibility shim keeps Guile as an optional
  plugin — which the plan allows: stage 7 removes Guile from the *build*, and a
  Guile-enabled build may still load custom reports.
- **The GTK/Qt front ends** cannot survive stage 7, because they start inside
  Guile. Stage 7 therefore presupposes the web UI is the only front end. Until
  then, Guile stays for them and reports stay available there.

## Risks

- **Wrong numbers.** The dominant risk, and why every stage has a golden test.
  A report that renders and is subtly wrong is worse than one that is missing.
- **Option surface.** Dozens of options per report; stage 3 must not
  accidentally change defaults.
- **Long tail.** Aging, lots and invoices are accounting logic, not queries;
  they may never be worth porting.

## Effort

Stages 0–2: weeks. Stages 3–5: weeks. Stage 6: open-ended, per report. Stage 7:
small in code, large in consequence — it ends the GTK/Qt tiers.
