# ADR 0002 — Bridge GLib and Qt with a pumped main context thread

- **Status**: Accepted (2026-11-06)
- **Supersedes**: none
- **Related**: [0001](0001-qt6-presentation-tier.md)

## Context

The engine is GLib-driven. Idle and timeout sources, module loading and
`QofEvent` notification all assume a GLib main loop, while a Qt application runs
its own event loop through `QCoreApplication::exec()`. Any Qt front end has to
let both run without either starving the other or deadlocking on shutdown.

## Options

1. **Pumped context thread.** A dedicated `GMainContext`, owned by one thread,
   advanced with `g_main_context_iteration(context, TRUE)`. Results cross into
   Qt with `QMetaObject::invokeMethod(..., Qt::QueuedConnection)`; work crosses
   back with `g_main_context_invoke()`.
2. **Custom event dispatcher.** Subclass `QAbstractEventDispatcher` and drive
   `g_main_context_prepare/check/dispatch` from inside Qt's loop on the GUI
   thread.
3. **Poll from a timer.** Call `g_main_context_iteration(context, FALSE)` from a
   `QTimer` on the GUI thread.

## Decision

Option 1, the pumped context thread.

## Rationale

- Qt's event loop is left exactly as documented; no dependence on dispatcher
  internals or private API.
- Blocking GLib work cannot stall painting, because it happens on its own
  thread.
- Cross-boundary calls are explicit and individually reviewable, which matters
  when the receiving side is 30 years of engine code.
- Option 2 avoids the thread hop but ties the design to Qt internals and makes
  shutdown ordering harder to reason about. Option 3 drops sources whenever the
  GUI thread is busy.

## Consequences

- Shutdown is explicit: set a stop flag, `g_main_context_wakeup()`, join the
  thread, unref the context. Getting this wrong hangs on exit.
- Nothing may touch engine state on the GLib thread unless it is safe to; the
  marshalling rule applies in both directions.
- Guile remains single-threaded regardless: all Scheme evaluation stays on one
  thread through a serialised queue, so a slow report does not get a second
  interpreter.

## Validation

A standalone ~100-line spike implemented both directions: a `g_timeout_add()`
source on a dedicated context, pumped by `g_main_context_iteration(context,
TRUE)`; ticks marshalled into Qt with the functor overload of
`QMetaObject::invokeMethod` (no `moc` pass needed); and a `QTimer` calling
`g_main_context_invoke()` with a `GSourceFunc` returning `G_SOURCE_REMOVE`.

Measured on Ubuntu 24.04 with Qt 6.4.2 and GLib 2.80: 44 GLib ticks, 44
delivered into Qt, 46 invocations delivered back into GLib, exit 0.

Two defects were found by running it rather than reading about it:
`pkg_check_modules` needs `IMPORTED_TARGET` to define the target it promises,
and `g_main_context_invoke` requires a `GSourceFunc`, not a `void(gpointer)`.

**Not verified:** the same code against a Windows Qt build. The vcpkg `qtbase`
build had not finished, so the Windows side of this result is open.
