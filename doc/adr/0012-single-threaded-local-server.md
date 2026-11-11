# ADR 0012 — One thread owns the engine, and a token gates the socket

Status: proposed
Branch: `webui/engine-server` (numbering is local to this branch)

## Context

The GnuCash engine is single-threaded by design: there are no locks around
books, accounts or splits, and the session, KVP and query machinery all assume
one owning thread. The Qt tier recorded the same constraint (`ADR 0012` there):
callbacks may arrive on another thread, but every engine access is marshalled
back to the owner.

A local HTTP server has to decide where the engine lives relative to the
socket, and how a browser is allowed to reach it.

## Decision

**The engine is owned by the thread that runs the server.** `scm_boot_guile`
takes the main thread; the session is opened there; the `GMainLoop` that
accepts and answers connections runs there too. Requests are handled serially
on that loop. There is no worker pool and no lock.

**The socket is loopback-only and token-gated.** `gnc-webd` binds
`127.0.0.1` on an ephemeral port and prints a per-run 128-bit token as the
first path segment. A request without the token gets the same `404` as an
unknown path. There is no option to bind a public address.

**`--debug-port [port]` is a development mode, not the way to run the app.**
Bare `--debug-port` binds `8984`; `--debug-port 9100` binds 9100. A value may
be written `host:port`, but the host is refused by name unless it is
`127.0.0.1` or `localhost`: the flag removes the token, so it must not widen
the socket. In this mode the token segment is gone — `/`, `/report/<name>` and
`/api/book` are the routes — the server prints a startup warning saying that
any local process can drive the UI (and, with `--writable`, edit and save the
book), and the page it serves carries a visible banner saying the same, because
"no token" is otherwise invisible. A busy port is a named startup error;
there is never a silent fallback to an ephemeral port, because a stable URL
that quietly is not stable is worse than a refusal. The listener sets
`SO_REUSEADDR`, so a restart can rebind the same port immediately even while
the previous run's accepted connections are in `TIME_WAIT`; a port that is
really being listened on still fails by name. The default (ephemeral port,
per-run token) is unchanged and remains the way to run the server.

**The session is read-only.** `SESSION_READ_ONLY`, and no endpoint writes.
(ADR 0013 later adds an explicit `--writable` opt-in; it does not change the
default.)

## Consequences

* A slow request blocks the others. For one local user and a book that fits in
  memory this is acceptable; it is also the only design the engine supports
  without a locking layer.
* The token is the only credential. It is printed to stdout, so it is as
  private as the terminal; loopback binding means a remote attacker cannot
  reach the port at all.
* Opening the UI from a browser on the same machine is enough; there is no
  TLS, because there is no network hop.
* If a future endpoint must write, it fits the same model — the write happens
  on the owning thread — but the read-only guarantee is what makes this slice
  safe to run against a real book.
