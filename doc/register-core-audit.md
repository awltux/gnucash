# `register-core` GTK audit (Phase 3a groundwork)

Method: `grep` over the 35 files of `gnucash/register/register-core` for GTK, GDK
and Pango symbols and for `gnc-ui`/`gnc-gnome` includes, recorded here so the
result can be re-checked in a minute:

```sh
cd gnucash/register/register-core
grep -hoE '\b(Gtk[A-Za-z]+|Gdk[A-Za-z]+|pango_[a-z_]+|PANGO_[A-Z_]+)\b' *.c *.h
grep -hoE '\bgnc_ui_[a-z_]+' *.c *.h
grep -n '#include' basiccell.h
```

## Result

**The core is almost GTK-free, and the coupling is three call sites.**

| File | GTK/Pango use | Classification |
|---|---|---|
| `doclinkcell.c` | `GtkWidget`, `gtk_widget_create_pango_layout`, `pango_layout_set_text`, `pango_layout_get_unknown_glyphs_count` (around line 271) | Presentation: it measures whether a font can render a string, by building a hidden label |
| `formulacell.c` | `gnc_warning_dialog (gnc_ui_get_main_window (NULL), …)` (line 120) | Behaviour: a modal error dialog raised from validation |
| `pricecell.c` | same dialog (line 160) | Behaviour, same |
| `basiccell.h` | includes `<gdk/gdk.h>`, `<gtk/gtk.h>`, `"gnc-ui-util.h"` | Of these, only `gnc-ui-util.h` is actually needed — it is the **app-utils** header and defines `GNCPrintAmountInfo`, `gnc_default_print_info`, `xaccPrintAmount`. It is GTK-free |
| the other 31 files | nothing | No GTK, GDK or Pango symbols at all |

`gnc-ui-util.h` exists in exactly one place — `libgnucash/app-utils/` — so the
cells' include of it is not the GTK one; the GTK dependency arrives through
`doclinkcell.c` and the two dialogs, plus the `gnc-register-core →
gnc-gnome-utils` link line in `CMakeLists.txt`.

## What the decoupling actually requires

1. **A text-metrics hook.** `doclinkcell.c` needs "can this font render this
   string". Replace the hidden `GtkLabel` with a function pointer supplied by the
   view (GTK implements it with Pango, Qt with `QFontMetrics`).
2. **A notification hook.** The two `gnc_warning_dialog` calls belong to the
   view: validation should return a failure the caller can present, not raise a
   modal dialog itself.
3. **Drop the vestigial includes** from `basiccell.h` and stop linking
   `gnc-gnome-utils` from `gnc-register-core`.

## Revised estimate

ADR 0011 estimated Phase 3a at **3–5 months**, inferred from the link line and the
`<gtk/gtk.h>` include in `basiccell.h`. The symbol-level audit says the work is
three call sites and a link line: **1–2 weeks**, with the remainder of Phase 3
being the view port (3b), which is unchanged.

The lesson is worth keeping: **a link line overstates coupling**. `gnc-register-core`
linked the whole of `gnc-gnome-utils` to reach one glyph check and two dialogs.

## Not verified

- The grep is textual: a GTK use hidden behind a macro, or in a header included
  from `register-core` but living elsewhere, would not appear. The check is to
  remove `gnc-gnome-utils` from the link line and build.
- `doclinkcell.c`'s measurement may not be the only reason for the GTK include in
  `basiccell.h`; removing the includes and compiling will settle it.
- Nothing here has been implemented. This document is an audit, not a change.
