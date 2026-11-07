# ADR 0011 — The register needs decoupling before Phase 3, not a view port

- **Status**: Accepted (2026-11-06)
- **Supersedes**: the assumption in
  [0001](0001-qt6-presentation-tier.md) that Phase 3 is "port the register"
- **Related**: [0004](0004-html-layer-separation.md) (the same pattern elsewhere)

## Context

`doc/qt6-migration-plan.md` described Phase 3 as reimplementing the register on
`QAbstractItemModel`, and PLAN.md asked how "split rows" map onto an item model.
A survey of `gnucash/register/` before writing widget code found that both
premises are wrong.

**The register is not row-based.** `gnucash/register/register-core` is a
**virtual-cell** architecture: `cellblock.h` describes a rectangular grid of
active cells with a cursor, explicitly because "it allows a group of cells to be
treated as a single transactional entity ... all edits to a group of cells can be
simultaneously committed or rejected". That is two-phase commit at cell
granularity, which is why the register can abandon an edit to a whole
transaction with Escape. `ledger-core` holds the split-register logic on top;
`register-gnome` draws it. There is no row-type enum anywhere in the tree.

**The cell layer is GTK-coupled.** This is the expensive part:

- `register-core/basiccell.h` includes `<gtk/gtk.h>` and `gnc-ui-util.h`
  (line 166);
- `formulacell.c`, `pricecell.c`, `quickfillcell.c`, `doclinkcell.c` and their
  headers include `gnc-ui-util.h` / `gnc-ui.h`;
- `gnc-register-core` links `gnc-gnome-utils` in its `CMakeLists.txt`.

So the "core" is not a portable model with a GTK view bolted on. A Qt register
cannot reuse it without dragging GTK into the Qt process — the same coupling
ADR 0004 found in the HTML layer, in a much larger component.

## Options

1. **Port only the view.** Write `register-qt` drawing with `QPainter`, keep
   `register-core` as is.
2. **Decouple the cell layer, then port the view.** Replace GTK types in
   `basiccell`/`cellblock` (fonts, colours, widget handles) with a neutral
   abstraction, keep `register-gnome` working on top of it, and add a Qt view.
3. **Rewrite the register on Qt item views.** Model rows and delegate editing,
   reimplementing cursor semantics.

## Decision

Option 2, in that order: **decouple first, port the view second.** Options 1 and
3 are rejected for stated reasons:

- Option 1 produces a Qt application that links GTK. That is precisely the
  outcome the migration exists to prevent, and it doubles the toolkit a Windows
  build would need.
- Option 3 discards the two-phase-commit cursor semantics and the per-cell
  editing rules that 36k lines encode, and would take longer than decoupling
  while being more likely to change what users feel.

## Consequences

- **Phase 3's estimate is wrong.** It assumed a view port; it is a cell-layer
  decoupling plus a view port. The budget in the migration plan should be
  revisited rather than discovered.
- The decoupling is a prerequisite for any Qt register work, and it is
  independent of Qt: it also makes the GTK register easier to test.
- Each cell type that touches `gnc-ui-util` is a small project: decide per cell
  whether the GTK use is presentation (abstract it) or genuinely widget
  behaviour (move it up to the view).
- The GTK build must keep working throughout, which means the abstraction is
  introduced under both views rather than replacing GTK outright.
- `QAbstractItemModel` may still be the right shape for the account tree and
  other list views. It is not the shape of the register.

## Validation

Survey only; no code written. The evidence is the source tree: the `CellBlock`
documentation quoted above, the `<gtk/gtk.h>` include in `basiccell.h:166`, the
`gnc-ui-util.h` includes across the cell implementations, and
`gnc-register-core`'s link line.

**Not investigated yet:** how much of `basiccell` is presentation (fonts,
colours) versus behaviour; whether a thin portable struct can serve both views
without redesigning the cell API.
