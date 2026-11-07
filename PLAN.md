# PLAN.md — working notes for the Qt 6 migration

**This file is for the duration of this implementation only and is expected to be
deleted when the migration work finishes.** Everything durable must live in the
repository's own records before then; see §12.

It exists because a session that resumes cold has to re-derive facts that are
expensive to rediscover: which build tree is configured how, what was actually
observed versus assumed, and which mistakes already cost hours.

The durable records are authoritative when they disagree:

- `doc/qt6-migration-plan.md` — phases, exit gates, status table
- `doc/adr/0001…0010` — one decision per record; `doc/adr/README.md` is the index
- `AGENTS.md` — how work is done in this project

## 0. Maintenance rule — update this file with the change

This file is part of the definition of done for this implementation, not
commentary written afterwards. **Every commit that changes behaviour, build
configuration, tooling or status updates the relevant section in the same
commit**, so the file is never more than one commit stale. A PLAN.md that lags
is worse than none: it reads authoritative while being wrong.

| If you change… | Update |
|---|---|
| a build target, option or file layout | §3 |
| phase progress, or the status of a claim | §4, §5 |
| how something is verified, or you observe something new | §5, §6 |
| an environment dependency or version | §8 |
| a trap you had to work around | §7 |
| what comes next | §9 |
| an assumption that turned out wrong | §10, and the ADR it sits on |

If none of these apply, the change is probably too small to deserve its own
commit — but say in the commit body which sections you checked, so the omission
is deliberate rather than forgotten.

---

## 1. The goal, in one paragraph

Replace GnuCash's GTK 3 presentation tier with Qt 6 **without touching the
accounting engine, the storage backends or the file formats**. The engine owns
the XML v2 file, the SQL store and the KVP slots; a front end that runs on the
same engine writes byte-identical books by construction. Success is a Qt tier at
parity, shipped alongside GTK for at least one release cycle.

Why this is being done at all: `CMakeLists.txt` requires `webkitgtk-3.0`
(WebKit1, ~2016) on Windows, and WebKitGTK has no Windows port, so Windows needs
the out-of-tree `gnucash-on-windows` jhbuild environment. Qt is the only
mainstream toolkit with a maintained embedded browser on all three platforms.
GTK 3 is also in maintenance mode.

## 2. Invariants — violate these and the work is worthless

- **The engine owns the format.** No UI writes a book directly; no UI change
  touches `libgnucash/engine` or `libgnucash/backend/*` semantics.
- **Compatibility gate.** Corpus books round-trip; a book written by the Qt build
  opens and saves in the GTK build of the same series and vice versa; version
  string, `GNUCASH_RESAVE_VERSION` and `GNC_FEATURE_*` are identical.
- **Verified or blocked.** Nothing is committed as working without a run that
  shows it. Anything unverifiable is recorded with the concrete reason.
- **Small commits, one idea each.** The branch is reviewed as a series.

## 3. Where things are

| Thing | Location |
|---|---|
| Branch | `phase0/qt6-groundwork`, 56 commits ahead of `stable`, **never pushed** |
| Review order | ADRs → `doc/qt6-migration-plan.md` → build config → `qt/` → `doc/adr/spike-*` |
| Linux build tree | `~/build/ext4-qt` (WSL, ext4 copy of the tree). `WITH_QT=ON`, Debug; **both** `bin/gnucash` (GTK) and `bin/gnucash-qt` built as of 2026-11-07 |
| Source copy for building | `~/src/gnucash` — an rsync of this repo; edit here, then sync |
| Core-only tree | `~/build/linux-core` (from the `linux-core` preset) |
| In-repo build dir | `.build/linux-core` (created by the preset; gitignored) |
| Qt shell binary | `~/build/ext4-qt/bin/gnucash-qt` — needs `-DWITH_QT=ON` |
| Book fixture | `libgnucash/backend/xml/test/test-files/load-save/sample1.gnucash` (104 accounts) |
| Register audit | `doc/register-core-audit.md` — now records what actually broke, not what grep predicted |
| Report HTML generator | `doc/adr/spike-report-pane/make-html.scm` |
| Windows scratch | `E:\gnc-qt-spike`, `E:\gnc-report-spike`, `E:\gnc-shell-spike` (not in repo) |
| MSYS2 | `C:\msys64`, use the **MINGW64** shell |

## 4. Phases

Estimates from `doc/qt6-migration-plan.md`. Status as last observed.

### Phase 0 — Groundwork ✅ done

*Why:* the toolchain and dependency story were both wrong in ways that only
appeared when executed, so they were settled first.
*How:* build presets, a dependency decision, CI, an event-loop spike, a licence
decision.

- `CMakePresets.json`, verified with CMake 4.3.1 (Windows) and 3.28.3 (Linux).
- **ADR 0007**: Windows toolchain is MinGW-w64/MSYS2 with *prebuilt* Qt. Not
  vcpkg: it compiles Qt from source (1585 debug steps) and never finished here.
- **ADR 0008**: CI installs the documented MSYS2 packages and runs the spike.
- **ADR 0002** + spike: GLib↔Qt bridge, verified on both platforms
  (Linux 44/44/46; Windows 47/47/49).

### Phase 1 — Headless plateau 🟡 mostly done

*Why:* three risky integrations had to be proven before any UI volume: backend
loading, report rendering, and engine events crossing threads.
*How:* a Qt program that opens a book, renders report HTML, observes events.

- **ADR 0009**: backend registration is `dlopen` + `qof_backend_module_init`.
  A real book loads: 104 accounts, 5 top-level.
- **Report pane**: report-engine HTML renders in `QWebEngineView` headlessly.
- **Event bridge**: rename made on the GLib thread observed in a Qt slot
  (`event_on_qt_thread=1`).

**Open:** a report rendered *for the loaded book*. Everything so far renders a
standalone document, which does not exercise the Guile session binding.

### Phase 2 — Read-only shell 🔴 just started

*Why:* the first thing a user would recognise as an application.
*How:* `gnucash-qt` gated behind `WITH_QT` (ADR 0010); account tree + report
pane; then menus, actions, tabs, preferences.

- Target exists with a tree and a pane. No menus, no actions, no editing, no save.

### Phase 3 — Register (the hard one) 🟡 3a started

*Why:* 36k lines across 50 files, hand-drawn, with a 30-year-old keyboard model.
The single largest risk in the programme.
*How:* `QAbstractItemModel` over engine cursors, painted view + delegates,
parity checklist, performance gates (20k splits < 1s open, 60 fps scroll,
<50 ms typing latency).
**Decide first:** does the split-row model map cleanly, or does it need a proxy
layer? Answer before writing widget code.

**3a, first slice — done (2026-11-07).** `register-core` is toolkit-free: no
GTK/GDK/Pango symbols, no GTK library in its `NEEDED` list, and a test that
links the library alone. The audit's "three call sites" were real but did not
bound the work; see `doc/register-core-audit.md`. **The remaining 3a work is
`ledger-core`**, which is the layer a Qt view would actually consume and which
still exposes `GtkWidget` in five public headers and builds eight message
dialogs.

### Phase 4 — Editing ⬜

*Why:* a tier that cannot write is a viewer.
*How:* transaction/split editors, reconcile, scheduled transactions, budgets,
assistants. Gate: every corpus write path reachable; interop gate green.

### Phase 5 — Import/export and business ⬜

*Why:* QIF/CSV/OFX and invoices are most of what small-business users do.
*How:* port import matchers and business documents; aqbanking.

### Phase 6 — Report UI ⬜

*Why:* reports are the product for many users; options are the customisation.
*How:* option widgets, custom report management, print/PDF. Gate: user Scheme
reports load and run unmodified.

### Phase 7 — Parity hardening ⬜

*Why:* accessibility, HiDPI, RTL and packaging are what make it shippable.
*How:* `QAccessible`, keyboard-only flows, i18n audit (keep gettext, **not**
`.ts`), MSIX/Inno/Flatpak/notarised DMG.

### Phase 8 — Optional, after the transition

`QSettings` replacing GSettings with a one-time import; `pybind11` replacing
SWIG; direct SQLite/libpq replacing libdbi behind the same interface. None
changes the on-disk format.

## 5. Verified versus assumed

| Claim | State |
|---|---|
| Event bridge, both platforms | ✅ observed |
| Backend registration, book load (104 accounts) | ✅ observed |
| Report pane renders report-engine HTML | ✅ observed |
| `WITH_QT=ON` builds and the self-test passes | ✅ observed |
| `WITH_QT=OFF` configures with zero Qt targets | ✅ observed |
| Report **for a loaded book** | ✅ observed via helper process (see §9) |
| Event **load** (20 renames, one burst, in order) | ✅ observed |
| Engine thread ownership | ✅ ADR 0012 — GUI thread owns it; self-test identical after the worker thread was removed |
| Coexistence rules (GSettings, helper reports, no shared UI state) | ✅ ADR 0013 — prospective; the preferences rule has not been exercised by a real settings UI |
| Account tree model | ✅ implemented and verified under edits — insert/remove signals fire (`model_ok=1 inserts=1 removes=1`) |
| Report path | ✅ ADR 0015 — out-of-process through Phase 5; the seam is `qt/report-runner.*` |
| App mode | ✅ `gnucash-qt <book>` opens and stays open; `--self-test` is the CI harness |
| Report selector | ✅ 63 reports listed from `gnucash-cli -R list` at startup; `Reports` menu plus `View → Reload report` |
| `register-core` is toolkit-free | ✅ observed 2026-11-07: zero `gtk_`/`gdk_`/`pango_` symbols, `NEEDED` lists no toolkit, GTK build and Qt smoke test green |
| Audit claim: "three call sites, Phase 3a 1–2 weeks" | ❌ falsified: the three sites were real, but removing them broke 15 objects in 4 directories; `ledger-core` is the GTK-bound layer |
| Phase 3 estimate | ✅ revised again after measurement: 3a is 2–3 months (not 1–2 weeks), of which the `register-core` slice is done and `ledger-core` is the 4–8 week remainder; 3b 5–7 months **only if** 3a covers `ledger-core` |
| Windows build of engine/shell | ❌ impossible today (no mingw Guile, no libdbi) |
| CI coverage of the Qt target | ⚠️ job written (`qt-smoke.yml`), never run on GitHub |

## 6. How to re-verify, from scratch

**Build from an ext4 copy, never from `/mnt/f`** (see §7). Configure takes 114 s
there:

```sh
rsync -a --delete --exclude=.build /mnt/f/git-clones/gnu-cash/gnucash/ ~/src/gnucash/
cmake -S ~/src/gnucash -B ~/build/ext4-qt -G Ninja -DWITH_QT=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build ~/build/ext4-qt --target gnucash gnucash-qt gnucash-cli -j 6
```

`gnucash` is the GTK baseline and must stay green — that is the compatibility
requirement, not a nicety. Then the two suites this slice touched:

```sh
cmake --build ~/build/ext4-qt --target test-split-register-copy-ops \
        test-register-core-decoupling -j 6
ctest --test-dir ~/build/ext4-qt -R register --output-on-failure
```

Expected: `test-split-register-copy-ops` and `test-register-core-decoupling`
pass. `test-register` fails in this tree for an unrelated reason (§7).

Then the Qt smoke test — the same script CI runs, so a local green means
something:

```sh
bash ~/src/gnucash/qt/run-smoke-test.sh ~/build/ext4-qt ~/src/gnucash
```

Expected: `accounts=104 top_level=5 pane_ok=1 events=20/20 in_order=1
on_qt_thread=1 model_ok=1 inserts=1 removes=1 reports=63` and
`qt smoke test passed`, exit 0. Anything else means the tree, the environment,
or the code moved.

The older hand-rolled recipe (a standalone `make-html.scm` document, then
`gnucash-qt <backend> <book> <html>`) still works but tests less: it renders a
document that is not the loaded book. Prefer the script.

## 7. Traps that already cost time

- **Configure takes 15–20 minutes** (891 s and 1161 s observed). Every edit to a
  `CMakeLists.txt` forces regeneration on the next build. Batch CMake edits.
- **`po/POTFILES.in` is tracked** and regenerated by configure. New `.cpp`/`.scm`
  files (spikes, `qt/main.cpp`) must go in `po/POTFILES.ignore`, and
  `POTFILES.in` reverted, or commits pick up noise.
- **Driving the report engine outside GnuCash** needs all four of
  `GUILE_LOAD_PATH`, `GUILE_LOAD_COMPILED_PATH`, `GNC_MODULE_PATH`,
  `LD_LIBRARY_PATH`. Omit the last and the error reads
  `Unbound variable: gnc:make-html-document` — which looks like a missing
  export, not a missing library.
- **A `#!/usr/bin/env guile` shebang** becomes an unterminated block comment
  under auto-compilation. Omit it.
- **WebEngine headless** needs `QT_QPA_PLATFORM=offscreen`,
  `QTWEBENGINE_DISABLE_SANDBOX=1`, `QTWEBENGINE_CHROMIUM_FLAGS="--no-sandbox
  --disable-gpu"`.
- **Never build Qt from source on Windows.** `VCPKG_BUILD_TYPE` is a triplet
  setting, not an environment variable; 4+ hours ended in a `libiconv`
  configure failure.
- **PowerShell → WSL/MSYS2 inline bash mangles quotes and pipes.** Write a
  script file; every inline attempt failed.
- **`gnc_account_get_name` does not exist** — the C API still uses
  `xaccAccountGetName`, and the compiler suggests `gnc_account_get_type`.
- **`gnucash-cli -R run` needs three scheme targets and compiled schemas.**
  Build `scm-reports scm-reports-standard scm-report-stylesheets`, and set
  `GSETTINGS_SCHEMA_DIR` to the build's schema directory after running
  `glib-compile-schemas` on it once. Without the stylesheet module every report
  fails with `symbol->string: Wrong type argument ... #f` from
  `report-core.scm:459`, which reads like a broken report system.
- **Never configure through `/mnt/f`.** A `cmake` configure on the 9p mount hung
  in uninterruptible I/O wait (0 CPU ticks in 30 s) for 40+ minutes, reproducibly,
  even after `wsl --shutdown`. Build from an ext4 copy instead:
  `rsync -a --exclude=.build /mnt/f/.../gnucash/ ~/src/gnucash/`, configure
  `~/build/ext4-qt`, and copy changed files back before committing. Configure
  takes **114 s** there. The old tree `~/build/linux-debug` is now unusable.
- **Editing a file on the Windows side does not reliably invalidate a WSL build.**
  A `cmake --build` reported success without recompiling `qt/main.cpp`, and the
  binary kept an older timestamp. `rm` the object (or `touch` the source) and
  rebuild; do not trust "build exit=0" alone when the edit came from Windows.
- **Logs in `/tmp` do not survive.** Ubuntu's tmpfiles rule empties `/tmp` at
  boot and a WSL distro restarts often, so a build log written to `/tmp` vanished
  mid-session and the 86-error falsification log had to be reproduced. Write
  logs under `$HOME` (`~/3a-logs/`).
- **`wsl.exe` fails with `Wsl/E_ACCESSDENIED` under the file sandbox.** The
  sandbox blocks named pipes, so WSL (and Docker) cannot reach the distro at
  all; it reads like a broken WSL install. Run WSL work with the sandbox off.
- **Every Scheme test fails in the ext4 tree** with `Unable to find file
  "test-register" in load path`: no `test-*.scm` is copied into the build tree
  at all (a `find` returns zero). Pre-existing and independent of code changes;
  only the C tests run there.
- **The Qt smoke test can fail transiently.** `pane_ok=0` plus "No suitable
  graphics backend found" appeared once and then passed on an unchanged binary.
  Re-run before blaming a change.
- `.gitattributes` has no `*.md` rule, so Markdown writes warn about CRLF.
  Harmless; blobs are LF.

## 8. Environment state (this machine)

- **WSL Ubuntu 24.04**: cmake 3.28.3, ninja, gcc 13, guile-3.0-dev, boost,
  swig, googletest/libgmock, libdbi + sqlite3 driver, webkit2gtk-4.1-dev,
  aqbanking, ofx, libsecret, gwengui-gtk3, python3-yaml, **qt6-base-dev**,
  **qt6-webengine-dev**.
- **MSYS2 MINGW64**: gcc, pkgconf, qt6-base 6.9.1, glib2 2.84.2. **No**
  mingw-w64 Guile, **no** libdbi — that is why a Windows GnuCash build still
  needs `gnucash-on-windows`.
- **Leftovers safe to delete** (~10 GB on E:): `E:\vcpkg`,
  `E:\vcpkg-downloads`, `E:\vcpkg-binary-cache`, `E:\gnc-vcpkg-installed`,
  `E:\gnc-*-spike*`, `E:\gnc-vcpkg-*.log`. Nothing in the repo refers to them.
- C: had ~19 GB free; E: ~100 GB. Keep Windows build output on E:.

## 9. Status of the three items, and what replaces them

All three are done or decided:

1. **Event load test** — 20 renames in one burst from the GLib thread, all 20
   delivered to a Qt slot, in order, none lost (`events=20/20 in_order=1`,
   `on_qt_thread=1`).
2. **Report for the loaded book** — the shell drives `gnucash-cli -R run
   --name "<report>"` over the loaded book and displays the result: Trial
   Balance of `sample1.gnucash` renders to 29,538 bytes and the pane reports
   `pane_ok=1`. **Caveat:** the rendering is a child process, not in-process;
   `gnc-report` links GTK3 through `gnc-html`, so the Qt process must not do it.
   Build-tree requirements: `scm-reports`, `scm-reports-standard` and
   `scm-report-stylesheets`, plus `GSETTINGS_SCHEMA_DIR` containing a compiled
   `gschemas.compiled`. Without the stylesheet module every report fails at
   `report-core.scm:459` with `symbol->string` on `#f`, which reads like a
   broken report system rather than a missing module.
3. **Register model** — ADR 0011: decouple the cell layer first, port the view
   second. **First slice done 2026-11-07**: `register-core` no longer includes
   `<gtk/gtk.h>` or links `gnc-gnome-utils`; it reports cell errors to its
   caller and asks the view for text metrics. Verified at link level (no toolkit
   in `NEEDED`, zero toolkit symbols) and by a new test that links the library
   on its own.

What is next, in order:

1. **Phase 3a continues, with `ledger-core`**: it is 19,450 lines, 14 of 28
   files use GTK, five public headers expose `GtkWidget`, and it builds eight
   message dialogs and runs two `gtk_dialog_run` loops. That is the layer a Qt
   view would consume, so it is the real 3a remainder; the
   `doc/register-core-audit.md` records the measurements and the revised 4–8
   week figure. Start by replacing the
   `GtkWidget *parent` API with an opaque handle and the dialogs with returned
   errors, the same shape as the cell slice.
2. **Phase 2 continues**: more actions (View menu, report selection), an account
   tree that shows balances, and preferences read through `gnc-gsettings`
   (ADR 0013). The shell now opens a book and stays open; the next increment is
   parity for browsing.
3. **Prove the model under edits**: the first cut handles renames via
   `dataChanged`; insertions and deletions (`beginInsertRows`) are tested by the
   self-test (`inserts=1 removes=1`) but `parent_of()` no longer asks the engine,
   so a full tree walk still needs a test.

Deferred deliberately: Windows CI for the Qt tier — it would protect a 250-line
stub while the real Windows work is years away (ADR 0010).

## 10. Assumptions that could be wrong

- ~~That the engine's event model is safe to drive from a non-GUI thread.~~ Now
  tested with a 20-event burst from the GLib thread: all delivered, in order, on
  the Qt thread. Still not tested under editing load with a live session (undo,
  transaction commit, dependent objects), which is when the engine is most
  likely to assume the main thread.
- That `QAbstractItemModel` can carry the register's split-row semantics without
  a bespoke proxy. Untested; it is Phase 3's first question.
- ~~That the register audit's "three call sites" bounded Phase 3a.~~ Falsified on
  2026-11-07: the three sites were real, but they were three of eighteen
  affected files, and the layer above the cells (`ledger-core`) is the most
  GTK-bound part of the register. The estimate moved from 1–2 weeks to 2–3
  months; see `doc/register-core-audit.md`.
- That `register-core` being toolkit-free is enough to build a Qt register view.
  It is necessary, not sufficient — `ledger-core` still exposes `GtkWidget` and
  runs modal dialogs, so 3b cannot start on it as it stands.
- That QtWebEngine on MinGW is workable. Verified only indirectly: the pane
  spike runs on Linux; the Windows event-loop spike uses QtCore only.

## 11. First hour of a new session

1. Read this file, then `doc/adr/README.md`, then `doc/qt6-migration-plan.md`.
2. `git log --oneline stable..HEAD` — 56 commits, review as a series.
3. `git status` must be clean; nothing was pushed.
4. Re-run §6 to confirm the claims still hold before building on them. Build in
   the ext4 tree, not through `/mnt/f`, and expect the Qt smoke test to be the
   fastest end-to-end check.
5. Pick from §9; one item per session, verified before moving on.

## 12. Before deleting this file

Move anything still true and not yet recorded into the durable documents:

- phase/status changes → `doc/qt6-migration-plan.md` table;
- any decision taken along the way → a new ADR (never edit an accepted one);
- new environment requirements → `doc/dev-setup-windows.md` (Windows) or
  `AGENTS.md` §3 (general);
- new traps → `AGENTS.md` §9 if they are general, or the plan if migration-only.

Then delete it and remove the reference from `AGENTS.md` and `toplvl_DIST_local`.
