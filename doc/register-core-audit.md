# `register-core` GTK audit (Phase 3a groundwork)

**Status: the audit was tested and corrected on 2026-11-07.** It predicted three
call sites and a 1–2 week phase. The three call sites were real — all three were
found and removed — but they did not bound the work: removing them broke 15
translation units in four directories, and the layer above `register-core` turned
out to be more GTK-coupled than the cells were.

The original method and result are kept below, because the mistake in them is
the useful part.

## 1. The original audit (grep-based, unproven)

Method: `grep` over the 35 files of `gnucash/register/register-core` for GTK, GDK
and Pango symbols and for `gnc-ui`/`gnc-gnome` includes:

```sh
cd gnucash/register/register-core
grep -hoE '\b(Gtk[A-Za-z]+|Gdk[A-Za-z]+|pango_[a-z_]+|PANGO_[A-Z_]+)\b' *.c *.h
grep -hoE '\bgnc_ui_[a-z_]+' *.c *.h
grep -n '#include' basiccell.h
```

Result:

| File | GTK/Pango use | Classification |
|---|---|---|
| `doclinkcell.c` | `GtkWidget`, `gtk_widget_create_pango_layout`, `pango_layout_set_text`, `pango_layout_get_unknown_glyphs_count` (around line 271) | Presentation: measures whether a font can render a string, by building a hidden label |
| `formulacell.c` | `gnc_warning_dialog (gnc_ui_get_main_window (NULL), …)` (line 120) | Behaviour: a modal error dialog raised from validation |
| `pricecell.c` | same dialog (line 160) | Behaviour, same |
| `basiccell.h` | includes `<gdk/gdk.h>`, `<gtk/gtk.h>`, `"gnc-ui-util.h"` | Only `gnc-ui-util.h` is needed: it is the **app-utils** header, and it is GTK-free |
| the other 31 files | nothing | No GTK, GDK or Pango symbols |

And the conclusion drawn from it: the work is "three call sites and a link line",
so Phase 3a is **1–2 weeks**, with 3b unchanged.

## 2. The experiment

Three edits, nothing else: remove `<gdk/gdk.h>` and `<gtk/gtk.h>` from
`basiccell.h`, and drop `gnc-gnome-utils` from `gnc-register-core`'s
`target_link_libraries`. Then rebuild `gnucash` (which had just been built
clean) with `-k 0` to collect every consequence rather than the first.

**Result: 15 object files failed, 86 errors, in four directories.**

| Directory | Files that broke | Cause |
|---|---|---|
| `register-core` | `doclinkcell.c`, `formulacell.c`, `pricecell.c` | The three predicted sites. `doclinkcell.c` failed on the Pango/glyph code as expected; the other two failed earlier, on `#include "gnc-ui.h"`, because that header lived on the include path the link line provided |
| `ledger-core` | 8 objects, from 4 headers | `split-register.h`, `gnc-ledger-display.h`, `gncEntryLedger.h` and `gncEntryLedgerP.h` name `GtkWidget` but only got the type from `basiccell.h`'s `<gtk/gtk.h>` |
| `register-gnome` | `datecell-gnome.c`, `gnucash-sheet.c`, `gnucash-header.c`, `gnucash-sheet-private.c` | They include `dialog-utils.h` / `gnc-gtk-utils.h` from `gnome-utils`, which was only on their include path transitively, through `gnc-register-core → gnc-gnome-utils` |
| `gnome-utils` | `dialog-account.h` (hit from `gncEntryLedgerControl.c`) | A public header that names `GtkWindow` without including `<gtk/gtk.h>`; it had been relying on its includers having it |

A fifth, quieter failure appeared only when a test linked
`libgnc-register-core.so` on its own: `undefined reference to
gnc_exp_parser_parse`. The cells parse formulas and amounts, so they need
`gnc-expressions`; that too had been arriving through `gnc-gnome-utils`.

## 3. What the grep could not see

Three things, all of which are about *transitive* dependencies rather than
symbols used in `register-core`:

1. **Five headers name GTK types without including GTK.** Four are in
   `ledger-core`, one in `gnome-utils`. `basiccell.h` was their include.
2. **A link line is also an include path.** `gnc-register-gnome` never declared
   `gnc-gnome-utils`; it got `gnome-utils`' headers through
   `gnc-register-core`. So the "GTK coupling" of `register-core` was partly
   other people's coupling flowing through it.
3. **A link line also supplies symbols.** `gnc-expressions` was never declared
   by `register-core` either. The audit's lesson — "a link line overstates
   coupling" — is half right: this one overstated the *cells'* use of GTK and
   understated their real dependencies. Both directions were invisible to grep.

## 4. The abstraction that replaced them

Not "put GTK back"; the three sites became two seams the view fills in.

| Site | Before | After |
|---|---|---|
| `doclinkcell.c`, glyph check | built a hidden `GtkLabel` and Pango layout to count unknown glyphs | `gnc_doclink_cell_set_glyph_support_func()` — the view installs a text-metrics callback; with none installed the glyphs are assumed present. The GTK view installs a Pango implementation in `gnucash_sheet_new()` |
| `formulacell.c`, `pricecell.c`, parse errors | called `gnc_warning_dialog (gnc_ui_get_main_window (NULL), …)` from `leave_cell` | `gnc_basic_cell_set_error()` records the message; `gnc_table_leave_update()` hands it to its caller as a `char *` the caller owns. The two callers that used to get a dialog for free — `gnucash_sheet.c` and `gncEntryLedgerLoad.c` — present it |

The rest of the fallout was fixed by making each file declare what it uses:
`<gtk/gtk.h>` in the five headers that name GTK types, `gnc-gnome-utils` on
`gnc-register-gnome`'s own link line, and `gnc-expressions` on
`gnc-register-core`'s.

## 5. Verified after the change

- `libgnc-register-core.so` has **no toolkit dependency**: its `NEEDED` entries
  are `gnc-expressions`, `glib`, `gnc-app-utils`, `gnc-engine`, `gnc-core-utils`
  and `libc`; `nm -D --undefined-only` finds zero `gtk_`/`gdk_`/`pango_`
  symbols, and a token grep over its 8,056 lines finds none either.
- `gnucash` builds and links (`bin/gnucash`, GTK dependencies intact).
- `test-split-register-copy-ops`: passes. New
  `test-register-core-decoupling`: passes — it covers both seams without a GUI,
  and links only `register-core`, so it also proves the library is toolkit-free.
- Qt smoke test: `accounts=104 top_level=5 pane_ok=1 events=20/20 in_order=1
  on_qt_thread=1 model_ok=1 inserts=1 removes=1 reports=63`, exit 0.

## 6. What this does to the estimate

The audit's number was attached to the wrong boundary. `register-core` is
toolkit-free now and it cost one session, not 1–2 weeks. But `register-core` is
not the layer a Qt view would sit on: `ledger-core` is, and it is the most
GTK-bound part of the register outside the widget code.

Measured, not guessed:

| Layer | Lines | GTK-bearing files | GTK in public headers |
|---|---|---|---|
| `register-core` | 8,056 | 0 of 35 | none (after this change) |
| `ledger-core` | 19,450 | 14 of 28 | 5 headers expose `GtkWidget`; 30 `GTK_WINDOW`, 22 `GtkWidget`, 8 `gtk_message_dialog_new`, 2 `gtk_dialog_run` |
| `register-gnome` | 12,840 | the view; expected | n/a |

`ledger-core` is where the register's *logic* lives (split register, entry
ledger), and it calls modal dialogs and returns `GtkWidget` parents. A Qt view
cannot consume it as it stands. So either Phase 3a takes `ledger-core` to a
GTK-free API too, or 3b re-implements that logic — which is the larger number.

| | Before | After | Basis |
|---|---|---|---|
| Phase 3a — `register-core` alone | 1–2 weeks | **done** (this change) | 3 sites, 8,056 lines |
| Phase 3a — remainder: `ledger-core` de-GTK | not in scope | **4–8 weeks** | 19,450 lines, 14 files, 5 public headers, 8 message dialogs and 2 nested dialog loops; same mechanical shape as this slice, but each dialog is a behaviour question |
| Phase 3a total | 1–2 weeks | **2–3 months** | ADR 0011's original 3–5 months is the right order of magnitude after all, at its lower end |
| Phase 3b — the Qt view | 5–7 months | 5–7 months **only if** 3a includes `ledger-core`; otherwise larger | 3b would otherwise re-implement 19k lines of register logic |

The honest summary: **the audit's three call sites were correct; its scope was
not.** "Almost GTK-free" was true of one library and false of the layer above
it, and the grep could not tell the difference because the dependencies it
missed were transitive. The boundary decision is recorded in
[ADR 0016](adr/0016-ledger-core-boundary.md).

## 7. Not verified, and blockages

- **No interactive GUI test.** Nobody typed a bad formula into a register and
  watched the dialog appear. The cell/table seam is unit-tested and the
  presentation call sites compile and link, but the end-user behaviour of the
  two dialogs is unproven this session. This is the one thing a reviewer should
  click through before the branch is trusted.
- **Windows not built.** Only the WSL/ext4 tree was tested.
- **`test-register` (Scheme) fails in this build tree**: `Unable to find file
  "test-register" in load path`. No `test-*.scm` is copied into the build tree
  at all (a `find` over it returns zero), so every Scheme test there fails the
  same way. This is a pre-existing build-tree gap, not a consequence of this
  change — the failure happens in `load-from-path` before any C code runs.
- The five headers still say `GtkWidget`. That is recorded here as the finding,
  not fixed: removing GTK from `ledger-core`'s API is the remainder of 3a.
