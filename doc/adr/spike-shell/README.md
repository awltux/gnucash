# Shell spike: engine and report pane in one process

The two halves were proven separately — a Qt process loading a book
([ADR 0009](../0009-backend-registration-in-qt.md)) and a `QWebEngineView`
rendering report-engine HTML. This joins them in one window, which is the shape
Phase 1 needs.

Left pane: accounts read from a real book through `libgnucash`.
Right pane: HTML produced by GnuCash's report engine.

## What it proves

- `libgnucash` and `QWebEngineView` coexist in one process without competing for
  globals, which was the open question.
- A book loads and its account tree is available for display.
- Report-engine output renders in the pane at the same time.

Observed on Ubuntu 24.04:

```
accounts=104 top_level=5 html_ok=1 rendered_bytes=246
shell exit=0
```

## What it does not prove — read this before citing it

- **No GLib events flow.** The book is loaded synchronously; nothing subscribes
  to `QofEvent`, and the pumped `GMainContext` from
  [ADR 0002](../0002-event-loop-integration.md) is not running here. The shell
  proves *hosting*, not *event integration*. Editing a transaction and watching
  a pane update is the untested path, and it is the one Phase 2 depends on.
- **The report is not about the book.** The HTML comes from a standalone
  document built by `make-html.scm`, not from a report rendered for
  `sample1.gnucash`. Report options, report lookup by name and the Guile↔engine
  session binding are all untested.
- **Nothing is interactive.** No menus, no editing, no session save.
- **Windows is untested**, and cannot be: the toolchain has no Guile.

## Building and running it

The report document comes from the report pane spike:

```sh
# see doc/adr/spike-report-pane/README.md for the environment variables
GUILE_LOAD_PATH=... GUILE_LOAD_COMPILED_PATH=... \
GNC_MODULE_PATH=... LD_LIBRARY_PATH=... \
  guile -s ../spike-report-pane/make-html.scm > report.html
```

Then build and run against a configured build tree (`$BUILD`):

```sh
g++ -std=c++17 -fPIC -O1 \
    -I"$SRC/libgnucash/engine" -I"$SRC/common" -I"$BUILD/common" \
    $(pkg-config --cflags Qt6Widgets Qt6WebEngineWidgets glib-2.0 gobject-2.0) \
    main.cpp -o shell-spike \
    -L"$BUILD/lib" -lgnc-engine -lgnc-core-utils \
    $(pkg-config --libs Qt6Widgets Qt6WebEngineWidgets glib-2.0 gobject-2.0) -ldl

QT_QPA_PLATFORM=offscreen \
QTWEBENGINE_DISABLE_SANDBOX=1 \
QTWEBENGINE_CHROMIUM_FLAGS="--no-sandbox --disable-gpu" \
LD_LIBRARY_PATH="$BUILD/lib:$BUILD/lib/gnucash" \
  ./shell-spike "$BUILD/lib/gnucash/libgncmod-backend-xml.so" \
                "$SRC/libgnucash/backend/xml/test/test-files/load-save/sample1.gnucash" \
                report.html
```

The program exits 0 only when the book loaded, the pane reported success, and
the document was read back non-empty; otherwise it prints why and exits non-zero.
`gnc_account_get_name` does not exist — the C API still uses
`xaccAccountGetName`, despite the `gnc_` naming elsewhere.
