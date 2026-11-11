# The Reports page: rail, document, inspector, and the designer's own mode

This is the design record for the Reports page's layout (Option A).  It is a
decision document, not a tutorial: the implementation is `web/ui/index.html`
and the browser checks are `web/test/pages-ui.mjs` and `web/test/ui-test.mjs`.

Related: `doc/web-ui-pages.md` (the page vocabulary and URL scheme),
`doc/web-ui-menu.md` (the menu inventory), `web/DESIGNER.md` (what a report
spec is).  This document changes only the Reports page
(`<base>/reports`, `<base>/report/<name>`, `<base>/report/spec/<id>`); the Book
page and its account tabs are untouched.

## 1. The decision

The Reports page is **three panes and a document**:

```
+-----------------+  +----------------------------------------+  +--------------------+
| Report list     |  | Reports > Trial Balance                |  | Options            |
| [search] [tags] |  | [list] [options] [print] [designer]    |  | (descriptor form)  |
| - templates     |  | +------------------------------------+  |  |                    |
| - saved reports |  | |                                    |  |  |                    |
|                 |  | |        the report document         |  |  |                    |
| New report      |  | |                                    |  |  |                    |
|                 |  | +------------------------------------+  |  |                    |
|                 |  | | 42 rows | 11/11/26 - 11/11/26 | ... |  |                    |
+-----------------+  +----------------------------------------+  +--------------------+
        rail                        document                          inspector
```

The **rail** is always visible at a normal width and scrolls independently: the
existing search box, the existing tag chips (on the report entries), the
templates and the saved reports, with their existing delete controls.  **New
report** moves from the old inline designer section to the rail's header, where
it is reachable before anything is selected.

The **document** is the existing `#view` iframe at the full remaining width and
height.  There is no padding or card around it: the report's own document
already sets its margins (`html,body{margin:0 8px}` in the native report
stylesheet).

The **inspector** is the existing descriptor-driven option form, moved from
under the document into the right pane.  It scrolls on its own; the document
is not pushed down by the height of the form.

Above the document: the **breadcrumb** `Reports › <report name>` (the first
part is a link back to the list) and a small action row (collapse the rail,
collapse the inspector, edit in the designer, print).  Under the document: the
**status bar** with the row count, the date range the frame was actually
rendered with, and whether what is on screen is saved on the server.

The page is a grid whose panes own their scrolling: the rail, the inspector and
the report document scroll independently, and the page's own overflow is only
the small remainder a very short viewport leaves.  This is what makes flow 1
possible without the options form pushing the document down.

## 2. The four flows

### 2.1 Same report, different dates (the common case)

The inspector is beside the document, not below it.  The reader changes `Start`
or `End` (or a period preset) and presses **Update report**.  The form submits
exactly what it submitted before; the change is in how the result arrives:

- the iframe is re-pointed at the new `/report/<name>?…` URL (the same
  document route as before);
- the page's own scroll position is captured before the navigation and restored
  when the new document has loaded, so applying options never jumps the page to
  the top;
- the frame's scroll position is restored the same way.  At the top (the common
  case when the options are visible beside the document) both restores are
  no-ops; when the reader had scrolled either, they stay where they were;
- the status bar's date range is replaced with the values actually sent (or the
  preset's label), and the row count is re-read from the new document once it
  has parsed.

Nothing about the document is lost: the frame is the same element, the
document route is unchanged, and the `srcdoc` preview path is untouched.

### 2.2 Find a report by name or tag

The rail is never hidden by selecting a report, and the report list scrolls
independently of the document.  Typing in **Search** filters the rendered list
client-side (name and tags, as before); a tag chip narrows it to that tag and
clicking the selected chip clears the filter.  The filter note (`n of m
reports`) stays under the search box.

The list is keyboard-reachable: `Ctrl+K` puts the cursor in the search box from
anywhere on the Reports page; `Enter` in the list opens the focused entry; the
up/down arrows move focus between the visible entries (the list is one tab stop
with a roving `tabindex`, like the tab strip).

The search text and the active tag are **not** page state: they narrow a list
that is already on screen and are deliberately not in the URL or in
`sessionStorage`.  A reload shows the full list.  This matches the tab strip's
rule, where only the identity of a page (not a transient filter) is persisted.

### 2.3 Build a report

The designer needs width.  It is a **mode of the Reports page**, not a section
under the document: `<base>/reports?designer=1` (and
`<base>/report/<name>?designer=1`, `<base>/report/spec/<id>?designer=1` when
the mode was entered from an open report).  The mode uses the whole content
area:

```
+----------------------------------+  +-------------------------------------+
| Report designer     [Back]       |  | Live preview                        |
| provenance banner                |  | +---------------------------------+ |
| source / name / tags / columns   |  | |                                 | |
| grouping / chart / accounts ...  |  | |   the report document (srcdoc)  | |
| [Preview] [Save report]          |  | |                                 | |
+----------------------------------+  +-------------------------------------+
        spec form (scrolls)                    #view (same iframe)
```

The live preview is the **same `#view` iframe** the document pane uses, placed
by the grid in the preview column.  This is deliberate: the preview is not a
second renderer and cannot drift from the document; `Preview` posts to
`/api/spec/preview` and puts the returned HTML in the frame as `srcdoc`,
exactly as before.  In addition, edits in the form schedule a debounced
(silent) preview — a failed intermediate spec leaves the last successful
preview on screen instead of replacing it with an error — so the form and its
output are visible together.  The explicit **Preview** button still reports a
rejection in `#spec-error`.

Entering and leaving:

- **New report** in the rail enters the mode with a blank form.
- **Edit in designer** above the document enters the mode for whatever is
  selected: a saved report's definition, a built-in's definition, a native
  report's starting point, or the honest "no designer equivalent" banner.
- **Back to reports** in the designer header leaves the mode and goes to
  `<base>/reports` (the list), by design: the mode is a place you go to build,
  not a fourth column.
- The URL is the mode: a reload lands in the designer with the same report
  selected; Back/Forward leave and re-enter it.

**Unsaved work.**  The designer is dirty once the user has changed a control
since the last population or successful save.  Leaving by the in-page
**Back to reports** (or by View > Report Designer while in the mode) asks for
confirmation when dirty: OK discards and leaves, Cancel stays.  This reuses the
register's guard *pattern* but not its exact policy: the register can save and
continue, while the designer cannot save a form that may be unnamed or invalid
on the reader's behalf, so the guard offers discard-or-stay rather than
save-or-stay.  The browser's own Back/reload/close cannot be refused by an
in-page handler; that limit is stated in §6 rather than hidden.

All existing designer behaviour is kept: the source picker and per-source
fields, the provenance banner (definition / starting point / none), Save as…
for a built-in or saved report, tags, and preview parity with the built-ins
(the browser suites still compare the preview's cells with the report's own
document).

### 2.4 Print, or hand the document to someone

The document is an HTML document in a frame, not a server-rendered page with
the application's chrome.  The **Print** action prints the frame's own
document (`#view.contentWindow.print()`), so the browser's print dialog sees
the report alone, not the rail, inspector, breadcrumb or menu bar.  This is the
honest web equivalent of the desktop's Print Report.

What the existing routes already give, and what they do not:

- `/report/<name>` and `/report/spec/<id>` answer the **report document** to a
  subresource request (the iframe, `curl`, a harness), and the **application
  shell** to a top-level navigation.  That split is recorded in
  `doc/web-ui-pages.md` §2.1 and is unchanged.
- Therefore copying the iframe's `src` into a new browser tab gives the shell,
  not the document.  The reliable ways to hand someone the document are the
  iframe/embed route, a `fetch`/`curl` of the same URL, or the browser's own
  "print" (which can save as PDF).  There is no PDF endpoint and none is added:
  writing a file is a desktop action (`doc/web-ui-menu.md`, File > Export as
  PDF…).
- The **Print** button is about the document alone; it does not try to print
  the application.

## 3. Panes: sizes, collapse rules, remembered widths

| pane | default width | minimum | maximum | remembered |
|---|---|---|---|---|
| rail | 17rem (272px) | 11rem | 40% of the viewport | yes, `sessionStorage` |
| inspector | 22rem (352px) | 14rem | 40% of the viewport | yes, `sessionStorage` |

- Both panes are **collapsible** with their header button or `[` / `]`.  A
  collapsed pane takes zero width; its grid column and the splitter between it
  and the document are removed, so the document immediately takes the space.
- Both panes are **resizable** by dragging the splitter between the pane and
  the document, or by focusing the splitter and pressing Left/Right (16px a
  press).  The new width is written to `sessionStorage` when the drag ends or
  the key is released.
- Widths and collapsed flags are view preferences, not page state.  They live
  in one `sessionStorage` record, `gnucash-webui.reports-panes.v1`, scoped to
  the browser tab, for the same reason the open register tab strip does: the
  URL names what is on screen, not how wide its furniture is.
- At a **narrow window** the page collapses the rail first, at `<= 1100px`,
  and then the inspector, at `<= 860px`.  Both toggles stay visible but say
  (via `disabled`/`title`) that the window is too narrow to show the pane;
  the document keeps its width, and at 800px it is the only column.  The
  breakpoints are `matchMedia` queries, not a fixed assumption that every
  reader uses a wide window.

## 4. Keyboard map

| key | where | action |
|---|---|---|
| `Ctrl+K` | Reports page, not in a text field | focus the report search box |
| `Enter` | report list | open the focused report |
| `Up` / `Down` | report list | move focus between visible entries |
| `[` | Reports page, not in a text field | collapse/show the rail |
| `]` | Reports page, not in a text field | collapse/show the inspector |
| `Left` / `Right` | a pane splitter | resize that pane by 16px |
| `Escape` | designer mode | ask to leave (only when dirty) / leave |

Keys the browser owns are not bound.  `Ctrl+K` is the one browser-ish key the
brief fixes; it is bound only on the Reports page and only when focus is not
already in an editable control, and it prevents the browser's own handling
only on that keypress.  `Ctrl+P` is left to the browser; the Print button calls
the frame's own `print()` rather than pretending the page can drive the
browser's print dialog.  `Ctrl+Tab`, `Ctrl+W`, `Ctrl+T`, `Ctrl+L` and the other
browser tab/address shortcuts are untouched.

## 5. URL and state

The rule is still `doc/web-ui-pages.md` §4: **the URL is the page state**.

| state | lives in | why |
|---|---|---|
| which page (Book vs Reports) | URL path | a page is a URL |
| which report (native, saved, built-in) | URL path | an open report is a URL |
| designer mode | URL query `?designer=1` | a mode of the page; reload/Back/Forward restore it |
| search text and active tag | memory only | a transient filter over a list already on screen |
| pane widths and collapsed flags | `sessionStorage` | furniture, like the open tab strip |
| list scroll position | browser's own scroll position | not state the page owns |
| last applied options (status bar dates) | memory, derived from the frame URL | it describes the document on screen, not the form's unsaved edits |

`?designer=1` is used rather than `<base>/reports/designer` because the page
already has three report-bearing paths (`reports`, `report/<name>`,
`report/spec/<id>`) and the mode applies to all of them; a query keeps one
canonical path per resource and needs no new server route.  `parseRoute()`
reads the query alongside the path; `setUrl()` writes the query when the mode
is entered and drops it when the mode is left.  Unknown queries are ignored.

## 6. What is explicitly not being done

- **No report gallery page.**  The rail is the list; there is no separate
  browse-by-tag or card grid, no thumbnails and no preview tiles.
- **No report-as-tabs.**  A report is still not a tab in the Book page's tab
  strip (`doc/web-ui-pages.md` §5).  The Reports page's own mode query is not a
  tab, and the tab strip is unchanged.
- No new dependencies, framework or build step.
- No new server route, endpoint or state.  `?designer=1` is parsed by the same
  shell the existing `/reports` route already serves.
- No read/write change: the page still reads the same `/api/*` routes and the
  report document routes, and the write path (register editing, saving) is
  untouched.
- No attempt to make the browser's Back button ask about unsaved designer
  edits.  In-page navigation is guarded; a reload/close is the browser's
  decision, exactly as it is for the register's `beforeunload` note.
- No PDF export, no "open the document in a new tab" button, because a
  top-level navigation to the document route is the shell by design (§2.4).

## 7. How this is checked

`web/test/pages-ui.mjs` (run in both token and `--debug-port` modes) checks:
each pane exists and can be collapsed, the widths survive a reload, applying
options does not scroll the page or the document, the status bar shows the
frame's row count and the dates actually used, the breadcrumb returns to the
list, `Ctrl+K`/`Enter`/`[`/`]`, entering the designer with a live preview and
leaving it without losing work, and the 800px layout's usable document.

`web/test/ui-test.mjs` keeps its existing report, options, saved-view and
designer checks; the handful that used to interact with the inline designer
now enter the designer mode first (nothing about what they assert is relaxed).
