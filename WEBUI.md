# The GnuCash web UI

`gnc-webd` opens a book with the GnuCash engine and serves it to a browser on
`127.0.0.1`. It links no toolkit: the presentation layer is HTML in a browser,
so the engine runs unchanged on platforms where the GTK (or Qt) front end is
the problem — Windows in particular.

    branch: webui/engine-server, cut from stable

## Why this exists

The Qt migration stalled on a Windows fact: Qt WebEngine does not compile with
MinGW, and MSYS2 ships `qt6-base` but no `qt6-webengine` — so the report viewer,
which is WebKit-based today, has no Qt-on-Windows answer. A browser is the one
renderer that is already installed, already sandboxed and already maintained.
The engine does not care who displays its HTML.

## Build

    cmake -S . -B build -G Ninja \
        -DWITH_GNUCASH=OFF -DWITH_REPORT=ON -DWITH_WEBUI=ON
    cmake --build build --target gnc-webd

`WITH_GNUCASH=OFF` skips the GTK application, and the configure requests no
GTK or WebKit packages at all. `WITH_REPORT` is on by default; `WITH_WEBUI`
is off by default while this is new.

## Run

    GNC_UNINSTALLED=YES GNC_BUILDDIR=$PWD/build \
    GNC_MODULE_PATH=$PWD/build/lib/gnucash \
    GUILE_LOAD_PATH=$PWD/build/share/guile/site/3.0 \
    build/bin/gnc-webd --backend build/lib/gnucash/libgncmod-backend-xml.so book.gnucash

It prints one line and then serves:

    webd listening on http://127.0.0.1:41234/9f2c...e1/

The path segment is a 128-bit token generated per run. Without it every path
answers 404. The socket binds the loopback address and there is no option to
change that. The session is opened read-only; there are no write endpoints.

## API

| path | returns |
|---|---|
| `/<token>/` | the UI (`web/ui/index.html`) |
| `/<token>/api/book` | `{"accounts":N,"top_level":M}` |
| `/<token>/api/accounts` | the account tree with formatted totals |
| `/<token>/report/<name>` | the report's HTML, rendered in this process |

Report names are URL-decoded, so `/report/Trial%20Balance` works.

## How it is put together

* **One thread.** The engine has no locking model; it is owned by the thread
  that initialises it. `scm_boot_guile` takes the main thread, the report
  engine initialises there, the book is opened there, and every request is
  handled on the `GMainLoop` that follows (ADR 0012).
* **In-process reports.** The report engine is toolkit-free — its font comes
  from a provider hook, its URL types from `gnc-html-url`, and its dialogs and
  progress bar from `(gnucash report report-gui)` — so a report is rendered
  with `gnc_run_report_with_error_handling` in the server's own process. No
  `gnucash-cli` child, no WebKit (ADR 0011).
* **GIO for the socket.** `GSocketService` plus a minimal HTTP/1.1 read/write.
  No new dependency for the transport; no framework in the UI.

## Testing

    web/run-smoke-test.sh <build-dir> <source-dir>

Starts the server on an ephemeral loopback port, reads the token from its
output, and checks the counts, the tree JSON, a rendered report, the tokenless
404, and that the UI is served. It passes both in a GTK build and in a
toolkit-free one.

## Limits (deliberate, for now)

* Read-only: no editing, no saving.
* One user, one book, one process; no concurrency beyond serialising requests.
* Plain HTTP on loopback; the token is the only credential.
* The UI is vanilla HTML/CSS/ES modules. No framework has been chosen yet —
  the JSON API is what a framework would sit on.

## Related

* `doc/adr/0011-reports-in-the-server-process.md`
* `doc/adr/0012-single-threaded-local-server.md`
