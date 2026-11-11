# Web UI roadmap — from test harness to application

Status: long-term goals, not yet scheduled
Branch: `webui/engine-server`
Related: `WEBUI.md`, `doc/report-engine-migration.md`, `doc/eguile-decision.md`

## Where the interface stands

The web UI was not designed. It was built to prove the engine works without
Guile, and every element exists because a test needed a handle on it: an
account tree to prove balances roll up, a report list to prove dispatch, an
iframe to prove documents render, a form generated from option descriptors to
prove options reach the engine, a designer to prove specs are data.

That is a test harness that happens to be usable. Its whole visual system is
about forty lines of CSS in `web/ui/index.html`. There is no layout system, no
navigation beyond the report list, no keyboard model, no accessibility pass, no
responsive behaviour, and no way to edit a transaction.

What exists today:

| surface | state |
|---|---|
| account tree with balances | works, one level of folding |
| report list, options form, saved views | works |
| report rendering (iframe) | works, 45 names in a Guile-free build |
| register page (server-rendered) | **read-only**, navigable from report links |
| register grid (content area) | works: keyboard navigation, transaction panel, and three editable text fields in a writable session (`--writable`, ADR 0014) |
| report designer, built-in specs, tags | works (tags in progress) |
| book selection | **command-line argument only** |
| data entry | **partial** — description, memo and number per split, with File > Save and the engine's backup; amounts, new transactions and undo are absent |

## Long-term goals

These are the goals, in the order they must happen. Each is a workstream, not a
task; estimates assume the pace this branch has established for report work
(one cluster ≈ 2–4 hours of agent time with parity gates), and are honest
guesses for UI work, which has no equivalent gate.

### 1. Book picker (days)

Open, create and switch books from the browser instead of passing a path to
`gnc-webd`. Unblocks every other workflow: today a user cannot change books at
all. Needs a recent-books list, a file picker (or a directory scan), and a
decision about whether the process serves one book or many.

### 2. Register as a real widget, then the write path (weeks)

The single largest piece of the project and the gate on daily use. A grid with
keyboard navigation, inline editing, split rows, running balance and type-ahead
on account names — then a session opened read-write, save, backups, and the
conflict rules that come with them.

This changes the security model the ADRs currently promise (read-only,
loopback, token), so it needs an ADR before code.

### 3. Application shell and design system (weeks)

A persistent frame with navigation, a register pane that replaces the report
frame when an account is picked, breadcrumbs and a way back; plus the basics
the page lacks: spacing and type scale, colour roles that respect accounting
signs, focus states, dark mode, a print stylesheet for reports, and a
component inventory (date range picker, account picker with search,
currency picker, amount entry with calculator, dialogs).

### 4. Keyboard and accessibility (days each, ongoing)

GnuCash users are keyboard-heavy; a web UI without a keyboard model is a
downgrade for anyone migrating. Screen-reader semantics on the tree and the
register, focus management across the iframe boundary, and a documented
shortcut set.

### 5. Framework decision (one decision, then a migration)

Deferred since the first slice: vanilla ES modules today; vendored W2UI or
Preact+htm are the alternatives. The page is small enough that this is still
open, which is why it should be decided at stage 2 — after a register exists,
because the register is what makes the answer non-obvious.

## What is explicitly not planned

- Recreating GTK's tabbed document model. The web tier's reports are pages, and
  the URL scheme (`/<token>/...`) should be designed deliberately if that
  changes.
- A visual clone of the GTK UI. The goal is a good web application that uses
  the same engine, not a re-skin.
- Mobile layouts. Not a target; the register alone makes it a different project.

## How this is tracked

This document is the record. It is a goal list, not a schedule: nothing here is
in progress unless a commit says so, and the state table above is the honest
snapshot at the time of writing. When a workstream starts, it gets a section
here with its acceptance criteria, the way `doc/report-engine-migration.md`
does for the report layer.
