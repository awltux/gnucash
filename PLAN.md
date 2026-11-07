# PLAN.md — working notes for the Qt 6 migration

Operational continuity notes: what is done, what is running, what is next, and
which traps already cost time. Kept in the tree so that a session can resume
without re-deriving any of it.

When this file and the durable records disagree, the durable records win:

- `doc/qt6-migration-plan.md` — phases, gates, status table
- `doc/adr/0001…0010` — one decision per record; `doc/adr/README.md` is the index

What belongs here rather than there: build trees, verified-versus-assumed
status, and the mistakes that were expensive. Anything that is a decision gets
an ADR instead.

---

## 1. The goal

Replace GnuCash's GTK 3 presentation tier with Qt 6, **without touching the
accounting engine, the storage backends or the file formats**.

Why: `CMakeLists.txt` requires `webkitgtk-3.0` (WebKit1, ~2016) on Windows, and
WebKitGTK has no Windows port, so a Windows build needs the out-of-tree
`gnucash-on-windows` jhbuild environment. Qt is the only mainstream toolkit with
a maintained embedded browser on Linux, Windows and macOS. GTK 3 is also in
maintenance mode.

What "done" means: a Qt presentation tier at parity, shipped alongside the GTK
one for at least a release cycle, reading and writing byte-identical books.

## 2. Invariants — do not violate these

- **The engine owns the format.** `libgnucash/engine` and `libgnucash/backend/*`
  define the XML v2 file, the SQL store and the KVP slots. A UI never bypasses
  them, and no UI phase changes them.
- **Compatibility gate.** Every corpus book round-trips; a book written by the
  Qt build opens in the GTK build of the same series and vice versa; the version
  string, `GNUCASH_RESAVE_VERSION` and `GNC_FEATURE_*` entries are identical.
- **Verified or blocked.** Nothing is committed as "working" without a run that
  shows it. When something cannot be verified, record it as blocked with the
  concrete reason.
- **Small commits, one idea each.** The branch is reviewed as a series.

## 3. Where things are

| Thing | Location |
|---|---|
| Branch | `phase0/qt6-groundwork`, 35 commits ahead of `stable`, **not pushed** |
| Linux build tree | `~/build/linux-debug` in WSL (currently configured `WITH_QT=OFF`) |
| Qt shell binary | `~/build/linux-debug/bin/gnucash-qt` (needs `-DWITH_QT=ON` to exist) |
| Report HTML generator | `doc/adr/spike-report-pane/make-html.scm` |
| Book fixture | `libgnucash/backend/xml/test/test-files/load-save/sample1.gnucash` |
| Windows scratch | `E:\gnc-qt-spike`, `E:\gnc-report-spike`, `E:\gnc-shell-spike` (not in repo) |
| MSYS2 | `C:\msys64`, use the **MINGW64** shell; Qt 6.9.1, GLib 2.84.2 installed |

## 4. Phases

Estimates are from `doc/qt6-migration-plan.md`. Status as of the last session.

### Phase 0 — Groundwork ✅ done

*Why:* settle the toolchain and dependency story before writing UI code, because
both were wrong in ways that only showed up when executed.
*How:* build presets, a dependency decision, CI, an event-loop spike, a licence
decision.

- `CMakePresets.json` (linux/macos/windows), verified on CMake 4.3.1 and 3.28.3.
- ADR 0007: Windows toolchain is **MinGW-w64/MSYS2**, prebuilt Qt. Not vcpkg —
  vcpkg compiles Qt from source (1585 debug steps) and never finished here.
- ADR 0008: CI installs the documented MSYS2 packages and runs the event-loop
  spike.
- ADR 0002 + spike: GLib↔Qt bridge, verified on **both** platforms
  (Linux 44/44/46, Windows 47/47/49).

### Phase 1 — Headless plateau 🟡 mostly done

*Why:* prove the three risky integrations before any UI volume: backend loading,
report rendering, and events crossing threads.
*How:* a Qt program that opens a book, renders report HTML, and observes engine
events.

- ADR 0009: backend registration is `dlopen` + `qof_backend_module_init`. A
  book loads: 104 accounts.
- Report pane: report-engine HTML renders in `QWebEngineView` headlessly
  (ADR 0001 chose the renderer; the spike proves the path).
- Event bridge: rename on the GLib thread observed in a Qt slot
  (`event_on_qt_thread=1`).

**Not done:** a report rendered *for the loaded book* (needs the Guile session
bound to the book). Everything so far renders a standalone document.

### Phase 2 — Read-only shell 🔴 just started

*Why:* the first thing a user would recognise as an application.
*How:* target `gnucash-qt` gated behind `WITH_QT` (ADR 0010), account tree +
report pane, then menus, actions, tabs, preferences.

- Target exists with an account tree and a pane. No menus, no actions, no
  editing, no save.

### Phase 3 — Register (the hard one) ⬜ not started

*Why:* 36k lines, 50 files, a hand-drawn grid with a 30-year-old keyboard model.
It is the single largest risk in the programme.
*How:* `QAbstractItemModel` over engine cursors, painted view + delegates,
parity checklist, performance gates (20k splits < 1s open, 60fps scroll,
<50ms typing latency).
**Open question:** does the split-row model map cleanly, or does it need a
proxy layer? Answer this before writing widget code.

### Phase 4 — Editing ⬜

*Why:* the tier is useless if it cannot write.
*How:* transaction/split editors, reconcile, scheduled transactions, budgets,
assistants. Exit gate: every corpus write path reachable, interop gate green.

### Phase 5 — Import/export and business ⬜

*Why:* QIF/CSV/OFX and invoices are most of what small-business users do.
*How:* port the import matchers and business documents; aqbanking.

### Phase 6 — Report UI ⬜

*Why:* reports are the product for many users; options are how they customise.
*How:* option widgets, custom report management, print/PDF. Gate: user Scheme
reports load and run unmodified.

### Phase 7 — Parity hardening ⬜

*Why:* accessibility, HiDPI, RTL and packaging are what make it shippable.
*How:* `QAccessible`, keyboard-only flows, i18n audit (keep gettext, **not**
`.ts`), MSIX/Inno/Flatpak/notarised DMG.

### Phase 8 — Optional, post-transition

`QSettings` replacing GSettings with a one-time import; `pybind11` replacing
SWIG; direct SQLite/libpq replacing libdbi behind the same interface. None
changes the on-disk format.

## 5. What is verified, and what merely runs

- ✅ Event bridge both platforms; backend registration; book load (104 accounts);
  report pane renders report-engine HTML; `WITH_QT=ON` builds and runs;
  `WITH_QT=OFF` configures with zero Qt targets.
- ⚠️ Report **for a book** — untested.
- ⚠️ Event **load** (many events, ordering, no drops) — untested.
- ❌ Windows engine/shell build — impossible today: no mingw Guile, no libdbi.
- ❌ CI coverage for the Qt target.

## 6. Next three steps

1. **Report for the loaded book.** Bind the Guile session to the book and render
   by report name. Last unproven piece of Phase 1.
2. **Event load test.** N modifications from the GLIB thread, assert the Qt slot
   sees all of them in order. Architectural risk; cheap to test.
3. **Register model note.** Decide `QAbstractItemModel` mapping for split rows
   before Phase 3 starts.

Deferred on purpose: Windows CI for the Qt tier. It would protect a 250-line stub
while the real Windows work is years away. See the note in ADR 0010.

## 7. Traps that already cost time

- **Configure takes ~15–20 minutes** (`891 s`, `1161 s` observed). Every edit to
  a `CMakeLists.txt` forces a regeneration on the next build. Batch CMake edits;
  do not iterate on them casually.
- **`po/POTFILES.in` is tracked** and regenerated by configure. New `.cpp`
  and `.scm` files (spikes, `qt/main.cpp`) must be added to `po/POTFILES.ignore`,
  and `POTFILES.in` reverted, or commits pick up noise.
- **Driving the report engine outside GnuCash** needs four variables:
  `GUILE_LOAD_PATH` (`<build>/share/guile/site/3.0`),
  `GUILE_LOAD_COMPILED_PATH` (`<build>/lib/x86_64-linux-gnu/guile/3.0/site-ccache`),
  `GNC_MODULE_PATH` (`<build>/lib/gnucash`), `LD_LIBRARY_PATH`
  (`<build>/lib:<build>/lib/gnucash`). Missing the last one reports as
  `Unbound variable: gnc:make-html-document`, which looks like a missing export.
- **A `#!/usr/bin/env guile` shebang** becomes an unterminated block comment
  under Guile's auto-compilation. Omit it.
- **WebEngine headless** needs `QT_QPA_PLATFORM=offscreen`,
  `QTWEBENGINE_DISABLE_SANDBOX=1`,
  `QTWEBENGINE_CHROMIUM_FLAGS="--no-sandbox --disable-gpu"`.
- **Never build Qt from source on Windows.** vcpkg compiles debug *and* release;
  `VCPKG_BUILD_TYPE` is a triplet setting, not an environment variable. It ran
  4+ hours and died in `libiconv`. Use MSYS2 packages.
- **Shell quoting through PowerShell into WSL/MSYS2 mangles inline bash.** Write
  a script file and run that; every attempt to inline pipes or quotes failed.
- **`gnc_account_get_name` does not exist** — the C API still uses
  `xaccAccountGetName`, and the compiler suggests `gnc_account_get_type`, which
  sends you nowhere useful.
- `.gitattributes` has no `*.md` rule, so every Markdown write emits a CRLF
  warning. Harmless; blobs are stored LF.

## 8. Open decisions

1. **Register model**: direct `QAbstractItemModel`, or a proxy for split rows?
2. **Report for a book**: how the Guile session and the `QofBook` are bound.
3. **When Windows CI arrives** (criteria are listed in ADR 0010).
4. **GLib lifetime**: today the pumped context is started per process; a real
   app needs an explicit lifecycle with the engine's session.
