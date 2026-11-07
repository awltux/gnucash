# ADR 0015 — Reports leave the process until the report layer is GTK-free

- **Status**: Accepted (2026-11-06)
- **Related**: [0004](0004-html-layer-separation.md), [0013](0013-coexistence-with-gtk.md)

## Context

[0013](0013-coexistence-with-gtk.md) recorded the helper-process report path as a
stopgap. That left a choice open: keep spawning `gnucash-cli` for the foreseeable
phases, or de-GTKify the report layer early so reports can render in-process.

The facts that decide it:

- `gnc-report` links `PkgConfig::GTK3` and depends on `gnc-html`, where WebKit
  lives. In-process rendering puts GTK inside the Qt application.
- The helper path works today: `gnucash-cli -R run --name "<report>"` renders a
  29,538-byte Trial Balance for the fixture book, and the pane displays it.
- Report *options* are where this becomes visible: a report is not a static
  document, and editing its options means re-running it. A child process per
  option change is a poor user experience, not a scaling problem.
- Phase 6 is the report UI phase. Doing the de-GTKification now would mean
  removing GTK from the HTML layer before the register's own decoupling, with no
  shipping milestone between the two.

## Decision

**Reports render out-of-process through Phase 5.** The de-GTKification of
`gnc-report`/`gnc-html` becomes the opening job of **Phase 6**, before any report
option UI is built.

The Qt side defines the seam now so the swap is not a rewrite: report rendering
is a single `ReportRunner` unit with `render(book, report_name, output_path)`.
One implementation shells out; a later one calls the engine directly. The window
does not know which is in use.

Revisit criteria — if any becomes true before Phase 6, this ADR is superseded
rather than stretched:

1. Report options must be interactive for a demonstrated user need.
2. A platform needs reports but cannot run `gnucash-cli` (a Windows packaging
   question, not a code question).
3. Spawn cost is measured and unacceptable (>1 s per report on a typical book).

## Consequences

- Every report costs a process spawn and a book load in that process. For a
  single report on open this is acceptable; it is why criterion 3 exists.
- The pane cannot reflect live data changes without a re-run. Accepted for now;
  Phase 6 is where reports become interactive.
- Phase 6 grows: it starts with removing GTK from the HTML layer, which is also
  what a GTK-free Windows build needs.
- The `ReportRunner` seam is the only place that knows how reports are produced,
  so the migration is contained.

## Validation

The helper implementation is the current code path, observed working: 29,538
bytes rendered for `sample1.gnucash`, `pane_ok=1`, exit 0. The in-process
implementation does not exist and is not claimed.
