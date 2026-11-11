# Pages, tabs and URLs in the web UI

Stage 1 of splitting the single-page web UI into pages.  This document records
the vocabulary, the URL scheme, the tab model and the one place where the web
tier deliberately diverges from the desktop.  It is a decision document, not a
tutorial: the implementation is `web/ui/index.html` and `web/server/main.cpp`.

Related: `doc/web-ui-menu.md` (the menu inventory and each item's reason),
`doc/web-ui-roadmap.md` (the workstream this sits inside), `WEBUI.md` (how to
run it).

## 1. The vocabulary

The desktop application has **one main window**.  The window's **pages are
tabs**: a page is a notebook tab, and the desktop's Views menu and page plugins
act on the tab that is showing.

| term | meaning | in this stage |
|---|---|---|
| **Accounts page** | the account tree with balances.  It is the window's **default tab**.  There is no "default account": the tab shows the whole book, not one account. | exists: the Book page's pinned **Accounts** tab |
| **register page** | one account's register, one tab per account. | exists: one **register tab** per opened account |
| **report page** | one report, one tab.  On the desktop report pages are tabs in the same main window as the accounts page. | exists as a page, **not** as a tab: see §5 (divergence) |
| **business pages** | customer, vendor, employee, job, invoice and bill pages, one per record. | not in the web tier |
| **scheduled-transaction pages** | the scheduled-transaction editor and its pages. | not in the web tier |

Two conventions follow from that vocabulary and are used everywhere:

- A page is identified by its **URL**, not by a hidden piece of client state.
- The **Accounts page** is the default tab.  It is never called the "default
  account", and opening an account never selects it as a default: it opens the
  account's register tab.

## 2. The URL scheme

Every page path is served the same application shell; the client router reads
the path and puts the page on screen.  `<base>` is `/<token>/` in a normal
session and `/` under `--debug-port`, so the paths below work unchanged in
both modes (the harnesses and `web/test/pages-ui.mjs` check both).

| path | page |
|---|---|
| `<base>` or `<base>/accounts` | Book page, **Accounts** tab |
| `<base>/register/<acct-guid>` | Book page, that account's register tab |
| `<base>/reports` | Reports page, no report open |
| `<base>/report/<name>` | Reports page, that native/Scheme report open |
| `<base>/report/spec/<id>` | Reports page, that saved or built-in report open |

`<base>/` is an alias for the Accounts tab, not a redirect: it stays the
address the server printed (the `--debug-port` page is served at `/`, and a
test that asserts that path keeps passing).  Unknown paths fall back to the
Accounts tab and are canonicalised there.

The existing document routes are **not** renamed or moved:

- `/report/<name>` and `/report/spec/<id>` are still the report **documents**
  the report iframe and every harness read.
- `/register/<spec>` is still the standalone server-rendered register page a
  report's drill-down link navigates to inside the frame.
- `/report-id/<id>` is still the report-to-report document route.

### 2.1 One path, two resources: how the shell and the document coexist

`/report/<name>`, `/report/spec/<id>` and `/register/<spec>` each have to be
two things: the **page** a reader navigates to, and the **document** the frame
or a harness fetches.  The server tells them apart by the request's **fetch
metadata**, which every browser sends and no command-line client does:

- `Sec-Fetch-Dest: document` — a top-level navigation.  The server serves the
  shell, and the client opens the page the path names.
- anything else, or no `Sec-Fetch-Dest` at all — a subresource.  The server
  serves exactly the document it has always served.  The report iframe
  (`Sec-Fetch-Dest: iframe`), `curl`, Python and Node harnesses and the
  designer's preview all take this path.

This is the resolution of the conflict the stage brief anticipated: a page URL
for a report cannot simply take over `/report/<name>`, because that path is
the report document for the iframe and for every harness.  The alternative —
a query parameter — was rejected because a harness would have had to change;
the alternative of a second path (`/pages/report/<name>`) was rejected because
the brief fixes the URL scheme.  Fetch metadata is the only signal that
distinguishes "a person navigated here" from "a document was requested", and a
client that does not send it (an old browser, a script) keeps the old,
document-serving behaviour.  `Cache-Control: no-store` is already on every
response, so the two forms cannot be confused by a cache either.

One consequence is recorded rather than hidden: the same URL returns different
representations, so the routing contract depends on fetch metadata.  This is
tested directly in `web/test/pages-ui.mjs` ("the report frame gets the report
document, not the shell", "/report/<name> still answers the report document to
a direct read").

## 3. The tab model

The Book page has a tab bar (`role="tablist"`) above its content.

- **Accounts** is **pinned**: it has no close control, cannot be closed by
  Delete/Backspace, and is the fallback when the active register tab closes.
- A **register tab** is one account.  Clicking the same account again switches
  to its existing tab instead of opening a second one.  Each register tab has
  a close control (`aria-label="Close the <account> tab"`); closing the active
  tab falls back to its neighbour, ultimately the Accounts tab.
- Each tab is a real ARIA tab: `role="tab"`, `aria-selected`, `aria-controls`
  pointing at a `role="tabpanel"` whose `aria-labelledby` is the tab.  The close
  control is a sibling of the tab button, not a button nested inside it: a tab
  is one interactive element, and a nested control would be a second,
  unreachable one.
- Only the active tab's panel holds the register: the single `#register`
  section moves into the active panel, so there is one grid and one register
  state whichever tab is showing.  Switching tabs re-reads the register for
  the account it shows; nothing from another account's tab can leak into it.
- Keyboard: **Ctrl+Tab** / **Ctrl+Shift+Tab** cycle the tabs from anywhere on
  the Book page; **Left/Right/Home/End** move between tabs when the tab list
  has focus; **Delete/Backspace** closes the focused register tab.  The
  Accounts tab is a tab stop and the other tabs are reached with the arrow
  keys (one roving tabindex).
- The tab strip does not change the Reports page: a report is not a tab in
  this stage (§5).

### 3.1 What the active tab does to the URL

Activating a tab **writes its page URL**: Accounts writes `<base>/accounts`, a
register tab writes `<base>/register/<guid>`.  The URL is written before the
register's data is read, so a reload during loading still lands on the tab.

Closing the active tab writes the neighbour's URL (or `<base>/accounts`).
Opening an account from the tree, by `View > New Accounts Page`, or from a
report's drill-down link is a history entry; so is every tab activation.

## 4. Where a reload lands, and what the URL does not hold

The rule: **the URL is the page state**.  A page, a tab and an open report are
all named by the path; the client router (`applyRoute`) reads it on load and on
`popstate`, so direct navigation, reload, Back and Forward all restore the same
page, tab and report.  Nothing needed to restore *where the reader was* lives
only in memory.

One thing cannot fit in a path: the **set of open register tabs** (a URL names
one tab, not the strip).  It is kept in `sessionStorage` under
`gnucash-webui.open-tabs.v1`, scoped to the browser tab:

- reload restores the strip and the active tab (the URL decides which is
  active);
- a link directly into a register opens that register's tab even when the
  strip was never restored;
- a new browser tab or another window starts with just the Accounts tab, which
  is the honest default; the URL still opens a register if it names one.

The strip is a view preference, not book data.  It is not sent to the server,
and the server has no per-reader session state.

## 5. Divergence: Reports is a separate page

On the desktop a **report page is a tab of the same main window** that holds
the accounts and register pages.  The web tier puts the report list, the
report's options, the report frame and the designer on a **separate Reports
page** (`<base>/reports`, `<base>/report/<name>`, `<base>/report/spec/<id>`).

This is a **web divergence, not desktop fidelity**, and it is recorded here so
that no later document claims otherwise.  It exists because the report list,
the designer and the frame are the web tier's own surfaces with no desktop
menu counterpart, and because the options form and the designer are page-sized
content that the desktop shows in dialogs and separate tabs.  The Reports page
keeps its internal layout: moving it to a URL did not redesign it.

Consequence for the menu: the desktop's generated per-page entries under
**Windows** are still absent (the tabs are not browser windows), and the View
menu's page items (`New Accounts Page`, `Reports`) are the web tier's way to
move between the two pages.  `New Accounts Page` keeps the desktop's label but
shows the pinned Accounts tab instead of creating a second accounts page — a
changed meaning, recorded in `doc/web-ui-menu.md`.

## 6. The server side

`web/server/main.cpp` serves the shell (`web/ui/index.html`) for `<base>/`,
`<base>/accounts` and `<base>/reports` unconditionally, and for
`register/<spec>`, `report/<name>` and `report/spec/<id>` when the request is a
top-level navigation (§2.1).  `--debug-port` uses the same routes without a
token segment.  No other route changed.

There is no server-side page state and no new API.  The page reads the same
`/api/*` routes it always read.

## 7. What stage 1 does not do

- No business pages and no scheduled-transaction pages.  The menu items that
  belong to them stay disabled and keep naming the page they need
  (`PAGE_REASONS`).
- No report tab in the Book page's tab strip: a report is the Reports page's
  content (§5).
- The register tab strip is not persisted across browser sessions (only across
  a reload of the same browser tab).
- Nothing in the report list, the option form or the designer was redesigned;
  they moved to a URL and kept their layout.
