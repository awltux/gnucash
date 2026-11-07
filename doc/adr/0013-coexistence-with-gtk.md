# ADR 0013 — Coexistence rules while both tiers ship

- **Status**: Accepted (2026-11-06)
- **Related**: [0001](0001-qt6-presentation-tier.md), [0010](0010-qt-tier-enters-the-build.md),
  [0012](0012-gui-thread-owns-the-engine.md)

## Context

The GTK and Qt presentations will coexist for at least one release cycle — the
migration plan requires the GTK build to keep passing every gate while the Qt
one grows. That raises questions the ADRs so far left implicit: what state the
two share, which binary owns it, and what the Qt tier may do that GTK cannot
undo.

Three things are shared today whether we plan for it or not: the user's book
files, the preference store, and the report engine. Each needs a rule, because
the obvious implementations of "let the Qt tier manage its own settings" or
"embed the report engine" would fork user state or pull GTK into the Qt process.

## Decision

**1. Preferences stay in GSettings.** The Qt tier reads and writes the existing
`org.gnucash.GnuCash.*` schemas through the same `gnc-gsettings` machinery the
GTK build uses. No Qt-side preference store exists while both tiers ship.

**2. Reports render through `gnucash-cli` until Phase 6.** The Qt application
spawns the CLI, which loads the book and runs the named report, and displays the
returned HTML. In-process rendering is rejected while `gnc-report` links GTK3.
The de-GTKification of `gnc-report`/`gnc-html` is the opening job of Phase 6, not
an optimisation to attempt opportunistically.

**3. One tree, two binaries, no shared mutable UI state.** `WITH_QT` stays opt-in
(ADR 0010), the Qt binary is `gnucash-qt` and installs beside `gnucash`, and both
use the same engine, backends and file formats. Neither tier stores UI state the
other must interpret: no Qt-only session files, no window geometry in KVP, no
"last opened" cache outside what GTK already writes.

## Consequences

- A preference the Qt tier needs must be added as a gschema entry so the GTK
  build sees it too. A Qt-only setting has to wait for Phase 8's `QSettings`
  import, or it does not ship.
- The helper-process report path is a **supported stopgap, not a design**. It
  costs a process spawn per report and cannot support live report options. Both
  costs are Phase 6's problem by construction.
- Anything that writes user state must be reachable by both tiers, or it is a
  bug against this ADR.
- Because both binaries share the engine, a stored-data bug found in one is
  reproducible in the other — which is the compatibility gate's whole point.

## Validation

Not yet observable: this ADR constrains future work rather than changing current
behaviour. What exists today that conforms: `WITH_QT` is opt-in, the Qt binary
uses `gnc-gsettings` for preferences (via the engine's preference calls), and
reports already go through the CLI. What does not yet exist: any Qt-tier
preference UI, so rule 1 has not been exercised by a real settings dialog.
