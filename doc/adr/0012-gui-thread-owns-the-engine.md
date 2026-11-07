# ADR 0012 — The GUI thread owns the engine

- **Status**: Accepted (2026-11-06)
- **Supersedes**: the thread choice in [0002](0002-event-loop-integration.md); the
  bridge it describes still exists, but it no longer crosses a thread
- **Related**: [0013](0013-coexistence-with-gtk.md)

## Context

[0002](0002-event-loop-integration.md) put the GLib context on a pumped worker
thread and marshalled engine events into Qt with a queued invocation. A spike
proved it worked, and a 20-event burst later proved it held: every event
arrived, in order, on the Qt thread.

That design was chosen when the engine's threading was unknown. What we learned
since:

- the engine is not documented as thread-safe, and GnuCash's GTK application
  mutates it from the main thread;
- the spike itself mutated an account **on the worker thread**, which is the
  pattern a real application must not adopt;
- every call site would have to negotiate which thread may touch the engine, and
  every event would pay a queue hop that exists only to paper over that
  ambiguity.

The cost of the worker thread is permanent and paid at every call site. The
benefit — not blocking painting during GLib work — is real but narrow, and
GnuCash has lived without it for two decades.

## Decision

**The GUI thread owns the engine.** The GLib context is iterated on the GUI
thread, engine calls are made there, and engine events are handled there with no
marshalling and no worker thread.

For now the iteration is a 5 ms `QTimer` calling `g_main_context_iteration()` on
the default context. A `QAbstractEventDispatcher` subclass would integrate more
tightly and is the known upgrade if timer granularity ever shows up in
measurements; it is not worth the complexity before there is evidence.

## Consequences

- Blocking engine work blocks painting. That matches GnuCash's existing
  behaviour, and keeps correctness cheap: one thread, one rule.
- Long-running work (imports, price quotes, report rendering) must **not** go on
  the GUI thread. Those need an explicit worker with a defined handoff, decided
  per feature rather than as a general threading model. Report rendering is
  already a child process, which sidesteps the question for now.
- The queued-invocation machinery in the shell is deleted, not merely unused.
- Any future component that calls the engine from another thread is a bug
  against this ADR, not an optimisation.

## Validation

The shell was refactored to this design and re-run headlessly:

```
report bytes: 29538
accounts=104 top_level=5 pane_ok=1 events=20/20 in_order=1 on_qt_thread=1
shell exit=0
```

Identical to the worker-thread version, with the thread and the queue hop
removed. `g_any_event_off_thread` remains as an assertion: if an event ever
arrives from another thread, the self-test fails.

**Not verified:** behaviour under editing load (undo, transaction commit,
dependent objects); whether any engine subsystem schedules work from a thread of
its own (aqbanking is the usual suspect); and whether a 5 ms timer is fast enough
for engine idles under real use.
