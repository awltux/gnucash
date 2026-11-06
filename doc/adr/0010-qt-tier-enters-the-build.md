# ADR 0010 — The Qt tier enters the build as an opt-in target

- **Status**: Accepted (2026-11-06)
- **Supersedes**: none
- **Related**: [0001](0001-qt6-presentation-tier.md),
  [0002](0002-event-loop-integration.md), [0009](0009-backend-registration-in-qt.md)

## Context

Phase 1 was validated entirely by spike programs kept next to the ADRs. They
proved the individual risks — event-loop bridge, backend registration, report
pane, and that the pieces coexist — but a spike is not a target: nothing in the
build compiled them, so they could rot without any test noticing, and Phase 2
has nowhere to grow.

The default build must not gain a Qt dependency. GnuCash ships a GTK
application, CI has no Qt 6 Widgets or WebEngine, and packagers must not be
forced to install a toolkit for a tier that is not shipped yet.

## Decision

Add `WITH_QT`, default **OFF**, and a `qt/` subdirectory containing the target
`gnucash-qt`. It links `gnc-engine`, `gnc-core-utils`, Qt 6 Widgets and
WebEngine, and is added to the build only when the option is on. Its files join
the distribution only when the directory is added.

## Consequences

- `cmake -DWITH_QT=ON` is required; without it nothing changes for anyone.
- The target is not in CI. It should be, once a runner has WebEngine; until then
  a Qt build break is found by whoever runs it locally. That is a real gap, not
  a theoretical one.
- `qt/` starts as one 250-line program. It is a place for the shell to grow, not
  yet an architecture: no plugin system, no settings, no actions.
- Installing requires Qt 6 at configure time; the install rule therefore only
  exists in a Qt-configured build.

## Validation

`cmake -DWITH_QT=ON` configures and builds `bin/gnucash-qt` in the existing
debug tree on Ubuntu 24.04, and the headless run reports:

```
accounts=104 top_level=5 pane_ok=1 event_on_qt_thread=1 events=1 renamed_to=Renamed by the GLib thread
```

exit 0. The account count comes from `sample1.gnucash`; the rename is performed
by a `g_timeout_source` on the pumped `GMainContext` from
[0002](0002-event-loop-integration.md), and the observation of the new name
happens in a Qt slot. That is the structural claim Phase 2 needed: an engine
change made on the GLib thread reaches the Qt thread, not merely a Qt object.

**Not verified:** the same on Windows (the toolchain has no Guile), CI coverage,
and any interactive behaviour — there are no menus, no editing and no save.
