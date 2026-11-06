# ADR 0001 — Qt 6 presentation tier, unchanged accounting engine

- **Status**: Accepted (2026-11-06)
- **Supersedes**: none
- **Related**: [0002](0002-event-loop-integration.md),
  [0003](0003-windows-dependencies-vcpkg.md),
  [0004](0004-html-layer-separation.md),
  [0005](0005-windows-ci-scope.md)
- **Plan**: [qt6-migration-plan.md](../qt6-migration-plan.md) holds phases,
  gates and status; this record holds the decision only.

## Context

GnuCash ships a GTK 3 desktop application whose HTML reports are rendered by
WebKitGTK. Two things follow:

1. **Windows is a second-class platform.** `CMakeLists.txt` requires
   `webkitgtk-3.0` (WebKit1, circa 2016) whenever `WIN32` is set. WebKitGTK has
   no Windows port, so a Windows build needs the out-of-tree
   `gnucash-on-windows` jhbuild environment, which rebuilds Guile and libdbi
   from patched source.
2. **GTK 3 is in maintenance mode.** Active toolkit development is on GTK 4.x,
   and contributor familiarity with GTK 3 continues to decline.

Qt 6 is the only mainstream toolkit with a maintained embedded browser
(QtWebEngine) on Linux, Windows and macOS, and the only option that makes MSVC a
supported compiler for this project.

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
the Guile report engine unchanged.** Ship `WITH_GTK` and `WITH_QT` builds from
one tree for at least one release cycle so users and packagers can fall back.

Qt supplies widgets, printing, settings and report rendering. GLib/GObject stay
inside the engine: they are maintained, cross-platform, and removing them would
break the C API that SWIG bindings and user Scheme reports depend on.

**Why not reimplement the engine.** The engine owns the XML v2 format, the SQL
triple store and the KVP slots. A front end running on the same engine writes
byte-identical books by construction. A second implementation of `QofBook`
serialization is explicitly out of scope, and the invariants that follow from
this are stated once in the plan's compatibility gate rather than repeated here.

## Licensing

GnuCash is GPL-2.0-or-later, so the combined work may be distributed under
GPL-3.0, which Qt's LGPL-3.0 permits. Adopting Qt therefore relicenses the
binary distribution to GPL-3.0. That is accepted as a consequence of this
decision rather than deferred: the "or later" clause makes the change
permissible, and the source obligation is unchanged.

## Consequences

**Positive.** Windows becomes a normal target; the WebKit 1 dependency
disappears; the register gains model/view separation; GTK 3 EOL stops being a
scheduling constraint.

**Negative.** 212k lines of UI are rewritten; QtWebEngine is a large dependency;
GNOME integration is partially replaced; the register is a 36k-line custom
widget with a 30-year-old keyboard model that is easy to regress.

**Neutral.** The C API, the Guile report API, the gettext catalogs and the file
formats are untouched.

## Alternatives considered

- **GTK 4 + WebKitGTK 6.0.** Lower churn and keeps the GNOME ecosystem, but does
  not solve Windows, because WebKitGTK still has no Windows port. Remains the
  fallback if Windows parity is dropped.
- **Web UI (Tauri/Electron).** Reports are already HTML, but a virtualised grid
  with inline editing, multi-row selection and the existing keyboard model is
  the weakest part of web UIs and the strongest requirement here.
- **Keep GTK 3 and maintain jhbuild.** Status quo; inherits an unmaintained
  browser engine and blocks Windows contributors.

## Open questions

1. Does the register model map cleanly onto `QAbstractItemModel`, or does it
   need a proxy layer for split rows?
2. Which packaging formats are in scope at Phase 7 (MSIX, Inno Setup, Flatpak,
   notarised DMG)?
