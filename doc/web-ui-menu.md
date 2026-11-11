# The menu bar — the desktop's menus, and what the web tier does with them

Status: scaffolded in `web/ui/index.html`, tested in `web/test/ui-test.mjs`.
Branch: `webui/engine-server`, at `89e8638dc8` when this was written.
Related: `doc/web-ui-roadmap.md` (the workstreams this sits inside),
`web/DESIGNER.md` (the report designer the Reports menu drives).

This document is the record of the menu work: what the desktop application's
menus actually contain, which of those items the web tier can carry out today,
and what it does with the rest. The inventory in Appendix A is quoted from the
source, not from memory.

## 1. What was read, and what was not

The menu bar of the desktop application is not one file. It is:

- a skeleton with a *position* for every item —
  `gnucash/ui/gnc-main-window.ui:6-525`, `<menu id="mainwin-menu">`;
- a set of plugin fragments, each of which inserts a section at one of those
  positions — the `gnucash/ui/gnc-plugin-*.ui` files, the page plugins among
  them, plus `gnucash/import-export/aqb/gnc-plugin-aqbanking.ui` and
  `gnucash/import-export/ofx/gnc-plugin-ofx.ui`;
- three menus built in code rather than declared: the report tree under
  Reports, the recent-file entries under File, and the per-window entries
  under Windows.

The C++ that consumes these files is present in this tree and was read with
them: `gnucash/gnome-utils/gnc-main-window.cpp`,
`gnucash/gnome-utils/gnc-plugin-menu-additions.c`,
`gnucash/gnome-utils/gnc-plugin-file-history.c`, the page plugins in
`gnucash/gnome/`, and `gnucash/gnome-utils/gnc-plugin.c`. (The files are
`.cpp`, not the `.c` a reader may remember; the branch has the C++ conversion
of the main window and the page plugins.)

**The manual is not in this tree.** GnuCash's user manual lives in the
separate `gnucash-docs` repository; `doc/` here holds design notes for this
branch (`web-ui-roadmap.md`, `report-engine-migration.md`,
`eguile-decision.md`) and no menu reference. The UI files and the code that
merges them are therefore the authority for this document. Where a reader's
memory of the published manual disagrees with them, this document follows the
tree and says so; the notable cases are in §3.5.

## 2. How the desktop menu is put together

### 2.1 A skeleton of positions

Every unresolved item in `gnc-main-window.ui` is a placeholder action such as
`mainwin.FilePlaceholder0` with `hidden-when="action-disabled"`
(`gnucash/ui/gnc-main-window.ui:11-15`). The action does not exist, so GTK
hides the placeholder; the position remains. A plugin then inserts a section
at the *index of that action*, by name:

```c
/* gnucash/gnome-utils/gnc-main-window.cpp:3567-3579 */
for (gint i = 0; ui_updates[i]; i++)
{
    menu_model_part = (GMenuModel *)gtk_builder_get_object (builder, ui_updates[i]);
    ...
    gsm->search_action_name = ui_updates[i];
    ...
    if (gnc_menubar_model_find_item (priv->menubar_model, gsm))
        g_menu_insert_section (G_MENU(gsm->model), gsm->index, NULL,
                               G_MENU_MODEL(menu_model_part));
```

The list of names is a per-plugin field. `gnc-plugin-basic-commands.c:135-143`
names `FilePlaceholder0`, `FilePlaceholder2`, `FilePlaceholder3`,
`FilePlaceholder5`, `EditPlaceholder3`, `EditPlaceholder5`,
`ActionsPlaceholder2`, `ToolsPlaceholder1`, `HelpPlaceholder1`; the register
page names eighteen positions (`gnc-plugin-page-register.cpp:297-314`), the
report page eight (`gnc-plugin-page-report.cpp:220-227`).

Two consequences matter for the inventory:

1. **A position can receive several sections.** All six page plugins and
   `gnc-plugin-basic-commands.c` name `EditPlaceholder3`, so `Find…` is
   declared by seven files (`gnc-plugin-basic-commands.ui:58` and the six page
   files). Only the open page's plugin is loaded from its page file, so the
   window normally holds one page's copy; the appendix lists every
   declaration rather than guessing which is visible.
2. **A position with no plugin is simply empty.** Seven positions are never
   filled in this tree (Appendix A.3), including `EditPlaceholder0` — see
   §3.5 about Undo.

### 2.2 Three menus built in code

- **Reports** is generated from the report definitions. The plugin that builds
  it searches for `ReportsPlaceholder0`
  (`gnucash/gnome-utils/gnc-plugin-menu-additions.c:516`) and assembles
  `GMenuItem`s from the extension data's path and label
  (`gnc-plugin-menu-additions.c:383-450`). That is why the Reports menu has no
  static list of reports in the UI files: the list *is* the report
  definitions' own menu paths.
- **File > recent files** is `${FilePlaceholder6}` filled by
  `gnucash/gnome-utils/gnc-plugin-file-history.c:425`.
- **Windows** gets one item per open window, created and labelled in
  `gnc_main_window_update_one_menu_action`
  (`gnc-main-window.cpp:1845-1900`, `g_menu_item_new (data->label, ...)` at
  line 1889).

### 2.3 Availability is a state table in the desktop too

The desktop keeps three lists next to the menu actions
(`gnc-main-window.cpp:327-357`): `always_insensitive_actions`
(`FilePrintAction`), `initially_insensitive_actions` (`FileCloseAction`) and
`always_hidden_actions` (`ViewSortByAction`, `ViewFilterByAction`). A page that
has meaning for one of them overrides it. This is the desktop's own idiom for
"present in the menu structure, not available here", and the web tier's
disabled-with-a-reason rule (§5) is the same idea made visible.

### 2.4 Labels the UI file does not contain

A few page plugins put an action name in `label` and replace it at run time,
because the label depends on the document type: the invoice page sets
`_Edit Invoice` / `_Edit Bill` / `_Edit Voucher` / `_Edit Credit Note`
(`gnucash/gnome/gnc-plugin-page-invoice.cpp:190-199`, and the same table again
for bills and vouchers). The appendix marks these as `[runtime: …]` rather
than quoting the action name as if it were a label.

### 2.5 What the tree does *not* contain

Read against the common memory of GnuCash, these absences are the ones that
matter, and each is why the web menu has no such item:

- **No Undo/Redo.** There is no `EditUndoAction`/`EditRedoAction` anywhere in
  the UI or the code; `EditPlaceholder0` (the slot before Cut/Copy/Paste,
  `gnc-main-window.ui:96-100`) is never filled. GnuCash has no undo facility,
  so the web menu has no Undo/Redo to disable — adding one would be an
  invention. (Grep for `Undo` finds only a tooltip in
  `gnc-plugin-log-replay.ui:8`, where it says replay cannot be undone.)
- **No Help > Manual entry.** `HelpPlaceholder0` is empty; the two live Help
  entries are Tutorial and Concepts Guide (`Ctrl+H`) and Contents (`F1`),
  plus About (`gnc-main-window.ui:492-522`).
- **`ActionsPlaceholder0` and `WindowsPlaceholder0/1` are empty.** Actions
  starts with Online Actions rather than a generic New item; Windows starts
  with New Window.

## 3. The inventory

Appendix A is the complete inventory: A.1 the twelve top-level menus in the
order the main window declares them, A.2 every item by menu with its label as
declared, accelerator, scope and `file:line`, A.3 the positions nothing fills,
A.4 where every position is filled. This section is the summary you need
before reading it.

### 3.1 The twelve top-level menus

From `gnucash/ui/gnc-main-window.ui:6-525`, in this order, with the mnemonic
the UI file declares and whether the submenu carries
`hidden-when="action-disabled"`:

| # | menu | mnemonic | hidden when empty | desktop declares |
|---|---|---|---|---|
| 1 | File | Alt+F | no | 7 positions |
| 2 | Edit | Alt+E | no | 7 positions |
| 3 | View | Alt+V | no | 6 positions |
| 4 | Transaction | Alt+N | **yes** | 5 positions, register page only |
| 5 | Actions | Alt+A | no | 7 positions |
| 6 | Business | Alt+B | no | 4 positions |
| 7 | Scheduled | Alt+S | **yes** | 1 position, SX page only |
| 8 | Reports | Alt+R | no | 2 positions |
| 9 | Tools | Alt+T | no | 3 positions |
| 10 | Extensions | Alt+X | **yes** | 1 position, test actions only |
| 11 | Windows | Alt+W | no | 2 positions + generated items |
| 12 | Help | Alt+H | no | 2 positions |

The three conditional menus are how the desktop hides page-scoped menus:
Transaction exists only with a register page, Scheduled only with the
scheduled-transactions page, Extensions only when the business plugin's test
actions are loaded.

### 3.2 Which items are page-scoped

Scope is decided by which file declares the item. A `gnc-plugin-page-*.ui`
file is merged while that kind of page is open, so every item it declares is
page-scoped; a `gnc-plugin-*.ui` that is not a page plugin is merged into
every window and is global. The merge lists are the second confirmation:

| page plugin | merge list |
|---|---|
| register | `gnc-plugin-page-register.cpp:297-314` |
| report | `gnc-plugin-page-report.cpp:220-227` |
| account tree | `gnc-plugin-page-account-tree.cpp:222` |
| invoice | `gnc-plugin-page-invoice.cpp:135` |
| budget | `gnc-plugin-page-budget.cpp:144` |
| owner tree | `gnc-plugin-page-owner-tree.cpp:171` |
| scheduled transactions | `gnc-plugin-page-sx-list.cpp:151` |

The page items that shape the shared menus are the ones a reader will
recognise: Transfer…, Reconcile…, Auto-clear…, Stock Assistant…, Stock
Split…, View Lots… on the register and accounts pages; Edit Account, Delete
Account, Find Account, Cascade Account Properties…, Renumber Subaccounts… on
the accounts page; the invoice entry items; the budget items; the four
Scheduled items. All of them are in Appendix A.2 with their page.

### 3.3 Reports is the report list

`ReportsPlaceholder0` is empty in the UI files. The menu is built from the
report definitions (`gnc-plugin-menu-additions.c:516`), so the desktop's
Reports menu *is* the list of reports, grouped by the definitions' own paths.
The static page items — `Save Report Configuration` (`Ctrl+Alt+S`) and
`Save Report Configuration As…` (`Ctrl+Alt+Shift+S`) — come from the report
page (`gnc-plugin-page-report.ui:72-81`).

### 3.4 Accelerators

Every accelerator in Appendix A is the UI file's `<attribute name="accel">`,
written in GTK's notation (`<Primary>` → `Ctrl`). The web tier shows them as
hints, not bindings — §4.9.

### 3.5 Where this document follows the tree over the published manual

The manual is not in this tree (§1), and this tree is a development branch.
Where the two could differ, the tree wins because it is what the application
actually builds. Concretely:

- GnuCash releases have had menu items the manual documents and this tree no
  longer declares (or never did in this branch) — most visibly no Undo/Redo
  anywhere, no Help > Manual, and Extensions carrying only developer test
  actions. The web menu reflects the tree: it has no Undo/Redo and no Manual.
- The tree's `ActionsPlaceholder0` is empty, so Actions opens with Online
  Actions. The web menu does the same.
- Two items exist only as *generated* menus: the report tree and recent files.
  Appendix A says so where they would be, and the web mapping (§4.6, §4.7)
  treats them as generated rather than as fixed labels.

## 4. The mapping to the web

### 4.1 The rules used

1. **The label is the desktop's label.** No web-only synonym and no stand-in
   ("Print report" for "Print" is not a substitute; it is another item).
2. **An item works only if the web tier can do the same thing**, not something
   similar. Refresh re-reads the page's data; Find focuses the report search,
   which is a changed meaning and is recorded as one (§4.8).
3. **An item that cannot work keeps its label and is disabled with a reason**
   naming the missing capability (§5).
4. **Page-scoped items keep their page.** The reason says which page they
   belong to; they become available when that page exists, without a table
   edit (§7).
5. **Web-only surfaces are marked as such.** Three items are the web tier's
   own, not the desktop's: Report List, New Report…, Report Designer. They
   carry a small `web` badge in the menu and are listed in §4.2.

### 4.2 The table, counted

The single table is `MENU_TABLE` in `web/ui/index.html`. It renders 12 menus
and 196 leaf items (submenu headings are not counted). Measured by the test on
a loaded page with a report selected: **9 items can act, 187 are disabled with
a reason.**

| menu | leaf items | can act today |
|---|---|---|
| File | 27 | — |
| Edit | 33 | Find… (and Report Options with a report selected) |
| View | 23 | Summary Bar, Refresh, Report Designer* |
| Transaction | 14 | — |
| Actions | 42 | — |
| Business | 30 | — |
| Scheduled | 4 | — |
| Reports | 7 | Report List*, New Report…*, Save Report Configuration As… |
| Tools | 8 | — |
| Extensions | 2 | — |
| Windows | 2 | — |
| Help | 4 | About |

`*` marks the three web-tier items. Everything else is a desktop item, named
as the desktop names it, disabled with a reason.

### 4.3 File

The whole menu is hypothetical in a read-only, single-book server: New File,
Open…, Save, Save As…, Revert, Close, Quit, Properties, Page Setup, Print,
the nine Import items and the five Export items all have no web path. Their
reasons are specific rather than "disabled": Open… says the book is a
command-line argument; Save says the session is read-only; Print says the
report is an HTML document in a frame, so printing is the browser's own;
Quit says the process is not the page.

Two File items are page-scoped and are shown that way: Print Checks… (register)
and Print Report… / Export as PDF… / Export Report… (report).

### 4.4 Edit

Cut/Copy/Paste are disabled even though the browser has them: the desktop's
items act on its register widget, and the web tier has no register and no
editable cell. The page-scoped account, invoice, budget and owner-tree items
are all present and disabled with their page. Find… works, with a changed
meaning (§4.8). Report Options works when a report is selected: it scrolls to
the options form the page already shows. Preferences, Style Sheets and Tax
Report Options are server/book-level and have no endpoint.

### 4.5 View

Summary Bar is a real toggle of the summary line; Refresh is real; Report
Designer is the web tier's own toggle. Toolbar, Status Bar and Tab Position
describe chrome the web tier does not have and are disabled. The page-scoped
view items (ledger styles, Sort By/Filter By, invoice Sort Order, Save layout
as default) are present and disabled with their page.

### 4.6 Transaction, Actions, Business, Scheduled

These are the menus whose desktop items presume a page or a write path.

- **Transaction** is register-only and the desktop hides it without one. The
  web keeps it visible, with a note saying "Register page only" and all 14
  items disabled, because the shape of the application is part of what the
  menu is for (§5.3).
- **Actions** has three global submenus that need channels the web lacks
  (Online Actions → aqbanking, Scheduled Transactions → a book editor, Budget
  → a budget editor), then the register/accounts actions
  (Transfer…, Reconcile…, Auto-clear…, Stock Assistant…, **Stock Split…**,
  View Lots…), the invoice entry actions, Check & Repair, Reset Warnings and
  Rename Page. Stock Split… is the case the brief names: it keeps that label,
  is disabled, and says a stock split writes to the book and the web tier has
  no register page.
- **Business** is the largest honest gap. Invoices *do* exist in the web tier,
  but as report documents rendered by the engine; the desktop's Business menu
  is about editing owners, jobs, payments and tax tables, none of which the
  web tier has. Every Business item is present and disabled; the reason says
  why, per group: no customer/vendor/employee data, no dialogs, the book is
  read-only, invoices appear only as documents. `New Invoice…`, `Find
  Invoice…` and `Invoices Due Reminder` are disabled for the same reason —
  they open an editor the web tier does not have, not the report that does.
- **Scheduled** is the scheduled-transactions page's menu, disabled with that
  page named.

### 4.7 Windows and the generated menus

Windows has New Window and New Window with Page, both disabled because the
browser owns windows and tabs and a page cannot enumerate them. The desktop's
per-window items are generated; the web has one document, so no generated
entry is invented.

The recent-files section under File is generated per recent file
(`gnc-plugin-file-history.c:425`). The web server has no recent-books list —
the book is its command-line argument — so there are no entries, and no
"Recent Books" placeholder label was invented to represent them.

### 4.8 The items that change meaning

- **Find…** (`Ctrl+F`): the desktop opens the Find Transactions dialog. The web
  focuses the report search box, which filters report names and tags, not
  transactions. The label and the shortcut hint are kept, the target is not;
  the menu's `aria-description` and this document are where that is said. It
  is the clearest case of an enabled item whose meaning changed, and it is the
  closest thing the page has to a find command.
- **Report Options**: the desktop opens the report's options dialog. The web
  scrolls to the options form it already shows for the selected report.
- **Save Report Configuration As…**: the desktop saves the current report
  page's options under a name. The web submits the designer's form, which is
  the same act on the same engine, for a spec. `Save Report Configuration`
  (without As…) is disabled because a saved spec is immutable in the web tier:
  saving always forks, and pretending otherwise would be the silent no-op the
  honesty rule forbids.
- **About**: see §4.10.

### 4.9 Accelerators are hints, not bindings

The desktop's accelerators are shown next to items because they are part of
what the menu says. They are **not bound**: `Ctrl+N` belongs to the browser
(new window), `Ctrl+P` opens the browser's print, `Ctrl+R` reloads the page
rather than the report, `Ctrl+H` opens history. Binding them would either
fight the browser or silently do the wrong thing, so the page binds only the
menu keys it owns (Escape, arrows, Home/End, Enter/Space, Tab — §6.3) and the
menu bar's `aria-description` says the shortcuts shown are the desktop's.
This is a deliberate downgrade from the desktop and belongs to the keyboard
workstream in `doc/web-ui-roadmap.md`.

### 4.10 About, and the version

The desktop's About dialog contains the copyright, the authors, **Version**
(`gnc_version()`), **Build ID** (`gnc_build_id()`) and the Finance::Quote
version (`gnucash/gnome-utils/gnc-main-window.cpp:5531-5557`, version lines
5548-5551).

`gnc-webd` publishes no version at all. It exposes no version route, no
`Server` header, and `/api/specs` returns only `specs` and `builtin`
(`web/server/report-spec.cpp:2408-2435`); the store's `"version"` field exists
only in the file on disk (`report-spec.cpp:1411`). The web dialog therefore
says:

> Version: not published by this server

and explains, in the dialog, that the desktop reads the version and build ID
from its build and that the page will not guess. If the server later reports a
report-store version through `/api/specs`, the page shows it: the code reads
the field when it is present. Showing a GnuCash release number here would
have been the one invented number in the whole menu, so it is absent.

### 4.11 What could have been approximated, and was not

Print, Page Setup, Cut/Copy/Paste and Quit all have browser-level
approximations (`window.print()`, the clipboard, closing the tab). They are
disabled anyway because the approximation is a *different action*: printing
the page is not printing the active page's report, and `window.print()` on a
page with a report in a frame prints the whole document. The reason text says
what the browser does instead, so a user is not left guessing.

## 5. The honesty rule

### 5.1 The rule

An item that cannot work is **disabled with a visible reason**. It is not
silently inert, and it is not removed. The menu shows the shape of the
application — what GnuCash offers, where — while never pretending an action
works.

### 5.2 How it is enforced

- The disabled item is a real, focusable `menuitem` with
  `aria-disabled="true"`, `title="<reason>"` and `aria-description="<reason>"`.
  It is not a native `disabled` control: a native-disabled button cannot be
  focused, so a keyboard or screen-reader user could never reach the reason.
  Playwright agrees it is not enabled — the test has to dispatch the click
  directly to prove the page ignores it.
- Activating it does nothing and leaves the menu open (`activateMenuItem`
  returns before running anything).
- The reason names the missing capability, with the page for page-scoped items
  (`PAGE_REASONS`), the state for state-dependent items (`MENU_NEEDS`), or the
  specific gap for the rest. "Unavailable" alone is not used.
- Every rendered disabled item is checked: the test counts items without a
  reason and requires zero (§6.5).
- Whole menus that are page-scoped carry a note at the top (Transaction,
  Scheduled) or describe their state (Extensions), and the menu's title has
  the same sentence as its `aria-description`.

### 5.3 Why the web shows menus the desktop hides

The desktop hides Transaction, Scheduled and Extensions when they have no
actions (`hidden-when="action-disabled"`, §3.1). The web tier does the
opposite on purpose: the brief asks the menu to show the shape of the
application, and a hidden menu teaches nothing. The note at the top of each
such menu states the page it belongs to, so the reason is visible rather than
inferred.

### 5.4 What the rule forbids, and where the line was drawn

- No substitute labels. Stock Split stays Stock Split.
- No enabled item that silently does nothing. `Save Report Configuration`
  would have been one (it has no update path), so it is disabled.
- No invented facts. About says the version is not published rather than
  printing one.
- No invented menu entries for generated menus. There is no "Recent Books"
  item, because the desktop's recent list is generated and the web has none.

## 6. The mechanism

### 6.1 One table

`MENU_TABLE` is an array of menus; each menu has `id`, `label`, `mnemonic`,
an optional `note`, and `items`. An item is:

```js
{ id, label, accel,            // id only where a test or a page needs a handle
  reason,                      // present => disabled, and the reason is shown
  page,                        // a page module id => disabled until present()
  needs,                       // a MENU_NEEDS key => disabled until it holds
  run,                         // the function that performs the action
  scope: "web",                // marks the web tier's own items
  items: [ ... ] }             // a submenu
```

`{ sep: true }` is a separator. The renderer builds the DOM once at load, and
recomputes enabled state and the reason text whenever a menu opens and after
every action. Labels and hints therefore cannot drift from the table.

### 6.2 Rendering

`renderMenus()` creates `#menubar > .menu > .menu-title + .menu-popup`,
recursively builds submenus, and keeps a map from each item button to its
table entry. State predicates are evaluated there, not scattered through the
page.

### 6.3 Keyboard

- Roving `tabindex` on the top-level titles: one tab stop for the whole bar.
- On a title: ArrowDown/Enter/Space opens and focuses the first item;
  ArrowLeft/Right moves between menus (opening the next if one is open);
  Home/End jump; Escape closes.
- In a menu: ArrowUp/Down move (wrapping), Home/End, ArrowRight opens a
  submenu or moves to the next menu, ArrowLeft closes a submenu back onto its
  trigger or moves to the previous menu, Enter/Space activates or opens,
  Escape closes the submenu or the menu and returns focus to the title.
- Tab is trapped while a menu is open: focus cycles within the open menu
  rather than escaping to the page behind it.
- Clicking outside closes; hovering a title while a menu is open switches.

### 6.4 ARIA

`role="menubar"` on the bar, `role="menuitem"` with `aria-haspopup` and
`aria-expanded` on the titles, `role="menu"` with `aria-labelledby` on each
popup, `role="menuitem"` (or `menuitemcheckbox` for the two toggles, with
`aria-checked`) on the items, `role="separator"` on the rules. Disabled items
carry `aria-disabled` and their reason in `title`/`aria-description`. The
menu's note is `role="presentation"`, duplicated on the title's
`aria-description` so the reason is announced.

### 6.5 What the tests check

The suite grows from 125 checks to 143; the menu adds 18:

1. the twelve top-level menus, in the desktop's order;
2. menubar/menu/menuitem roles, all popups closed, one tab stop;
3. opening File shows the desktop's items and both submenus;
4. Escape closes and returns focus to the title;
5. ArrowDown opens on the first item, arrows move between items;
6. ArrowRight/Left move between top-level menus;
7. a submenu opens into its first item, Escape returns to the trigger;
8. Stock Split… keeps its label, is disabled, and states why;
9. activating it does nothing and leaves the menu open;
10. a page-scoped menu (Transaction) shows its items disabled and names its
    page;
11. Find… focuses the report search box;
12. Summary Bar hides/shows the summary and reports its checked state;
13. Report Designer hides/shows the designer;
14. Report List focuses the report list;
15. About opens, states the version situation, closes with Escape;
16. Save Report Configuration As… saves a report server-side and the list
    shows it;
17. every disabled item in the rendered bar carries a reason (and the counts
    are printed: 196 items, 9 available, 187 disabled);
18. a page module adds an item before a named base item without editing the
    base table.

The suite is run three times; each run must exit 0 with no console errors and
no failed requests.

## 7. Extensibility

Page modules live in `MENU_PAGES`. A module says whether its page is present
and may contribute items to existing menus:

```js
MENU_PAGES.register = {
    present: () => registerPage !== null,   // flips the page-scoped items live
    menus: {
        // appended to the menu
        reports: [{ label: "Account Report", page: "register",
                    run: openAccountReport }],
        // or inserted before an item the base table already has
        view: { before: "refresh",
                items: [{ label: "Sort By…", page: "register",
                          run: openSortDialog }] },
    },
};
```

Two things follow without editing `MENU_TABLE`:

- The page-scoped items already in the base table (`page: "register"` and the
  other page ids) become enabled as soon as `present()` returns true, and are
  disabled with the page's reason until then.
- Genuinely new items can be added, appended or inserted before a named item,
  in the desktop's order.

The test injects a module, re-renders, checks the item appears before
`Refresh`, removes the module and checks it is gone — so the extension point
is exercised, not just described.

## 8. What is not in this work

- **Styling and the design system.** The menu adds about fifty lines of CSS
  for structure (placement, focus, disabled colour, the dialog); type scale,
  colour roles, focus states, dark mode and component inventory are
  workstream 3 in `doc/web-ui-roadmap.md` and are untouched.
- **Bindings for the desktop accelerators.** Hints only (§4.9); the keyboard
  workstream owns real bindings and a shortcut reference.
- **The write path.** Everything File/Edit/Business would need — saving,
  imports, owners, invoices as editable documents — is absent, and this menu
  does not pretend otherwise.
- **Server endpoints.** No route was added for version, preferences, imports,
  exports, prices or document links. The menu is UI-only, which is why the
  existing server gates are unaffected.
- **Translations.** The web page's strings are English; the desktop's labels
  are translated in `po/`, but the web tier has no translation layer yet. The
  labels here are the desktop's English labels.
- **A register page.** Transaction, and the page-scoped halves of Edit, View,
  Actions and Reports, are waiting for it; the module hook is where it will
  attach.

## Appendix A — the inventory, as the code declares it

Every entry below is a line in a UI file or a `file:line` in the code that builds a menu, not a recollection. Read A.1 for the bar, A.2 for the items, A.3 for the positions nothing fills, and A.4 for where each position is filled. §2 explains why the same label can occur more than once.

### A.1 Top-level menus, in the order the main window declares them

Source: `gnucash/ui/gnc-main-window.ui:6-525` (`<menu id="mainwin-menu">`). “Hidden when disabled” is the submenu's own `hidden-when` attribute: with no actions merged into it the menu does not appear at all.

| # | label as declared | mnemonic | hidden when disabled | placeholder positions |
|---|---|---|---|---|
| 1 | `_File` | Alt+F | no | `FilePlaceholder0`, `FilePlaceholder1`, `FilePlaceholder2`, `FilePlaceholder3`, `FilePlaceholder4`, `FilePlaceholder5`, `FilePlaceholder6` |
| 2 | `_Edit` | Alt+E | no | `EditPlaceholder0`, `EditPlaceholder1`, `EditPlaceholder2`, `EditPlaceholder3`, `EditPlaceholder4`, `EditPlaceholder5`, `EditPlaceholder6` |
| 3 | `_View` | Alt+V | no | `ViewPlaceholder0`, `ViewPlaceholder1`, `ViewPlaceholder2`, `ViewPlaceholder3`, `ViewPlaceholder4`, `ViewPlaceholder5` |
| 4 | `Tra_nsaction` | Alt+N | yes | `TransPlaceholder0`, `TransPlaceholder1`, `TransPlaceholder2`, `TransPlaceholder3`, `TransPlaceholder4` |
| 5 | `_Actions` | Alt+A | no | `ActionsPlaceholder0`, `ActionsPlaceholder1`, `ActionsPlaceholder2`, `ActionsPlaceholder3`, `ActionsPlaceholder4`, `ActionsPlaceholder5`, `ActionsPlaceholder6` |
| 6 | `_Business` | Alt+B | no | `BusinessPlaceholder0`, `BusinessPlaceholder1`, `BusinessPlaceholder2`, `BusinessPlaceholder3` |
| 7 | `_Scheduled` | Alt+S | yes | `SchedulePlaceholder0` |
| 8 | `_Reports` | Alt+R | no | `ReportsPlaceholder0`, `ReportsPlaceholder1` |
| 9 | `_Tools` | Alt+T | no | `ToolsPlaceholder0`, `ToolsPlaceholder1`, `ToolsPlaceholder2` |
| 10 | `E_xtensions` | Alt+X | yes | `ExtensionsPlaceholder0` |
| 11 | `_Windows` | Alt+W | no | `WindowsPlaceholder0`, `WindowsPlaceholder1` |
| 12 | `_Help` | Alt+H | no | `HelpPlaceholder0`, `HelpPlaceholder1` |

### A.2 Every item, menu by menu

The lists below are the declaration, not a screenshot. A `PlaceholderN` item is a *position*: each plugin that names it inserts its own section there (`gnucash/gnome-utils/gnc-main-window.cpp:3567-3579`). Several plugins can insert at the same position (all six page plugins and `gnc-plugin-basic-commands.c` name `EditPlaceholder3`), so the same label can appear more than once here; at run time the window holds the sections of the plugins that are loaded, and only the open page's plugin is loaded from its page file. Labels are quoted raw: `_` marks the mnemonic and `…` is part of the label. Values in `[brackets]` are labels the plugin sets at run time, not literals in the UI file. Scope `global` means the plugin is merged into every window; `page: …` means the item is only present while that kind of page is open.

#### File

- `New _File` · `Ctrl+N` · global · `gnucash/ui/gnc-plugin-basic-commands.ui:5`
- `_Open…` · `Ctrl+O` · global · `gnucash/ui/gnc-plugin-basic-commands.ui:11`
- **_Import** ▸
  - `Import Bills & _Invoices…` · global · `gnucash/ui/gnc-plugin-bi-import.ui:5`
  - `Import _Accounts from CSV…` · global · `gnucash/ui/gnc-plugin-csv-import.ui:5`
  - `Import _Transactions from CSV…` · global · `gnucash/ui/gnc-plugin-csv-import.ui:10`
  - `Import _Prices from a CSV file…` · global · `gnucash/ui/gnc-plugin-csv-import.ui:15`
  - `Import _Customers & Vendors…` · global · `gnucash/ui/gnc-plugin-customer-import.ui:5`
  - `_Replay GnuCash .log file…` · global · `gnucash/ui/gnc-plugin-log-replay.ui:5`
  - `Import _QIF…` · global · `gnucash/ui/gnc-plugin-qif-import.ui:5`
  - `Import using AqBanking` · global · `gnucash/import-export/aqb/gnc-plugin-aqbanking.ui:5`
  - `Import _OFX/QFX…` · global · `gnucash/import-export/ofx/gnc-plugin-ofx.ui:5`
- `_Save` · `Ctrl+S` · global · `gnucash/ui/gnc-plugin-basic-commands.ui:20`
- `Save _As…` · `Ctrl+Shift+S` · global · `gnucash/ui/gnc-plugin-basic-commands.ui:26`
- `Re_vert` · global · `gnucash/ui/gnc-plugin-basic-commands.ui:32`
- `_Print` · `Ctrl+P` · global · `gnucash/ui/gnc-plugin-basic-commands.ui:40`
- `_Print` · `Ctrl+P` · page: account tree · `gnucash/ui/gnc-plugin-page-account-tree.ui:5`
- `_Print` · `Ctrl+P` · page: budget · `gnucash/ui/gnc-plugin-page-budget.ui:5`
- `_Print` · `Ctrl+P` · page: invoice · `gnucash/ui/gnc-plugin-page-invoice.ui:5`
- `_Print` · `Ctrl+P` · page: customer / vendor / employee · `gnucash/ui/gnc-plugin-page-owner-tree.ui:5`
- `_Print Checks…` · `Ctrl+P` · page: register · `gnucash/ui/gnc-plugin-page-register.ui:5`
- `_Print Report…` · `Ctrl+P` · page: report · `gnucash/ui/gnc-plugin-page-report.ui:5`
- `_Print` · `Ctrl+P` · page: scheduled transactions · `gnucash/ui/gnc-plugin-page-sx-list.ui:5`
- `Pa_ge Setup` · `Ctrl+Shift+P` · global · `gnucash/ui/gnc-main-window.ui:39`
- `Export as P_DF…` · page: report · `gnucash/ui/gnc-plugin-page-report.ui:55`
- **_Export** ▸
  - `Export _Accounts` · global · `gnucash/ui/gnc-plugin-basic-commands.ui:50`
  - `Export Account T_ree to CSV…` · global · `gnucash/ui/gnc-plugin-csv-export.ui:5`
  - `Export _Transactions to CSV…` · global · `gnucash/ui/gnc-plugin-csv-export.ui:10`
  - `Export A_ctive Register to CSV…` · global · `gnucash/ui/gnc-plugin-csv-export.ui:15`
  - `Export _Report…` · page: report · `gnucash/ui/gnc-plugin-page-report.ui:64`
- `Proper_ties` · `Alt+Return` · global · `gnucash/ui/gnc-main-window.ui:61`
- `(one item per recent file, generated)` · global · `gnucash/ui/gnc-plugin-file-history.ui:5`
- `_Close` · `Ctrl+W` · global · `gnucash/ui/gnc-main-window.ui:76`
- `_Quit` · `Ctrl+Q` · global · `gnucash/ui/gnc-main-window.ui:82`

#### Edit

- `Cu_t` · `Ctrl+X` · global · `gnucash/ui/gnc-main-window.ui:101`
- `_Copy` · `Ctrl+C` · global · `gnucash/ui/gnc-main-window.ui:107`
- `_Paste` · `Ctrl+V` · global · `gnucash/ui/gnc-main-window.ui:113`
- `_Edit Account` · `Ctrl+E` · page: account tree · `gnucash/ui/gnc-plugin-page-account-tree.ui:15`
- `_Delete Account` · `Delete` · page: account tree · `gnucash/ui/gnc-plugin-page-account-tree.ui:22`
- `F_ind Account` · `Ctrl+I` · page: account tree · `gnucash/ui/gnc-plugin-page-account-tree.ui:29`
- `_Cascade Account Properties…` · page: account tree · `gnucash/ui/gnc-plugin-page-account-tree.ui:36`
- `_Renumber Subaccounts…` · page: account tree · `gnucash/ui/gnc-plugin-page-account-tree.ui:42`
- `Esti_mate Budget…` · page: budget · `gnucash/ui/gnc-plugin-page-budget.ui:15`
- `_All Periods…` · page: budget · `gnucash/ui/gnc-plugin-page-budget.ui:21`
- `_Delete Budget…` · page: budget · `gnucash/ui/gnc-plugin-page-budget.ui:27`
- `Edit Note` · page: budget · `gnucash/ui/gnc-plugin-page-budget.ui:33`
- `Budget Report` · page: budget · `gnucash/ui/gnc-plugin-page-budget.ui:39`
- `Edit Invoice  [runtime: Edit Bill / Edit Voucher / Edit Credit Note]` · page: invoice · `gnucash/ui/gnc-plugin-page-invoice.ui:15`
- `Duplicate Invoice  [runtime: … Bill / … Voucher]` · page: invoice · `gnucash/ui/gnc-plugin-page-invoice.ui:21`
- `Post Invoice  [runtime: … Bill / … Voucher]` · page: invoice · `gnucash/ui/gnc-plugin-page-invoice.ui:27`
- `Unpost Invoice  [runtime: … Bill / … Voucher]` · page: invoice · `gnucash/ui/gnc-plugin-page-invoice.ui:33`
- `Assign as payment…` · page: register · `gnucash/ui/gnc-plugin-page-register.ui:14`
- `Edit payment…` · page: register · `gnucash/ui/gnc-plugin-page-register.ui:21`
- `Open _Account` · page: account tree · `gnucash/ui/gnc-plugin-page-account-tree.ui:51`
- `Open _SubAccounts` · page: account tree · `gnucash/ui/gnc-plugin-page-account-tree.ui:57`
- `_Edit Account` · `Ctrl+E` · page: register · `gnucash/ui/gnc-plugin-page-register.ui:31`
- `F_ind Account` · `Ctrl+I` · page: register · `gnucash/ui/gnc-plugin-page-register.ui:38`
- `_Find…` · `Ctrl+F` · global · `gnucash/ui/gnc-plugin-basic-commands.ui:58`
- `_Find…` · `Ctrl+F` · page: account tree · `gnucash/ui/gnc-plugin-page-account-tree.ui:66`
- `_Find…` · `Ctrl+F` · page: budget · `gnucash/ui/gnc-plugin-page-budget.ui:48`
- `_Find…` · `Ctrl+F` · page: invoice · `gnucash/ui/gnc-plugin-page-invoice.ui:42`
- `_Find…` · `Ctrl+F` · page: customer / vendor / employee · `gnucash/ui/gnc-plugin-page-owner-tree.ui:64`
- `_Find…` · `Ctrl+F` · page: register · `gnucash/ui/gnc-plugin-page-register.ui:48`
- `_Find…` · `Ctrl+F` · page: report · `gnucash/ui/gnc-plugin-page-report.ui:15`
- `_Find…` · `Ctrl+F` · page: scheduled transactions · `gnucash/ui/gnc-plugin-page-sx-list.ui:15`
- `Pr_eferences` · global · `gnucash/ui/gnc-main-window.ui:142`
- `St_yle Sheets` · global · `gnucash/ui/gnc-plugin-report-system.ui:5`
- `Ta_x Report Options` · global · `gnucash/ui/gnc-plugin-basic-commands.ui:68`
- `Ta_x Report Options` · page: account tree · `gnucash/ui/gnc-plugin-page-account-tree.ui:76`
- `Ta_x Report Options` · page: budget · `gnucash/ui/gnc-plugin-page-budget.ui:58`
- `Ta_x Report Options` · page: invoice · `gnucash/ui/gnc-plugin-page-invoice.ui:52`
- `Ta_x Report Options` · page: customer / vendor / employee · `gnucash/ui/gnc-plugin-page-owner-tree.ui:74`
- `Ta_x Report Options` · page: register · `gnucash/ui/gnc-plugin-page-register.ui:58`
- `_Report Options` · page: report · `gnucash/ui/gnc-plugin-page-report.ui:25`
- `Ta_x Report Options` · page: scheduled transactions · `gnucash/ui/gnc-plugin-page-sx-list.ui:25`
- `Budget _Options…` · page: budget · `gnucash/ui/gnc-plugin-page-budget.ui:69`
- `Ta_x Report Options` · page: report · `gnucash/ui/gnc-plugin-page-report.ui:34`

#### View

- `_Toolbar` · global · `gnucash/ui/gnc-main-window.ui:177`
- `Su_mmary Bar` · global · `gnucash/ui/gnc-main-window.ui:182`
- `Stat_us Bar` · global · `gnucash/ui/gnc-main-window.ui:187`
- **Tab P_osition** ▸
  - `To_p` · global · `gnucash/ui/gnc-main-window.ui:194`
  - `B_ottom` · global · `gnucash/ui/gnc-main-window.ui:200`
  - `_Left` · global · `gnucash/ui/gnc-main-window.ui:206`
  - `_Right` · global · `gnucash/ui/gnc-main-window.ui:212`
- `_Filter By…` · page: account tree · `gnucash/ui/gnc-plugin-page-account-tree.ui:89`
- `_Filter By…` · page: budget · `gnucash/ui/gnc-plugin-page-budget.ui:78`
- `Use as Default Layout for Customer Business items` · page: invoice · `gnucash/ui/gnc-plugin-page-invoice.ui:63`
- `Reset Default Layout for Customer Business items` · page: invoice · `gnucash/ui/gnc-plugin-page-invoice.ui:69`
- `_Filter By…` · page: customer / vendor / employee · `gnucash/ui/gnc-plugin-page-owner-tree.ui:85`
- `_Basic Ledger` · page: register · `gnucash/ui/gnc-plugin-page-register.ui:69`
- `_Auto-Split Ledger` · page: register · `gnucash/ui/gnc-plugin-page-register.ui:77`
- `Transaction _Journal` · page: register · `gnucash/ui/gnc-plugin-page-register.ui:85`
- **Sort _Order** ▸
  - `_Standard` · page: invoice · `gnucash/ui/gnc-plugin-page-invoice.ui:83`
  - `_Date` · page: invoice · `gnucash/ui/gnc-plugin-page-invoice.ui:89`
  - `Date of _Entry` · page: invoice · `gnucash/ui/gnc-plugin-page-invoice.ui:95`
  - `_Quantity` · page: invoice · `gnucash/ui/gnc-plugin-page-invoice.ui:101`
  - `_Price` · page: invoice · `gnucash/ui/gnc-plugin-page-invoice.ui:107`
  - `Descri_ption` · page: invoice · `gnucash/ui/gnc-plugin-page-invoice.ui:113`
- `_Double Line` · page: register · `gnucash/ui/gnc-plugin-page-register.ui:96`
- `_Sort By…` · page: register · `gnucash/ui/gnc-plugin-page-register.ui:106`
- `_Filter By…` · page: register · `gnucash/ui/gnc-plugin-page-register.ui:111`
- `_Refresh` · `Ctrl+R` · page: account tree · `gnucash/ui/gnc-plugin-page-account-tree.ui:97`
- `_Refresh` · `Ctrl+R` · page: budget · `gnucash/ui/gnc-plugin-page-budget.ui:86`
- `_Refresh` · `Ctrl+R` · page: invoice · `gnucash/ui/gnc-plugin-page-invoice.ui:124`
- `_Refresh` · `Ctrl+R` · page: customer / vendor / employee · `gnucash/ui/gnc-plugin-page-owner-tree.ui:93`
- `_Refresh` · `Ctrl+R` · page: register · `gnucash/ui/gnc-plugin-page-register.ui:119`
- `_Refresh` · `Ctrl+R` · page: report · `gnucash/ui/gnc-plugin-page-report.ui:45`
- `_Save layout as default` · page: scheduled transactions · `gnucash/ui/gnc-plugin-page-sx-list.ui:36`
- `_Refresh` · `Ctrl+R` · page: scheduled transactions · `gnucash/ui/gnc-plugin-page-sx-list.ui:42`
- `New Accounts _Page` · global · `gnucash/ui/gnc-plugin-account-tree.ui:5`

#### Transaction

- `Cu_t Transaction` · page: register · `gnucash/ui/gnc-plugin-page-register.ui:129`
- `_Copy Transaction` · page: register · `gnucash/ui/gnc-plugin-page-register.ui:135`
- `_Paste Transaction` · page: register · `gnucash/ui/gnc-plugin-page-register.ui:141`
- `Dup_licate Transaction` · page: register · `gnucash/ui/gnc-plugin-page-register.ui:147`
- `_Delete Transaction` · page: register · `gnucash/ui/gnc-plugin-page-register.ui:153`
- `Remo_ve Other Splits` · page: register · `gnucash/ui/gnc-plugin-page-register.ui:159`
- `_Enter Transaction` · page: register · `gnucash/ui/gnc-plugin-page-register.ui:168`
- `Ca_ncel Transaction` · page: register · `gnucash/ui/gnc-plugin-page-register.ui:174`
- `_Void Transaction` · page: register · `gnucash/ui/gnc-plugin-page-register.ui:183`
- `_Unvoid Transaction` · page: register · `gnucash/ui/gnc-plugin-page-register.ui:189`
- `Add _Reversing Transaction` · page: register · `gnucash/ui/gnc-plugin-page-register.ui:195`
- `_Manage Document Link…` · page: register · `gnucash/ui/gnc-plugin-page-register.ui:204`
- `_Open Linked Document` · page: register · `gnucash/ui/gnc-plugin-page-register.ui:210`
- `Jump to Business item` · page: register · `gnucash/ui/gnc-plugin-page-register.ui:219`

#### Actions

- **_Online Actions** ▸
  - `Get _Balance` · global · `gnucash/import-export/aqb/gnc-plugin-aqbanking.ui:17`
  - `Get _Transactions…` · global · `gnucash/import-export/aqb/gnc-plugin-aqbanking.ui:22`
  - `Issue _SEPA Transaction…` · global · `gnucash/import-export/aqb/gnc-plugin-aqbanking.ui:29`
  - `Issue SEPA I_nternal Transaction…` · global · `gnucash/import-export/aqb/gnc-plugin-aqbanking.ui:35`
  - `_Internal Transaction…` · global · `gnucash/import-export/aqb/gnc-plugin-aqbanking.ui:40`
  - `Issue SEPA Direct _Debit…` · global · `gnucash/import-export/aqb/gnc-plugin-aqbanking.ui:45`
  - `Show _log window` · global · `gnucash/import-export/aqb/gnc-plugin-aqbanking.ui:52`
- **_Scheduled Transactions** ▸
  - `_Scheduled Transaction Editor` · global · `gnucash/ui/gnc-plugin-basic-commands.ui:83`
  - `Since _Last Run…` · global · `gnucash/ui/gnc-plugin-basic-commands.ui:88`
  - `_Mortgage & Loan Repayment…` · global · `gnucash/ui/gnc-plugin-basic-commands.ui:93`
- **B_udget** ▸
  - `_New Budget` · global · `gnucash/ui/gnc-plugin-budget.ui:9`
  - `_Open Budget` · global · `gnucash/ui/gnc-plugin-budget.ui:14`
  - `_Copy Budget` · global · `gnucash/ui/gnc-plugin-budget.ui:19`
  - `_Delete Budget` · global · `gnucash/ui/gnc-plugin-budget.ui:24`
- `New _Account…` · page: account tree · `gnucash/ui/gnc-plugin-page-account-tree.ui:109`
- `New Account _Hierarchy…` · page: account tree · `gnucash/ui/gnc-plugin-page-account-tree.ui:115`
- `_Enter` · page: invoice · `gnucash/ui/gnc-plugin-page-invoice.ui:134`
- `_Cancel` · page: invoice · `gnucash/ui/gnc-plugin-page-invoice.ui:140`
- `_Delete` · page: invoice · `gnucash/ui/gnc-plugin-page-invoice.ui:146`
- `_Blank` · page: invoice · `gnucash/ui/gnc-plugin-page-invoice.ui:152`
- `_Transfer…` · `Ctrl+T` · page: register · `gnucash/ui/gnc-plugin-page-register.ui:228`
- `_Reconcile…` · page: register · `gnucash/ui/gnc-plugin-page-register.ui:235`
- `_Auto-clear…` · page: register · `gnucash/ui/gnc-plugin-page-register.ui:241`
- `Stock Ass_istant…` · page: register · `gnucash/ui/gnc-plugin-page-register.ui:247`
- `Stoc_k Split…` · page: register · `gnucash/ui/gnc-plugin-page-register.ui:253`
- `View _Lots…` · page: register · `gnucash/ui/gnc-plugin-page-register.ui:259`
- `_Transfer…` · `Ctrl+T` · page: account tree · `gnucash/ui/gnc-plugin-page-account-tree.ui:124`
- `_Reconcile…` · page: account tree · `gnucash/ui/gnc-plugin-page-account-tree.ui:131`
- `_Auto-clear…` · page: account tree · `gnucash/ui/gnc-plugin-page-account-tree.ui:137`
- `Stock Ass_istant…` · page: account tree · `gnucash/ui/gnc-plugin-page-account-tree.ui:143`
- `Stoc_k Split…` · page: account tree · `gnucash/ui/gnc-plugin-page-account-tree.ui:149`
- `View _Lots…` · page: account tree · `gnucash/ui/gnc-plugin-page-account-tree.ui:155`
- `Dup_licate Entry` · page: invoice · `gnucash/ui/gnc-plugin-page-invoice.ui:161`
- `Move Entry _Up` · page: invoice · `gnucash/ui/gnc-plugin-page-invoice.ui:167`
- `Move Entry Do_wn` · page: invoice · `gnucash/ui/gnc-plugin-page-invoice.ui:173`
- `_Blank Transaction` · `Ctrl+B` · page: register · `gnucash/ui/gnc-plugin-page-register.ui:268`
- `_Go to Date` · `Ctrl+G` · page: register · `gnucash/ui/gnc-plugin-page-register.ui:275`
- `S_plit Transaction` · page: register · `gnucash/ui/gnc-plugin-page-register.ui:282`
- `Edit E_xchange Rate` · page: register · `gnucash/ui/gnc-plugin-page-register.ui:288`
- `Sche_dule…` · page: register · `gnucash/ui/gnc-plugin-page-register.ui:294`
- `_Jump to the other account` · page: register · `gnucash/ui/gnc-plugin-page-register.ui:300`
- **_Check & Repair** ▸
  - `Check & Repair A_ccount` · page: account tree · `gnucash/ui/gnc-plugin-page-account-tree.ui:168`
  - `Check & Repair Su_baccounts` · page: account tree · `gnucash/ui/gnc-plugin-page-account-tree.ui:174`
  - `Check & Repair A_ll` · page: account tree · `gnucash/ui/gnc-plugin-page-account-tree.ui:180`
- **_Check & Repair** ▸
  - `All Transactions` · page: register · `gnucash/ui/gnc-plugin-page-register.ui:317`
  - `This transaction` · page: register · `gnucash/ui/gnc-plugin-page-register.ui:322`
- `Reset _Warnings…` · global · `gnucash/ui/gnc-main-window.ui:343`
- `Re_name Page` · global · `gnucash/ui/gnc-main-window.ui:348`

#### Business

- **_Customer** ▸
  - `Customers Overview` · global · `gnucash/ui/gnc-plugin-business.ui:8`
  - `_New Customer…` · global · `gnucash/ui/gnc-plugin-business.ui:13`
  - `_Find Customer…` · global · `gnucash/ui/gnc-plugin-business.ui:18`
  - `New _Invoice…` · global · `gnucash/ui/gnc-plugin-business.ui:23`
  - `Find In_voice…` · global · `gnucash/ui/gnc-plugin-business.ui:28`
  - `New _Job…` · global · `gnucash/ui/gnc-plugin-business.ui:33`
  - `Find Jo_b…` · global · `gnucash/ui/gnc-plugin-business.ui:38`
  - `_Process Payment…` · global · `gnucash/ui/gnc-plugin-business.ui:43`
  - `Invoices _Due Reminder` · global · `gnucash/ui/gnc-plugin-business.ui:48`
- **_Vendor** ▸
  - `Vendors Overview` · global · `gnucash/ui/gnc-plugin-business.ui:57`
  - `_New Vendor…` · global · `gnucash/ui/gnc-plugin-business.ui:62`
  - `_Find Vendor…` · global · `gnucash/ui/gnc-plugin-business.ui:67`
  - `New _Bill…` · global · `gnucash/ui/gnc-plugin-business.ui:72`
  - `Find Bi_ll…` · global · `gnucash/ui/gnc-plugin-business.ui:77`
  - `New _Job…` · global · `gnucash/ui/gnc-plugin-business.ui:82`
  - `Find Jo_b…` · global · `gnucash/ui/gnc-plugin-business.ui:87`
  - `_Process Payment…` · global · `gnucash/ui/gnc-plugin-business.ui:92`
  - `Bills _Due Reminder` · global · `gnucash/ui/gnc-plugin-business.ui:97`
- **_Employee** ▸
  - `Employees Overview` · global · `gnucash/ui/gnc-plugin-business.ui:106`
  - `_New Employee…` · global · `gnucash/ui/gnc-plugin-business.ui:111`
  - `_Find Employee…` · global · `gnucash/ui/gnc-plugin-business.ui:116`
  - `New _Expense Voucher…` · global · `gnucash/ui/gnc-plugin-business.ui:121`
  - `Find Expense _Voucher…` · global · `gnucash/ui/gnc-plugin-business.ui:126`
  - `_Process Payment…` · global · `gnucash/ui/gnc-plugin-business.ui:131`
- `Business Linked Documents` · global · `gnucash/ui/gnc-plugin-business.ui:137`
- `Sales _Tax Table` · global · `gnucash/ui/gnc-plugin-business.ui:145`
- `_Billing Terms Editor` · global · `gnucash/ui/gnc-plugin-business.ui:150`
- `Manage Document Link…` · page: invoice · `gnucash/ui/gnc-plugin-page-invoice.ui:182`
- `Open Linked Document` · page: invoice · `gnucash/ui/gnc-plugin-page-invoice.ui:188`
- `Pay Invoice  [runtime: Pay Bill / Pay Voucher]` · page: invoice · `gnucash/ui/gnc-plugin-page-invoice.ui:197`

#### Scheduled

- `_New Schedule` · page: scheduled transactions · `gnucash/ui/gnc-plugin-page-sx-list.ui:52`
- `_Edit Schedule` · page: scheduled transactions · `gnucash/ui/gnc-plugin-page-sx-list.ui:58`
- `_Delete Schedule` · page: scheduled transactions · `gnucash/ui/gnc-plugin-page-sx-list.ui:64`
- `_Run Schedule` · page: scheduled transactions · `gnucash/ui/gnc-plugin-page-sx-list.ui:70`

#### Reports

- `_Company Report` · page: invoice · `gnucash/ui/gnc-plugin-page-invoice.ui:206`
- `Account Report` · page: register · `gnucash/ui/gnc-plugin-page-register.ui:331`
- `Account Report - Single Transaction` · page: register · `gnucash/ui/gnc-plugin-page-register.ui:337`
- `Save _Report Configuration` · `Ctrl+Alt+S` · page: report · `gnucash/ui/gnc-plugin-page-report.ui:73`
- `Save Report Configuration As…` · `Ctrl+Alt+Shift+S` · page: report · `gnucash/ui/gnc-plugin-page-report.ui:80`

#### Tools

- `_Online Banking Setup…` · global · `gnucash/import-export/aqb/gnc-plugin-aqbanking.ui:62`
- `_Price Database` · global · `gnucash/ui/gnc-plugin-basic-commands.ui:103`
- `_Security Editor` · global · `gnucash/ui/gnc-plugin-basic-commands.ui:108`
- `_Loan Repayment Calculator` · global · `gnucash/ui/gnc-plugin-basic-commands.ui:113`
- `_Close Book…` · global · `gnucash/ui/gnc-plugin-basic-commands.ui:118`
- `_Import Map Editor` · global · `gnucash/ui/gnc-plugin-basic-commands.ui:123`
- `_Transaction Linked Documents` · global · `gnucash/ui/gnc-plugin-basic-commands.ui:128`
- `_General Journal` · global · `gnucash/ui/gnc-plugin-register.ui:5`

#### Extensions

- **_Business** ▸
  - `Test Search Dialog` · global · `gnucash/ui/gnc-plugin-business.ui:162`
  - `Initialize Test Data` · global · `gnucash/ui/gnc-plugin-business.ui:167`

#### Windows

- `_New Window` · global · `gnucash/ui/gnc-main-window.ui:465`
- `New Window with _Page` · global · `gnucash/ui/gnc-main-window.ui:470`
- `Window0Action  [action name; label set in code]` · global · `gnucash/ui/gnc-main-window.ui:482`

#### Help

- `Tutorial and Concepts _Guide` · `Ctrl+H` · global · `gnucash/ui/gnc-main-window.ui:500`
- `_Tips Of The Day` · global · `gnucash/ui/gnc-plugin-basic-commands.ui:136`
- `_Contents` · `F1` · global · `gnucash/ui/gnc-main-window.ui:511`
- `_About` · global · `gnucash/ui/gnc-main-window.ui:517`

### A.3 Placeholder positions no plugin fills at this commit

These are declared in `gnc-main-window.ui` and never receive a section, so the desktop shows nothing at that position: `ActionsPlaceholder0`, `EditPlaceholder0`, `HelpPlaceholder0`, `ViewPlaceholder0`, `WindowsPlaceholder0`, `WindowsPlaceholder1`.

`ReportsPlaceholder0` is the exception: it is empty in the UI files because the
report menu is built at run time from the report definitions, and the plugin
that builds it searches for exactly that action name
(`gnucash/gnome-utils/gnc-plugin-menu-additions.c:516`). See §2.2 and §3.3.

The `_Scheduled` menu's only position, `SchedulePlaceholder0`, is filled by the scheduled-transactions page plugin (`gnucash/ui/gnc-plugin-page-sx-list.ui:51`); the menu is `hidden-when=action-disabled`, so it appears only while that page is open. `Tra_nsaction` is the same shape, filled only by the register page (`gnucash/ui/gnc-plugin-page-register.ui:128-226`). `E_xtensions` is filled only by the business plugin's test actions (`gnucash/ui/gnc-plugin-business.ui:157`).

### A.4 Where each placeholder position is filled

| placeholder | UI file and line | plugins at this position |
|---|---|---|
| `ActionsPlaceholder1` | `gnucash/import-export/aqb/gnc-plugin-aqbanking.ui:12` | 1 |
| `ActionsPlaceholder2` | `gnucash/ui/gnc-plugin-basic-commands.ui:78` | 1 |
| `ActionsPlaceholder3` | `gnucash/ui/gnc-plugin-budget.ui:4` | 1 |
| `ActionsPlaceholder4` | `gnucash/ui/gnc-plugin-page-account-tree.ui:108` | 3 |
| `ActionsPlaceholder4` | `gnucash/ui/gnc-plugin-page-invoice.ui:133` | 3 |
| `ActionsPlaceholder4` | `gnucash/ui/gnc-plugin-page-register.ui:227` | 3 |
| `ActionsPlaceholder5` | `gnucash/ui/gnc-plugin-page-account-tree.ui:123` | 3 |
| `ActionsPlaceholder5` | `gnucash/ui/gnc-plugin-page-invoice.ui:160` | 3 |
| `ActionsPlaceholder5` | `gnucash/ui/gnc-plugin-page-register.ui:267` | 3 |
| `ActionsPlaceholder6` | `gnucash/ui/gnc-plugin-page-account-tree.ui:163` | 2 |
| `ActionsPlaceholder6` | `gnucash/ui/gnc-plugin-page-register.ui:311` | 2 |
| `BusinessPlaceholder0` | `gnucash/ui/gnc-plugin-business.ui:4` | 1 |
| `BusinessPlaceholder1` | `gnucash/ui/gnc-plugin-business.ui:144` | 1 |
| `BusinessPlaceholder2` | `gnucash/ui/gnc-plugin-page-invoice.ui:181` | 1 |
| `BusinessPlaceholder3` | `gnucash/ui/gnc-plugin-page-invoice.ui:196` | 1 |
| `EditPlaceholder1` | `gnucash/ui/gnc-plugin-page-account-tree.ui:14` | 4 |
| `EditPlaceholder1` | `gnucash/ui/gnc-plugin-page-budget.ui:14` | 4 |
| `EditPlaceholder1` | `gnucash/ui/gnc-plugin-page-invoice.ui:14` | 4 |
| `EditPlaceholder1` | `gnucash/ui/gnc-plugin-page-register.ui:13` | 4 |
| `EditPlaceholder2` | `gnucash/ui/gnc-plugin-page-account-tree.ui:50` | 2 |
| `EditPlaceholder2` | `gnucash/ui/gnc-plugin-page-register.ui:30` | 2 |
| `EditPlaceholder2-c` | `gnucash/ui/gnc-plugin-page-owner-tree.ui:14` | 1 |
| `EditPlaceholder2-e` | `gnucash/ui/gnc-plugin-page-owner-tree.ui:46` | 1 |
| `EditPlaceholder2-v` | `gnucash/ui/gnc-plugin-page-owner-tree.ui:30` | 1 |
| `EditPlaceholder3` | `gnucash/ui/gnc-plugin-basic-commands.ui:57` | 8 |
| `EditPlaceholder3` | `gnucash/ui/gnc-plugin-page-account-tree.ui:65` | 8 |
| `EditPlaceholder3` | `gnucash/ui/gnc-plugin-page-budget.ui:47` | 8 |
| `EditPlaceholder3` | `gnucash/ui/gnc-plugin-page-invoice.ui:41` | 8 |
| `EditPlaceholder3` | `gnucash/ui/gnc-plugin-page-owner-tree.ui:63` | 8 |
| `EditPlaceholder3` | `gnucash/ui/gnc-plugin-page-register.ui:47` | 8 |
| `EditPlaceholder3` | `gnucash/ui/gnc-plugin-page-report.ui:14` | 8 |
| `EditPlaceholder3` | `gnucash/ui/gnc-plugin-page-sx-list.ui:14` | 8 |
| `EditPlaceholder4` | `gnucash/ui/gnc-plugin-report-system.ui:4` | 1 |
| `EditPlaceholder5` | `gnucash/ui/gnc-plugin-basic-commands.ui:67` | 8 |
| `EditPlaceholder5` | `gnucash/ui/gnc-plugin-page-account-tree.ui:75` | 8 |
| `EditPlaceholder5` | `gnucash/ui/gnc-plugin-page-budget.ui:57` | 8 |
| `EditPlaceholder5` | `gnucash/ui/gnc-plugin-page-invoice.ui:51` | 8 |
| `EditPlaceholder5` | `gnucash/ui/gnc-plugin-page-owner-tree.ui:73` | 8 |
| `EditPlaceholder5` | `gnucash/ui/gnc-plugin-page-register.ui:57` | 8 |
| `EditPlaceholder5` | `gnucash/ui/gnc-plugin-page-report.ui:24` | 8 |
| `EditPlaceholder5` | `gnucash/ui/gnc-plugin-page-sx-list.ui:24` | 8 |
| `EditPlaceholder6` | `gnucash/ui/gnc-plugin-page-budget.ui:68` | 2 |
| `EditPlaceholder6` | `gnucash/ui/gnc-plugin-page-report.ui:33` | 2 |
| `ExtensionsPlaceholder0` | `gnucash/ui/gnc-plugin-business.ui:157` | 1 |
| `FilePlaceholder0` | `gnucash/ui/gnc-plugin-basic-commands.ui:4` | 1 |
| `FilePlaceholder1` | `gnucash/import-export/aqb/gnc-plugin-aqbanking.ui:4` | 7 |
| `FilePlaceholder1` | `gnucash/import-export/ofx/gnc-plugin-ofx.ui:4` | 7 |
| `FilePlaceholder1` | `gnucash/ui/gnc-plugin-bi-import.ui:4` | 7 |
| `FilePlaceholder1` | `gnucash/ui/gnc-plugin-csv-import.ui:4` | 7 |
| `FilePlaceholder1` | `gnucash/ui/gnc-plugin-customer-import.ui:4` | 7 |
| `FilePlaceholder1` | `gnucash/ui/gnc-plugin-log-replay.ui:4` | 7 |
| `FilePlaceholder1` | `gnucash/ui/gnc-plugin-qif-import.ui:4` | 7 |
| `FilePlaceholder2` | `gnucash/ui/gnc-plugin-basic-commands.ui:19` | 1 |
| `FilePlaceholder3` | `gnucash/ui/gnc-plugin-basic-commands.ui:39` | 8 |
| `FilePlaceholder3` | `gnucash/ui/gnc-plugin-page-account-tree.ui:4` | 8 |
| `FilePlaceholder3` | `gnucash/ui/gnc-plugin-page-budget.ui:4` | 8 |
| `FilePlaceholder3` | `gnucash/ui/gnc-plugin-page-invoice.ui:4` | 8 |
| `FilePlaceholder3` | `gnucash/ui/gnc-plugin-page-owner-tree.ui:4` | 8 |
| `FilePlaceholder3` | `gnucash/ui/gnc-plugin-page-register.ui:4` | 8 |
| `FilePlaceholder3` | `gnucash/ui/gnc-plugin-page-report.ui:4` | 8 |
| `FilePlaceholder3` | `gnucash/ui/gnc-plugin-page-sx-list.ui:4` | 8 |
| `FilePlaceholder4` | `gnucash/ui/gnc-plugin-page-report.ui:54` | 1 |
| `FilePlaceholder5` | `gnucash/ui/gnc-plugin-basic-commands.ui:49` | 3 |
| `FilePlaceholder5` | `gnucash/ui/gnc-plugin-csv-export.ui:4` | 3 |
| `FilePlaceholder5` | `gnucash/ui/gnc-plugin-page-report.ui:63` | 3 |
| `FilePlaceholder6` | `gnucash/ui/gnc-plugin-file-history.ui:4` | 1 |
| `HelpPlaceholder1` | `gnucash/ui/gnc-plugin-basic-commands.ui:135` | 1 |
| `ReportsPlaceholder1` | `gnucash/ui/gnc-plugin-page-invoice.ui:205` | 3 |
| `ReportsPlaceholder1` | `gnucash/ui/gnc-plugin-page-register.ui:330` | 3 |
| `ReportsPlaceholder1` | `gnucash/ui/gnc-plugin-page-report.ui:72` | 3 |
| `ReportsPlaceholder1-c` | `gnucash/ui/gnc-plugin-page-owner-tree.ui:102` | 1 |
| `ReportsPlaceholder1-e` | `gnucash/ui/gnc-plugin-page-owner-tree.ui:132` | 1 |
| `ReportsPlaceholder1-v` | `gnucash/ui/gnc-plugin-page-owner-tree.ui:117` | 1 |
| `SchedulePlaceholder0` | `gnucash/ui/gnc-plugin-page-sx-list.ui:51` | 1 |
| `ToolsPlaceholder0` | `gnucash/import-export/aqb/gnc-plugin-aqbanking.ui:61` | 1 |
| `ToolsPlaceholder1` | `gnucash/ui/gnc-plugin-basic-commands.ui:102` | 1 |
| `ToolsPlaceholder2` | `gnucash/ui/gnc-plugin-register.ui:4` | 1 |
| `TransPlaceholder0` | `gnucash/ui/gnc-plugin-page-register.ui:128` | 1 |
| `TransPlaceholder1` | `gnucash/ui/gnc-plugin-page-register.ui:167` | 1 |
| `TransPlaceholder2` | `gnucash/ui/gnc-plugin-page-register.ui:182` | 1 |
| `TransPlaceholder3` | `gnucash/ui/gnc-plugin-page-register.ui:203` | 1 |
| `TransPlaceholder4` | `gnucash/ui/gnc-plugin-page-register.ui:218` | 1 |
| `ViewPlaceholder1` | `gnucash/ui/gnc-plugin-page-account-tree.ui:88` | 5 |
| `ViewPlaceholder1` | `gnucash/ui/gnc-plugin-page-budget.ui:77` | 5 |
| `ViewPlaceholder1` | `gnucash/ui/gnc-plugin-page-invoice.ui:62` | 5 |
| `ViewPlaceholder1` | `gnucash/ui/gnc-plugin-page-owner-tree.ui:84` | 5 |
| `ViewPlaceholder1` | `gnucash/ui/gnc-plugin-page-register.ui:68` | 5 |
| `ViewPlaceholder2` | `gnucash/ui/gnc-plugin-page-invoice.ui:77` | 2 |
| `ViewPlaceholder2` | `gnucash/ui/gnc-plugin-page-register.ui:95` | 2 |
| `ViewPlaceholder3` | `gnucash/ui/gnc-plugin-page-register.ui:105` | 1 |
| `ViewPlaceholder4` | `gnucash/ui/gnc-plugin-page-account-tree.ui:96` | 7 |
| `ViewPlaceholder4` | `gnucash/ui/gnc-plugin-page-budget.ui:85` | 7 |
| `ViewPlaceholder4` | `gnucash/ui/gnc-plugin-page-invoice.ui:123` | 7 |
| `ViewPlaceholder4` | `gnucash/ui/gnc-plugin-page-owner-tree.ui:92` | 7 |
| `ViewPlaceholder4` | `gnucash/ui/gnc-plugin-page-register.ui:118` | 7 |
| `ViewPlaceholder4` | `gnucash/ui/gnc-plugin-page-report.ui:44` | 7 |
| `ViewPlaceholder4` | `gnucash/ui/gnc-plugin-page-sx-list.ui:35` | 7 |
| `ViewPlaceholder5` | `gnucash/ui/gnc-plugin-account-tree.ui:4` | 1 |

`mainwin-menu` and the page popup menus (`mainwin-popup*`) are omitted: the first is the bar itself, the others are context menus, not menu-bar items. The reconcile window (`recwin-menu`) and the embedded register (`embeddedwin-menu`) have their own menu bars with their own items; they are different windows, outside this inventory, and their popup menus are context menus.

