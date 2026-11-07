# Phase 2 gate assessment (2026-11-07)

The migration plan says: *"A hard go/no-go follows Phase 2: if read-only
browsing is not at parity, stop and ship the GTK 4 path instead."* Phase 2's
exit gate is *"Browsing parity; keyboard navigation for tree and tabs."*

This document measures the Qt tier against that criterion and records what the
evidence supports. It is an assessment, not a decision: the go/no-go is a
product decision, and the plan text stays as written until it is ratified.

## What the Qt tier can do

| Surface | State | Evidence |
|---|---|---|
| Open a book, stay open | works | `--self-test` and app mode; CI `qt-smoke` |
| Account tree: names, hierarchy, balance, period total | works | `model_ok=1 total_ok=1 period_ok=1` |
| Reports: list, render, select, reload, one tab each | works | `reports=63 pane_ok=1 tabs_ok=1` |
| Preferences: negative-in-red, relative accounting period | works | `prefs_ok=1 prefs_ui_ok=1` |
| Keyboard: GTK's browsing bindings | works | `keys_ok=1`, [audit](qt6-keyboard-audit.md) |
| Nothing writes to a book | by construction | session opened read-only |

Every flag above is asserted by the harness CI runs, not by inspection.

## What GTK browses

Nine page types (`gnucash/gnome/gnc-plugin-page-*.cpp`): account tree, budget,
invoice, owner tree, register, register filter, register sort, report,
scheduled-transaction list. The account tree model alone exposes about forty
column roles (`gnc-tree-model-account.h:53-…`) — type, commodity, code,
description, present/cleared/reconciled balances in three currencies, dates,
colour and tooltip variants. Reports have option dialogs; the Qt tier renders
defaults only.

## Verdict on the literal criterion

**Not met, and not reachable where it sits.** The Qt tier implements two of
nine page types, and the one users spend their time in — the register — is
Phase 3b. Holding the gate as written therefore means holding it after 3b,
5–7 months from now, which is the opposite of a risk checkpoint: by then the
money that a no-go would save has already been spent.

The criterion is also the same mistake the register audit made: "parity" is
judgement until it is written down, and writing it down for all of browsing
reveals it is a definition of the finished product, not a gate.

## What the gate is actually for

It exists to answer one question before the expensive phases: **is the Qt
approach viable for this codebase?** The evidence for that is not "every view
exists" — it is that the seams work and the cost of the remaining work is
knowable: the engine owns the data, a Qt process can drive it on one thread,
reports render, engine events arrive, the GTK build is untouched, and CI runs
both tiers. All of that is now observed.

## Recommendation: three checkpoints, each with a decision

| Gate | When | Pass criteria |
|---|---|---|
| **A — shell viability** | now | The Phase 2 checklist is done or decided; `qt-smoke` green locally and in CI; GTK build and register tests green; no stored-data change. **All met**, except the human click-through below. |
| **B — layer boundary** | after Phase 3a | `ledger-core` exposes no `GtkWidget`; register tests pass; a test links the cell and ledger layers in a process without GTK — the same link-level proof used for `register-core`. |
| **C — register prototype** | early in Phase 3b, not after it | A read-only Qt register over the 20k-split corpus meets the plan's own performance gates (open < 1 s, 60 fps scroll, < 50 ms typing latency) and passes a written register parity checklist. |

Gate C is the one that de-risks 3b, because it is the first point where the
five-to-seven-month estimate meets real code. Gates A and B exist to stop the
programme earlier if the approach is wrong.

If this is ratified, `doc/qt6-migration-plan.md:43` should say "go/no-go after
checkpoint A" and Phase 2's exit gate should read "shell parity", with the
register's parity living in 3b's exit gate where it belongs.

## Decision required

Either:

1. **Adopt A/B/C** (recommended): the plan's single gate becomes three, and the
   shell gate can be held now on the evidence above; or
2. **Keep the literal gate**: accept that it can only be held after 3b, and
   that it is therefore a retrospective check rather than a risk checkpoint.

## Not verified

- **No one has clicked the Qt application.** Every claim above is a harness
  assertion or a model-level check; the visual result (columns, colours, tab
  layout, dialogs) has not been seen by a human. This is the weakest link in
  the evidence and the first thing a reviewer should do.
- Report **options** are not exercised: 63 reports render with defaults.
- macOS CI is red at "download dependency tarball" before any compilation — an
  infrastructure failure, recorded in PLAN.md §7, not a code result.
- The 20k-split performance gates have never been measured in the Qt tier;
  they are Gate C's subject.
