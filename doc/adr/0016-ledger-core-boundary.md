# ADR 0016 — The Qt register view needs a GTK-free `ledger-core`, not only a GTK-free `register-core`

- **Status**: Accepted (2026-11-07)
- **Related**: [0011](0011-register-decoupling.md) — its decision (decouple before
  porting) stands; only the boundary it implied is corrected here
- **Related**: [0004](0004-html-layer-separation.md) — the same
  abstraction-under-both-views pattern, applied one layer up

## Context

ADR 0011 decided to decouple the cell layer before writing a Qt view, and
`doc/register-core-audit.md` then estimated that work at three call sites and
1–2 weeks. On 2026-11-07 that estimate was tested by removing the two GTK
includes from `basiccell.h` and `gnc-gnome-utils` from `gnc-register-core`'s
link line, leaving everything else alone.

The three call sites were real, and they are now gone: `register-core` is
toolkit-free (verified at link level — no toolkit in its `NEEDED` list, zero
`gtk_`/`gdk_`/`pango_` symbols). But the removal broke 15 translation units in
four directories, and the interesting breakages were not the three:

- five headers outside `register-core` (four in `ledger-core`, one in
  `gnome-utils`) name `GtkWidget`/`GtkWindow` and only got the type from
  `basiccell.h`'s `<gtk/gtk.h>`;
- four `register-gnome` files reached `gnome-utils`' headers through
  `gnc-register-core`'s link line, never declaring the dependency;
- `register-core` used `gnc_exp_parser_parse` without declaring
  `gnc-expressions`, which also arrived through that link line.

So the layer above the cells is where the coupling actually lives. Measured:
`register-core` is 8,056 lines with no GTK; `ledger-core` is 19,450 lines, 14 of
its 28 source files use GTK, five public headers expose `GtkWidget`, and it
builds eight message dialogs and runs two `gtk_dialog_run` loops (nested main
loops). `register-gnome` is the 12,840-line view, which is expected to be GTK.

A Qt view must sit on *something*. `ledger-core` is the register's logic — the
split-register cursor, the entry ledger, the edit/commit rules — and it is the
interface 3b would consume. As it stands it cannot be consumed from a Qt
process without dragging GTK in, which is the outcome the migration exists to
prevent (ADR 0001, ADR 0011 option 1).

## Options

1. **Start 3b on `ledger-core` as it is**, linking GTK in the Qt process
   temporarily and removing it later.
2. **Reimplement the split-register logic in Qt**, reading only `register-core`
   (which is now clean).
3. **De-GTKify `ledger-core` first**, then write 3b against its API.

## Decision

Option 3. Option 1 recreates the coupling the migration is removing, in the one
process that must not have it, and "temporarily" has no exit gate. Option 2
discards 19k lines of cursor, commit and validation semantics that users feel
directly — the same objection ADR 0011 raised against rewriting the register on
item views — and it would be paid for out of 3b's budget without reducing the
total.

Concretely, before 3b starts, `ledger-core` must:

- replace `GtkWidget *parent` in its public API with an opaque handle the view
  supplies, so callers do not name a toolkit type;
- return validation/dialog outcomes to its caller instead of raising
  `gtk_message_dialog_new` and running `gtk_dialog_run` itself, following the
  seam already used for the cells (`gnc_basic_cell_set_error` →
  `gnc_table_leave_update` returning the message, presented by the view).

## Consequences

- **Phase 3a's boundary moves up one layer**, and with it the estimate: 1–2
  weeks becomes 2–3 months, of which the `register-core` slice is done and
  `ledger-core` is 4–8 weeks. This is close to ADR 0011's original 3–5 months,
  which was right for the wrong reason (it inferred from the link line rather
  than from measurement).
- **3b stays at 5–7 months only if 3a includes `ledger-core`.** If it does not,
  3b absorbs a reimplementation and grows.
- The pattern is now established twice (HTML layer, cells), so the `ledger-core`
  work is mechanical rather than exploratory; the per-dialog decisions (who
  presents, what happens on the read-only path) are where the time goes.
- `register-gnome` keeps working throughout: the abstraction is added under it,
  and it remains the only implementation until a Qt view exists.

## Validation

Measured and verified on 2026-11-07 in the WSL/ext4 tree: the falsification
build (15 objects, 86 errors, four directories), the link-level check of
`libgnc-register-core.so`, the GTK build, `test-split-register-copy-ops` and a
new `test-register-core-decoupling`, and the Qt smoke test. Details and the
exact numbers are in `doc/register-core-audit.md`.

**Not verified:** the dialogs were not exercised interactively, so presentation
behaviour for a bad cell value is compile- and unit-tested but not
click-tested; and none of this was built on Windows.
