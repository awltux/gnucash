# GnuCash Qt 6 migration plan

This is the **living** document for the migration: phases, exit gates, current
status and risks. It is expected to change every time work lands.

Decisions do not live here. They live in `doc/adr/`, one decision per record, and
they are not edited once accepted — a changed decision gets a new ADR that
supersedes the old one. If you are looking for *why* rather than *what next*,
read the ADRs.

| Record | Decision |
|---|---|
| [0001](adr/0001-qt6-presentation-tier.md) | Qt 6 replaces the presentation tier; the engine is kept |
| [0002](adr/0002-event-loop-integration.md) | Bridge GLib and Qt with a pumped `GMainContext` thread |
| [0003](adr/0003-windows-dependencies-vcpkg.md) | Windows dependencies come from a vcpkg manifest |
| [0004](adr/0004-html-layer-separation.md) | Separate URL handling from the GTK widget before adding a renderer |
| [0005](adr/0005-windows-ci-scope.md) | Windows CI verifies the manifest, not the build |
| [0006](adr/0006-prebuilt-qt-for-development.md) | Prebuilt Qt for development; the manifest stays the contract |
| [0007](adr/0007-windows-toolchain-mingw.md) | Windows toolchain is MinGW-w64 through MSYS2 |
| [0008](adr/0008-windows-ci-exercises-setup.md) | Windows CI exercises the documented setup and the spike |
| [0009](adr/0009-backend-registration-in-qt.md) | A Qt process registers a backend by loading its module |

## Phases and exit gates

| # | Phase | Exit gate | Estimate |
|---|---|---|---|
| 0 | Groundwork: presets, dependency manifest, CI, event-loop spike, licence | Spike opens a book; Windows dependency path proven | 2–3 months |
| 1 | Headless plateau: Qt shell opens books, runs Guile reports, renders HTML offscreen, exports PDF | All standard reports render from the golden corpus; compatibility gate green | 3–4 months |
| 2 | Read-only shell: main window, actions, tabs, account tree, preferences | Browsing parity; keyboard navigation for tree and tabs | 3–4 months |
| 3 | Register: `QAbstractItemModel` over engine cursors, painted view, delegates | Parity checklist signed off; performance gates met | 6–9 months |
| 4 | Editing: transaction and split editors, reconcile, scheduled transactions, budgets, assistants | Every corpus write path reachable from Qt; interop gate green | 5–7 months |
| 5 | Import/export and business: QIF, CSV, OFX, invoices, bills, customers, vendors, employees | Import → reconcile → report loop complete | 5–7 months |
| 6 | Report UI: option widgets, custom report management, print and PDF | User Scheme reports load and run unmodified | 3–4 months |
| 7 | Parity hardening: accessibility, HiDPI, RTL, i18n audit, packaging | Beta on all three platforms | 4–6 months |

Total: approximately 30–45 person-months. A hard go/no-go follows Phase 2: if
read-only browsing is not at parity, stop and ship the GTK 4 path instead.

Post-transition, and only after the UI lands: `QSettings` replacing GSettings
with a one-time import, `pybind11` replacing SWIG, and direct SQLite/libpq
drivers replacing libdbi behind the existing backend interface. None of those
change the on-disk format.

## Compatibility gate

No phase exits without these, on every backend (XML, SQLite, PostgreSQL):

- every book in the golden corpus survives load → save → reload, compared
  canonically (XML normalised for attribute order and whitespace, SQL as sorted
  row dumps);
- a book written by the Qt build opens and saves in the GTK build of the same
  series and vice versa;
- the Qt build reports the same version string, the same
  `GNUCASH_RESAVE_VERSION`, and sets the same `GNC_FEATURE_*` entries.

## Status

Updated as work lands. Last update: 2026-11-06.

| Item | State | Evidence |
|---|---|---|
| ADR set | Done | 0001–0005 accepted |
| `CMakePresets.json` | Done | `cmake --list-presets` on CMake 4.3.1 (Windows) and 3.28.3 (Linux); `linux-core` configures and builds `gnc-core-utils` |
| vcpkg manifest | Done, Windows only | Resolves to qtbase 6.11.3 and qtwebengine 6.11.3 on Windows; requests nothing on Linux |
| Event-loop bridge | Verified on Linux and Windows | 44/44/46 on Qt 6.4.2 + GLib 2.80 (Linux); 47/47/49 on Qt 6.9.1 + GLib 2.84.2 (Windows) |
| `gnc-html` URL layer | Done | Builds and links; test passes and links no GTK |
| Windows CI | Done, narrowed | Manifest resolution job; MSYS2 cannot build GnuCash (no mingw Guile, no libdbi) |
| Qt on Windows | Done, prebuilt | MSYS2 Qt 6.9.1 + GLib 2.84.2, MinGW-w64; see [0007](adr/0007-windows-toolchain-mingw.md) and [dev setup](dev-setup-windows.md) |
| vcpkg source build of Qt | Abandoned | 4+ hours then `libiconv` configure failed (exit 77) and the retry wedged; kept only as an experiment, see [0006](adr/0006-prebuilt-qt-for-development.md) |
| Qt loads a book through the engine | Done | `spike-engine-qt` opens `sample1.gnucash`, 104 accounts, exit 0; see [0009](adr/0009-backend-registration-in-qt.md) |
| Report pane renders HTML | Done | Report engine produces the document, `QWebEngineView` renders it headlessly (241 bytes in, 246 out, ok=1) |
| Shell hosts engine and report pane | Done, hosting only | 104 accounts, 5 top-level, pane ok, exit 0; no GLib events flow yet |
| Phase 1 headless shell | In progress | Engine and pane coexist; report is not yet rendered for the loaded book |

## Risks

| Risk | Mitigation |
|---|---|
| Format drift through a "small" engine change | Backend freeze during UI phases; golden corpus; interop matrix |
| Guile plus Qt threading bugs | Single Scheme thread, serialised queue, TSAN builds |
| Register UX regression | Parity checklist, beta flag, recorded keyboard workflows |
| Windows dependency builds stay expensive | Keep the manifest minimal; do not add qtwebengine to the default Windows build until reports need it |
| Contributor loss (GTK familiarity) | Keep the GTK build working; port in small slices |
| Effort overrun | Go/no-go after Phase 2 |
