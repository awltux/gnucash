# ADR 0001 — Qt 6 presentation tier, unchanged accounting engine

- **Status**: Accepted
- **Date**: 2026-11-06
- **Approved**: 2026-11-06, by the owner of this repository checkout
- **Supersedes**: none
- **Related**: none yet

## Context

GnuCash ships a GTK 3 desktop application whose HTML reports are rendered by
WebKitGTK. Two consequences drive this decision:

1. **Windows is a second-class platform.** `CMakeLists.txt` requires
   `webkitgtk-3.0` (WebKit1, circa 2016) whenever `WIN32` is set. WebKitGTK has
   no Windows port, so building for Windows requires the out-of-tree
   `gnucash-on-windows` jhbuild environment, which rebuilds the dependency
   stack (including a patched Guile 2.2 and a patched libdbi) from source.
2. **GTK 3 is in maintenance mode.** Active toolkit development happens in
   GTK 4.x, and contributor familiarity with GTK 3 continues to decline.

Qt 6 is the only mainstream toolkit that provides a maintained embedded browser
(QtWebEngine, Chromium-based) on Linux, Windows and macOS, and it is the only
option that makes MSVC a supported compiler for this project.

This ADR records the decision and the migration plan. It does **not** authorise
any change to stored data, the engine, or the file format.

### Measurements that bound the work

| Area | Files | Lines |
|---|---|---|
| `libgnucash/engine` | 132 | 92,351 |
| `libgnucash/backend/{xml,sql,dbi}` | 87 | 37,077 |
| Guile report engine (`.scm`) | 164 | 65,493 |
| GTK presentation tier (`gnome`, `gnome-utils`, `register`, `import-export`, `html`, `gnome-search`) | 279 | 212,255 |
| of which the register widget (`gnucash/register/`) | 50 | 36,037 |
| `.glade` UI definitions | 66 | — |

Additional coupling: 184 `G_DEFINE_TYPE`/`G_DECLARE_*` macros, 339 libdbi
references, 391 WebKit references confined to `gnucash/html/`, 382
`cairo_`/`pango_` call sites.

## Decision

**Replace the presentation tier with Qt 6; keep `libgnucash`, the backends and
the Guile report engine unchanged.**

Qt supplies the widgets, the event loop integration, printing (`QPdfWriter`,
`QPrinter`), settings (`QSettings`), and report rendering (`QWebEngineView`
replacing WebKitGTK). GLib/GObject remain inside the engine: they are
maintained, cross-platform, and removing them would break the C API that the
SWIG bindings and user Scheme reports depend on.

Ship `WITH_GTK` and `WITH_QT` builds from one tree for at least one release
cycle, so users and packagers can fall back.

### Why not reimplement the engine

The engine is the compatibility anchor. It owns the XML v2 format, the SQL
triple store and the KVP slots. Any UI that runs on the same engine writes
byte-identical books by construction. A parallel implementation of
`QofBook` serialization is explicitly out of scope.

### Licensing

GnuCash is GPL-2.0-or-later, so the combined work may be distributed under
GPL-3.0, which Qt's LGPL-3.0 permits. Adopting Qt therefore relicenses the
binary distribution to GPL-3.0. That is accepted as a consequence of this
decision rather than deferred: the "or later" clause makes the change
permissible, the source obligation is unchanged, and the retained Guile report
engine stays under the same terms as today.

## Compatibility contract

This is the acceptance criterion for every phase.

1. **Golden-book corpus.** 25–40 real books, one per feature axis: multi-currency,
   lots and capital gains, business documents (invoices, bills, customers,
   vendors, employees), budgets, scheduled transactions, closing entries, plus
   XML, SQLite and PostgreSQL variants, plus books written by released 4.x and
   5.x versions.
2. **Round trip.** Every corpus book must survive load → save → reload with a
   canonicalized comparison (XML: normalise attribute order and whitespace;
   SQL: sorted row-level dumps, not object-level assertions).
3. **Interop.** A book written by the Qt build must open, save and reload
   correctly in the GTK build of the same series, and vice versa. Both
   directions, for every backend.
4. **Version identity.** The Qt build reports the same version string and the
   same `GNUCASH_RESAVE_VERSION`, and sets the same `GNC_FEATURE_*` entries as
   the GTK build of the same series. Two binaries in one series must never
   mutually refuse to write.
5. **Backend freeze.** No change under `libgnucash/backend/` during a UI phase.
   Engine changes follow the stored-data gate in `AGENTS.md` §5.

CI runs the matrix on Linux, Windows and macOS:

| Build | XML | SQLite | PostgreSQL | Role |
|---|---|---|---|---|
| GTK | yes | yes | yes | baseline |
| Qt | yes | yes | yes | candidate |
| cross-read GTK → Qt and Qt → GTK | yes | yes | yes | interop gate |

## Architecture seams requiring up-front design

**Event loop.** Engine notifications are GLib-driven (`QofEvent`). Either run a
dedicated thread pumping `g_main_context_iteration` and marshal to Qt through
queued connections, or subclass `QAbstractEventDispatcher` and call
`g_main_context_prepare/check/dispatch`. Pick one in Phase 0 with a spike; a
wrong choice surfaces later as intermittent deadlocks.

**Guile is single-threaded.** All Scheme evaluation stays on one thread.
Reports must run through a serialised queue with a progress dialog rather than
concurrently.

**The HTML seam is GTK-shaped.** `struct _GncHtmlClass`
(`gnucash/html/gnc-html.h:122-145`) already exposes a vtable (`show_url`,
`show_data`, `reload`, `print`, `parse_url`) but inherits `GtkBinClass` and
takes `GdkEventButton*` in its callbacks. Phase 1 splits document/URL handling
from widget embedding: `gnc-html.cpp` keeps building and dispatching documents,
and a `QWebEngineView` target consumes the same HTML that Scheme produces. The
split also removes the WebKit dependency from the Windows build.

## Phases and gates

| # | Phase | Exit gate | Estimate |
|---|---|---|---|
| 0 | Groundwork: build presets, dependency manifest, CI matrix, event-loop spike, licence decision | Spike opens a book; Windows CI builds the headless CLI | 2–3 months |
| 1 | Headless plateau: Qt shell opens books, runs Guile reports, renders HTML offscreen, exports PDF | All standard reports render from the corpus; golden-book gate green | 3–4 months |
| 2 | Read-only shell: main window, actions, tabs, account tree, preferences | Browsing parity; keyboard navigation for tree and tabs | 3–4 months |
| 3 | Register: `QAbstractItemModel` over engine cursors, painted view, delegates | Parity checklist signed off; performance gates met | 6–9 months |
| 4 | Editing: transaction/split editors, reconcile, scheduled transactions, budgets, assistants | Every corpus write path reachable from Qt; interop gate green | 5–7 months |
| 5 | Import/export and business: QIF/CSV/OFX, invoices, bills, customers, vendors, employees | Import → reconcile → report loop complete | 5–7 months |
| 6 | Report UI: option widgets, custom report management, print/PDF | User Scheme reports load and run unmodified | 3–4 months |
| 7 | Parity hardening: accessibility, HiDPI, RTL, i18n audit, packaging | Beta on all three platforms | 4–6 months |

Total: approximately 30–45 person-months. A hard go/no-go follows Phase 2: if
read-only browsing is not at parity, stop and ship the GTK 4 path instead.

Phases 8+ (optional, after the UI transition): `QSettings` replacing GSettings
with a one-time import, `pybind11` replacing SWIG, and direct SQLite/libpq
drivers replacing libdbi behind the existing backend interface. None of these
change the on-disk format.

### Phase 0 status (2026-11-06)

Landed on the `phase0/qt6-groundwork` branch:

- this ADR;
- `CMakePresets.json`, verified with CMake 4.3.1 on Windows (only the Windows
  preset is offered) and 3.28.3 on Linux (only the Linux presets are offered);
- a Linux configure baseline: `linux-core` configures from a clean build
  directory on Ubuntu 24.04 and builds `gnc-core-utils`;
- `vcpkg.json`, which resolves on Windows to qtbase 6.11.3 (widgets, sql,
  sqlite, testlib, windeployqt) and qtwebengine 6.11.3 (pdf, webengine), and
  requests nothing on Linux, where Qt comes from the distribution.

Blocked until the toolchain allows verification:

- **Building the Qt tier.** The manifest resolves, but installing it means
  building Qt from source on Windows, which has not been attempted here.
- **Windows CI job.** The MSYS2 installation available here has no compiler, so
  such a workflow could not be shown to pass and was not committed.
- **Event-loop spike and the `gnc-html` document/widget split.** Both need Qt 6
  and a tree that builds the GUI.

## Consequences

**Positive.** Windows becomes a normal target built with MSVC and vcpkg rather
than jhbuild; the WebKit 1 dependency disappears; the register gains a real
model/view separation; GTK 3 EOL stops being a scheduling constraint.

**Negative.** 212k lines of UI are rewritten; QtWebEngine adds a large runtime
dependency; GNOME integration (GSettings, dconf, libsecret) is partially
replaced; the register is a 36k-line custom widget with a 30-year-old keyboard
model that is easy to regress.

**Neutral.** The C API, the Guile report API, the gettext catalogs (60+
languages, translated on Weblate) and the file formats are untouched.

## Alternatives considered

- **GTK 4 + WebKitGTK 6.0.** Lower churn, keeps the GNOME ecosystem, but does
  not solve Windows, because WebKitGTK still has no Windows port. Rejected for
  this ADR; it remains the fallback if Windows parity is dropped.
- **Web UI (Tauri/Electron).** Reports are already HTML, so this is tempting,
  but a virtualised grid with inline editing, multi-row selection and the
  existing keyboard model is the weakest part of web UIs and the strongest
  requirement of this application.
- **Keep GTK 3 and maintain the jhbuild environment.** Status quo. Rejected
  because it blocks Windows contributors and inherits an unmaintained browser
  engine.

## Open questions

1. Is the GPL-3.0 relicense acceptable to all significant contributors?
2. Does the register model map cleanly onto `QAbstractItemModel`, or does it
   need a bespoke proxy layer for split rows?
3. Which packaging formats are in scope at Phase 7 (MSIX, Inno Setup, Flatpak,
   notarised DMG)?
