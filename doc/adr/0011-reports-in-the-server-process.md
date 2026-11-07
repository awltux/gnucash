# ADR 0011 — Reports are rendered in the server process

Status: proposed
Branch: `webui/engine-server` (numbering is local to this branch; `stable`
carries no `doc/adr/`)

## Context

Reports were the reason the report layer could not be detached from GTK. The
engine's report code (`gnc-report`) linked `gtk+-3.0` for a font lookup, and
the report *Scheme* pulled in `(gnucash html)` and `(gnucash gnome-utils)`,
which load `libgnc-html` (GTK + WebKit) and `libgnc-gnome-utils` (GTK dialogs).
Anything that wanted a report dragged the toolkit in.

The alternatives for a web server were therefore to spawn `gnucash-cli` for
every report — a child process, a GTK runtime dependency, and a serialisation
boundary — or to make the report engine toolkit-free.

## Decision

The report engine is toolkit-free and the server renders reports in its own
process.

Four couplings were removed, each at the seam that actually existed:

| what reports needed | how it is provided now |
|---|---|
| the display font | `gnc_report_set_font_family_func()`; the GTK app installs its theme lookup, headless gets `Arial` |
| URL types (`gnc_build_url`) | moved out of `gnc-html.cpp` into the toolkit-free `gnc-html-url` library |
| error/warning dialogs | `(gnucash report report-gui)` parameters; the GTK app installs `gnc-error-dialog` etc. |
| the progress bar | same module; headless default does nothing |

The report subtree is its own CMake unit (`WITH_REPORT`), built before the
GNOME tree and buildable with `WITH_GNUCASH=OFF`.

## Consequences

* `gnc-webd` renders with `gnc_run_report_with_error_handling` on the thread
  that owns the engine — no child process, no IPC, no WebKit.
* `libgnc-report.so` has no toolkit `NEEDED` entries, and neither does
  `gnucash-cli`; a GTK-free build produces reports.
* The GTK build is unchanged: both baseline reports are byte-identical before
  and after, because the GTK application installs the same font lookup and
  the same dialogs it used to call directly.
* Reports no longer re-export `(gnucash html)`. In-tree report code never used
  those functions; an out-of-tree report that did would have to import the
  module itself (and then it needs GTK, as before).
