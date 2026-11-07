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
| Branch | `phase0/qt6-groundwork`, 36 commits ahead of `stable`, **never pushed** |
| Review order | ADRs → `doc/qt6-migration-plan.md` → build config → `qt/` → `doc/adr/spike-*` |
| Linux build tree | `~/build/linux-debug` (WSL). **Currently `WITH_QT=OFF`** |
| Core-only tree | `~/build/linux-core` (from the `linux-core` preset) |
| In-repo build dir | `.build/linux-core` (created by the preset; gitignored) |
| Qt shell binary | `~/build/linux-debug/bin/gnucash-qt` — needs `-DWITH_QT=ON` |
| Book fixture | `libgnucash/backend/xml/test/test-files/load-save/sample1.gnucash` (104 accounts) |
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

### Phase 3 — Register (the hard one) ⬜ not started

*Why:* 36k lines across 50 files, hand-drawn, with a 30-year-old keyboard model.
The single largest risk in the programme.
*How:* `QAbstractItemModel` over engine cursors, painted view + delegates,
parity checklist, performance gates (20k splits < 1s open, 60 fps scroll,
<50 ms typing latency).
**Decide first:** does the split-row model map cleanly, or does it need a proxy
layer? Answer before writing widget code.

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
| Report **for a loaded book** | ⚠️ untested |
| Event **load** (many events, ordering, no drops) | ⚠️ untested |
| Windows build of engine/shell | ❌ impossible today (no mingw Guile, no libdbi) |
| CI coverage of the Qt target | ❌ none |

## 6. How to re-verify, from scratch

Configure with the tier on (≈15–20 min, see §7):

```sh
cmake -S . -B ~/build/linux-debug -DWITH_QT=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build ~/build/linux-debug --target gnucash-qt -j 6
```

Then the headless self-test — report HTML from the report engine, book from the
fixture:

```sh
BUILD=~/build/linux-debug; SRC=$PWD
GUILE_LOAD_PATH="$BUILD/share/guile/site/3.0" \
GUILE_LOAD_COMPILED_PATH="$BUILD/lib/x86_64-linux-gnu/guile/3.0/site-ccache" \
GNC_MODULE_PATH="$BUILD/lib/gnucash" \
LD_LIBRARY_PATH="$BUILD/lib:$BUILD/lib/gnucash" \
  guile -s doc/adr/spike-report-pane/make-html.scm > /tmp/report.html

QT_QPA_PLATFORM=offscreen QTWEBENGINE_DISABLE_SANDBOX=1 \
QTWEBENGINE_CHROMIUM_FLAGS="--no-sandbox --disable-gpu" \
LD_LIBRARY_PATH="$BUILD/lib:$BUILD/lib/gnucash" \
  "$BUILD/bin/gnucash-qt" \
  "$BUILD/lib/gnucash/libgncmod-backend-xml.so" \
  "$SRC/libgnucash/backend/xml/test/test-files/load-save/sample1.gnucash" \
  /tmp/report.html
```

Expected: `accounts=104 top_level=5 pane_ok=1 event_on_qt_thread=1 events=1 …`,
exit 0. Anything else means the tree, the environment, or the code moved.

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

## 9. Next three steps

1. **Report for the loaded book.** Bind the Guile session to the book, render by
   report name. Last unproven piece of Phase 1.
2. **Event load test.** N modifications from the GLib thread; assert the Qt slot
   sees all, in order, no deadlock. Cheap, and architectural.
3. **Register model note.** Decide `QAbstractItemModel` mapping for split rows
   before Phase 3 starts.

Deferred deliberately: Windows CI for the Qt tier — it would protect a 250-line
stub while the real Windows work is years away (ADR 0010).

## 10. Assumptions that could be wrong

- That the engine's event model is safe to drive from a non-GUI thread. The
  spike renames an account from the GLib thread and it works, but that is one
  event, not a workload. If the engine assumes the main thread, the whole
  pumped-context design (ADR 0002) needs revisiting.
- That `QAbstractItemModel` can carry the register's split-row semantics without
  a bespoke proxy. Untested; it is Phase 3's first question.
- That QtWebEngine on MinGW is workable. Verified only indirectly: the pane
  spike runs on Linux; the Windows event-loop spike uses QtCore only.

## 11. First hour of a new session

1. Read this file, then `doc/adr/README.md`, then `doc/qt6-migration-plan.md`.
2. `git log --oneline stable..HEAD` — 36 commits, review as a series.
3. `git status` must be clean; nothing was pushed.
4. Re-run §6 to confirm the claims still hold before building on them.
5. Pick from §9; one item per session, verified before moving on.

## 12. Before deleting this file

Move anything still true and not yet recorded into the durable documents:

- phase/status changes → `doc/qt6-migration-plan.md` table;
- any decision taken along the way → a new ADR (never edit an accepted one);
- new environment requirements → `doc/dev-setup-windows.md` (Windows) or
  `AGENTS.md` §3 (general);
- new traps → `AGENTS.md` §9 if they are general, or the plan if migration-only.

Then delete it and remove the reference from `AGENTS.md` and `toplvl_DIST_local`.
