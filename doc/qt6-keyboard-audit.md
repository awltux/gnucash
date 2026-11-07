# Qt keyboard parity audit (Phase 2)

Method: the GTK tier's shortcuts are not in the code — they are loaded from
`gnucash/ui/accelerator-map` by `gtk_accel_map_load`
(`gnc-gnome-utils.c:660-675`), plus the bindings GtkTreeView and GtkNotebook
provide themselves. So the audit compares that table and those widget defaults
against what `gnucash-qt` binds, and records a decision per row.

Only browsing is in scope: the Qt tier has no editor, so register editing keys
are listed for completeness and not implemented.

## Where the bindings come from

| Source | What it covers |
|---|---|
| `gnucash/ui/accelerator-map` | every `GtkAction` in the main window and the plugin pages (`<Primary>o`, `<Primary>r`, …) |
| `gnucash/ui/accelerator-map-osx` | the same table with platform substitutions, used when `MAC_INTEGRATION` |
| GtkTreeView defaults | account tree navigation: arrows, `+`/`-`, Home/End, Page Up/Down, type-ahead search |
| GtkNotebook defaults | `Ctrl+Page Up` / `Ctrl+Page Down` switch pages; GnuCash does not override them |

## Browsing rows

| Action | GTK | Qt before this change | Decision |
|---|---|---|---|
| Open a book | `Ctrl+O` | `QKeySequence::Open` = `Ctrl+O` | match, no change |
| Quit | `Ctrl+Q` | `QKeySequence::Quit` = `Ctrl+Q` | match, no change |
| Refresh / reload the current page | `Ctrl+R` (`ViewRefreshAction`, every page) | `F5` only | **add `Ctrl+R`**, keep `F5` as the Qt habit |
| Close the current page | `Ctrl+W` (`FileCloseAction`) | none | **add `Ctrl+W`**: closes the current report page, ignored on Accounts |
| Switch page | `Ctrl+Page Up/Down` (GtkNotebook) | `Ctrl+Tab` / `Ctrl+Shift+Tab` | **add `Ctrl+Page Up/Down`**; keep `Ctrl+Tab` because it is the Qt convention |
| Preferences | Edit → Preferences, no accelerator | none | **add the menu item** (the dialog is the next increment) |
| New book, save, save-as, print, import, export | `Ctrl+N`, `Ctrl+S`, `Ctrl+Shift+S`, `Ctrl+P`, … | none | out of scope: the shell is read-only (Phase 4 and 5) |
| Account tree: find account | `Ctrl+I` (`EditFindAccountAction`) | none | deferred: `QTreeView` type-ahead searches the visible rows, which is not the same dialog; decide at the gate whether parity needs it |
| Account tree: edit / transfer / delete | `Ctrl+E`, `Ctrl+T`, `Delete` | none | out of scope: editing (Phase 4) |
| Register editing keys | many (`Ctrl+B`, `Ctrl+G`, …) | none | out of scope: there is no register view yet (Phase 3b) |
| Help | `F1`, About | none | deferred: no help system in the Qt tier |

## Tree navigation

`gnc-tree-view-account.c` installs no key handler, so the tree inherits
GtkTreeView's bindings: arrows move and expand, `+`/`-` expand and collapse,
Home/End and Page Up/Down move, and typing searches.

`QTreeView` provides arrows, Home/End, Page Up/Down and a type-ahead search as
well, but **not** `+`/`-`. The Qt tree therefore gains two small bindings so the
expand/collapse keys behave the same; everything else is the widget default on
both sides.

## What this audit does not claim

It does not prove that every GTK key works in the Qt tier, only that the
browsing-relevant bindings above were compared one by one. Register, invoice,
budget and scheduled-transaction keys are out of scope until those views exist,
and the audit will need a second pass then.
