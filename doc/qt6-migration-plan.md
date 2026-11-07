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
| [0010](adr/0010-qt-tier-enters-the-build.md) | The Qt tier enters the build as an opt-in target |
| [0011](adr/0011-register-decoupling.md) | The register needs decoupling before Phase 3, not a view port |
| [0012](adr/0012-gui-thread-owns-the-engine.md) | The GUI thread owns the engine |
| [0013](adr/0013-coexistence-with-gtk.md) | Coexistence rules while both tiers ship |
| [0014](adr/0014-account-tree-model.md) | The account tree is a model over engine objects, not a snapshot |
| [0015](adr/0015-report-path-out-of-process.md) | Reports leave the process until the report layer is GTK-free |

## Phases and exit gates

| # | Phase | Exit gate | Estimate |
|---|---|---|---|
| 0 | Groundwork: presets, dependency manifest, CI, event-loop spike, licence | Spike opens a book; Windows dependency path proven | 2–3 months |
| 1 | Headless plateau: Qt shell opens books, runs Guile reports, renders HTML offscreen, exports PDF | All standard reports render from the golden corpus; compatibility gate green | 3–4 months |
| 2 | Read-only shell: main window, actions, tabs, account tree, preferences | Browsing parity; keyboard navigation for tree and tabs | 3–4 months |
| 3a | Register cell layer: remove GTK from `register-core` **and then `ledger-core`** (ADR 0011, audited in `register-core-audit.md`) while the GTK register keeps working | `register-core` is toolkit-free (done); `ledger-core` reaches a GTK-free API; `gnucash` still builds and passes the register tests | 2–3 months (was 1–2 weeks) |
| 3b | Register view: painted Qt view over the decoupled cell layer | Parity checklist signed off; performance gates met | 5–7 months |
| 4 | Editing: transaction and split editors, reconcile, scheduled transactions, budgets, assistants | Every corpus write path reachable from Qt; interop gate green | 5–7 months |
| 5 | Import/export and business: QIF, CSV, OFX, invoices, bills, customers, vendors, employees | Import → reconcile → report loop complete | 5–7 months |
| 6 | Report UI: **de-GTKify `gnc-report`/`gnc-html` first** (ADR 0015), then option widgets, custom reports, print and PDF | User Scheme reports load and run unmodified, in-process | 5–6 months |
| 7 | Parity hardening: accessibility, HiDPI, RTL, i18n audit, packaging | Beta on all three platforms | 4–6 months |

Total: approximately 35–55 person-months. A hard go/no-go follows Phase 2: if
read-only browsing is not at parity, stop and ship the GTK 4 path instead.

Phase 3 was estimated at 6–9 months while it was "port the register". ADR 0011
found that `register-core` includes `<gtk/gtk.h>` and links `gnc-gnome-utils`,
so it is split into a decoupling (3a) and a view port (3b) with a larger total.

3a was then estimated at 1–2 weeks from a grep that found three GTK call sites in
`register-core`. That number was tested on 2026-11-07 by removing the includes
and the link line: the three sites were real and are now gone, but the removal
broke 15 objects in four directories, and it showed that `ledger-core` — the
layer a Qt view would consume, 19,450 lines with five `GtkWidget` public headers
and eight message dialogs — is the GTK-bound part. The `register-core` slice is
done; 3a's remainder is `ledger-core`, at 4–8 weeks, so the phase is 2–3 months.
`doc/register-core-audit.md` has the measurements. The lesson is that a grep
finds symbol uses, not transitive includes, link paths or API shape.

Post-transition, and only after the UI lands: `QSettings` replacing GSettings
with a one-time import, `pybind11` replacing SWIG, and direct SQLite/libpq
drivers replacing libdbi behind the existing backend interface. None of those
change the on-disk format.

## Phase 2 gate checklist

The gate is "browsing parity; keyboard navigation for tree and tabs", which is
judgement until it is written down. This is the list; the go/no-go decides each
line as **done** or **deliberately out of scope, with a reason**. Nothing is
"probably fine".

| Behaviour | State | Evidence / what would close it |
|---|---|---|
| Open a book from the file system and stay open | done | `--self-test` and app mode; `File → Open…` |
| Account tree: names, hierarchy | done | `model_ok=1`, tree renders from the engine |
| Account tree: balance and period totals | done | `total_ok=1 period_ok=1`, formatted by the GTK tree's own helpers |
| Account tree: hidden accounts, sorting | not done | GTK keeps "show hidden" in its view-state file; ADR 0013 forbids sharing that state, so this needs a gschema key or an explicit scope decision |
| Reports: list, render, select, reload | done | `reports=63`, `pane_ok=1`, `Reports` menu |
| Reports: more than one open at a time | done | one tab per report, each with its own view (`tabs_ok=1`) |
| Tabs | done | `Accounts` page plus a page per report; `Ctrl+Tab` / `Ctrl+Shift+Tab` |
| Preferences read through `gnc-gsettings` | done | `prefs_ok=1`, `general/negative-in-red` |
| Preferences UI | not done | deferred by decision until the rest is settled; must reuse existing keys |
| Keyboard: tree navigation | done | arrows, Home/End, Page Up/Down and type-ahead are `QTreeView` defaults; `+`/`-` added to match GtkTreeView (`keys_ok=1`, see [the audit](qt6-keyboard-audit.md)) |
| Keyboard: tab switching | done | `View → Next/Previous tab`, `Ctrl+Tab` / `Ctrl+Shift+Tab` and `Ctrl+Page Down/Up` |
| Keyboard: menu shortcuts | done (browsing) | Open `Ctrl+O`, Quit `Ctrl+Q`, Reload `F5`/`Ctrl+R`, Close page `Ctrl+W`; editing shortcuts are Phase 4 |
| No shared mutable UI state (ADR 0013) | done | no Qt-side store; state file and KVP untouched |

When every line reads done or has a named reason, the gate is a decision about
the product, not about the code.

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

Updated as work lands. Last update: 2026-11-07.

| Item | State | Evidence |
|---|---|---|
| ADR set | Done | 0001–0016 accepted (0003 and 0005 superseded) |
| `CMakePresets.json` | Done | `cmake --list-presets` on CMake 4.3.1 (Windows) and 3.28.3 (Linux); `linux-core` configures and builds `gnc-core-utils` |
| vcpkg manifest | Done, Windows only | Resolves to qtbase 6.11.3 and qtwebengine 6.11.3 on Windows; requests nothing on Linux |
| Event-loop bridge | Verified on Linux and Windows | 44/44/46 on Qt 6.4.2 + GLib 2.80 (Linux); 47/47/49 on Qt 6.9.1 + GLib 2.84.2 (Windows) |
| `gnc-html` URL layer | Done | Builds and links; test passes and links no GTK |
| Windows CI | Done, narrowed | Manifest resolution job; MSYS2 cannot build GnuCash (no mingw Guile, no libdbi) |
| Qt on Windows | Done, prebuilt | MSYS2 Qt 6.9.1 + GLib 2.84.2, MinGW-w64; see [0007](adr/0007-windows-toolchain-mingw.md) and [dev setup](dev-setup-windows.md) |
| vcpkg source build of Qt | Abandoned | 4+ hours then `libiconv` configure failed (exit 77) and the retry wedged; kept only as an experiment, see [0006](adr/0006-prebuilt-qt-for-development.md) |
| Qt loads a book through the engine | Done | `spike-engine-qt` opens `sample1.gnucash`, 104 accounts, exit 0; see [0009](adr/0009-backend-registration-in-qt.md) |
| Report pane renders HTML | Done | Report engine produces the document, `QWebEngineView` renders it headlessly (241 bytes in, 246 out, ok=1) |
| Qt tier in the build | Done | `-DWITH_QT=ON` builds `bin/gnucash-qt`; `-DWITH_QT=OFF` configures cleanly with zero `gnucash-qt` targets |
| Qt CI smoke test | Done | `.github/workflows/qt-smoke.yml` ran green on its first execution (run 42395385776, 3m21s, all six steps); it configures with `WITH_QT=ON` on ubuntu-24.04 and calls the same `qt/run-smoke-test.sh` a developer runs |
| Engine events reach Qt | Done | A 20-rename burst from the GLib thread arrived in order, none lost, all on the Qt thread (`events=20/20 in_order=1`) |
| Report for a loaded book | Done, via helper process | `gnucash-cli -R run` renders Trial Balance for `sample1.gnucash` (29,538 bytes) and the Qt pane displays it; in-process rendering still blocked by `gnc-report`→GTK |
| Account tree totals | Done | `Total` and `Total (Period)` columns, formatted by the GTK tree's own app-utils helpers, refreshed from engine events including ancestors (`total_ok=1 period_ok=1`) |
| Preferences read through `gnc-gsettings` | Done | `general/negative-in-red` drives the tree's negative colour; the self-test flips it both ways and restores it (`prefs_ok=1`) |
| Phase 2 read-only shell | Started | Target opens a book and stays open, with menus, tabs (an `Accounts` page plus a page per report), an account tree (names, totals and period totals) and reports driven by the helper's own list. Preferences are read through `gnc-gsettings`; still missing: a preferences UI and a keyboard audit — see the gate checklist above |

## Risks

| Risk | Mitigation |
|---|---|
| Format drift through a "small" engine change | Backend freeze during UI phases; golden corpus; interop matrix |
| Guile plus Qt threading bugs | Single Scheme thread, serialised queue, TSAN builds |
| Register UX regression | Parity checklist, beta flag, recorded keyboard workflows |
| Windows dependency builds stay expensive | Keep the manifest minimal; do not add qtwebengine to the default Windows build until reports need it |
| Contributor loss (GTK familiarity) | Keep the GTK build working; port in small slices |
| Effort overrun | Go/no-go after Phase 2 |
