# ADR 0018 — The report pane needs a non-WebEngine backend on Windows

- **Status**: Proposed (2026-11-07)
- **Related**: [0001](0001-qt6-presentation-tier.md) — whose Windows rationale this
  refines
- **Related**: [0007](0007-windows-toolchain-mingw.md) — the toolchain the
  browser cannot be built with
- **Related**: [0015](0015-report-path-out-of-process.md) — reports already
  arrive as HTML from a child process

## Context

ADR 0001 motivates the migration with: "Qt is the only mainstream toolkit with
a maintained embedded browser on all three platforms", because `webkitgtk-3.0`
has no Windows port. ADR 0007 then chose MinGW-w64 through MSYS2 as the
Windows toolchain, and ADR 0010 deferred Windows CI.

Two facts break that combination:

1. **Qt WebEngine does not build with MinGW.** Qt's own platform notes say so
   directly: *"Currently, Qt WebEngine does not compile with MinGW"*, and list
   Visual Studio 2019+ and the Windows 11 SDK as requirements for Windows
   (<https://doc.qt.io/qt-6.8/qtwebengine-platform-notes.html>). They also warn
   against building it with MSYS2 tools.
2. **MSYS2 does not package it.** Its mingw64 repository carries
   `mingw-w64-x86_64-qt6-base` (installed here, 6.9.1) and no
   `qt6-webengine` at all — checked against the local sync database.

So on the chosen Windows toolchain the Qt *widgets* tier is buildable and the
*report pane* is not. `gnucash-qt` will not link there, which means the
migration's stated purpose — a Windows GnuCash without the jhbuild
`gnucash-on-windows` environment — is not reachable as the plan stands.

The other half of the Windows problem is already solved: `gnucash-on-windows`
builds patched Guile 2.2, patched libdbi and (until Qt) WebKitGTK for mingw
(`gnucash.modules`: `guile2`, `libdbi`; `dependencies/mingw-w64-webkitgtk`). A
native Windows GnuCash exists today. Replacing its presentation tier is the
part that needs a renderer that actually works on mingw.

## Options

1. **Switch the Windows toolchain to MSVC.** Then Qt WebEngine works — and
   every GnuCash dependency (Guile, glib, libxml2, boost, libdbi) has to be
   rebuilt with MSVC. `HACKING` and `cmake/README_CMAKE.txt` declare MSVC out
   of scope, and this is a multi-month dependency project on its own.
2. **Keep MinGW and give the report pane a per-platform backend.** Reports
   already arrive as HTML from `gnucash-cli` (ADR 0015), so the pane is a
   viewer, not a renderer. On Windows it can be **WebView2** (the Edge runtime
   that ships with Windows 11) or the **system browser**, behind a small
   interface; Linux and macOS keep Qt WebEngine.
3. **Wait for MSYS2 to package WebEngine.** Upstream states it does not
   compile with MinGW; packaging it would mean patching Chromium for a
   toolchain Qt does not support.

## Decision

*Proposed:* option 2.

Because ADR 0015 already moved report *generation* out of process, the pane
only needs `setHtml`-shaped behaviour, and the interface is small:

```
ReportView:  void show_html(const QByteArray&);
             bool is_loaded() const;
```

- Linux/macOS: `QWebEngineView` (today's implementation).
- Windows: WebView2 first choice (embedded, no external window, ships with the
  OS); the system browser as the fallback if the WebView2 loader proves
  awkward under MinGW.
- The rest of the Qt tier — account tree, tabs, preferences, dialogs — is
  ordinary Qt Widgets and builds under MinGW today.

A short spike decides WebView2 versus the external browser before Phase 3b
starts, because 3b is five to seven months and its whole justification is
Windows.

## Consequences

- ADR 0001's "one embedded browser on all three platforms" becomes "one report
  path, a per-platform viewer". The migration keeps its reason — no WebKitGTK
  anywhere — but stops promising one renderer everywhere.
- `qt/` gains a report-view interface and two implementations instead of one
  `QWebEngineView`.
- Windows packaging (Phase 7) must carry the WebView2 loader, or document the
  external-browser fallback.
- The Windows toolchain decision (ADR 0007) stands for everything except the
  browser. That is a much smaller exception than moving to MSVC.
- A Windows build also still needs the `gnucash-on-windows` dependency
  environment (patched Guile, libdbi); the Qt tier removes the toolkit, not
  the engine's dependencies. That is worth saying plainly in the plan.

## Validation

Nothing implemented. Verified by inspection and upstream documentation:

- Qt WebEngine platform notes (6.8), quoted above;
- the MSYS2 mingw64 package list on this machine — `qt6-base` present,
  `qt6-webengine` absent;
- `gnucash-on-windows`' `gnucash.modules` and `dependencies/`, which build
  Guile 2.2, libdbi and package WebKitGTK for mingw.

**Not verified:** that WebView2 can be embedded from a MinGW-built Qt
application, and that the resulting pane behaves like the WebEngine one for
GnuCash's report HTML. That is the spike this ADR asks for.
