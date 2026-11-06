# Report pane spike

Evidence for the report surface chosen in
[ADR 0001](../0001-qt6-presentation-tier.md): a Qt 6 application hosts
`QWebEngineView`, loads an HTML document, and reads the rendered result back —
with the document produced by **GnuCash's own report engine**, not by hand.

## What is proven

Two programs, run in sequence:

1. `make-html.scm` builds a document with `gnc:make-html-document`, adds a text
   object, and renders it with `gnc:html-document-render`. Output starts:

   ```html
   <!DOCTYPE html>
   <html dir='auto'>
   <head>
   <meta http-equiv="content-type" content="text/html; charset=utf-8" />
   <title>GnuCash report pane spike</title></head>...
   ```

2. `main.cpp` loads that HTML into a `QWebEngineView`, waits for
   `loadFinished`, and reads it back with `toHtml()`.

Observed: 241 bytes generated, `load ok=1`, 246 bytes read back, exit 0. The
counts differ because WebEngine normalises the document; assert on exit status.

## Driving the report engine from outside GnuCash

Four environment variables, all pointing into a configured build tree:

| Variable | Value |
|---|---|
| `GUILE_LOAD_PATH` | `<build>/share/guile/site/3.0` |
| `GUILE_LOAD_COMPILED_PATH` | `<build>/lib/<multiarch>/guile/3.0/site-ccache` |
| `GNC_MODULE_PATH` | `<build>/lib/gnucash` |
| `LD_LIBRARY_PATH` | `<build>/lib:<build>/lib/gnucash` |

The module build is `cmake --build <build> --target scm-report`.

Two failures cost time and are worth knowing:

- A `#!/usr/bin/env guile` line makes Guile treat `#!` as an unterminated block
  comment when it auto-compiles the script. Omit the shebang.
- Without `LD_LIBRARY_PATH`, `(use-modules (gnucash report))` fails at `dlopen`
  of `libgnucash-guile.so`, and the error surfaces as `Unbound variable:
  gnc:make-html-document` — which looks like a missing export, not a missing
  library.

## Running it

```sh
# 1. generate
GUILE_LOAD_PATH=... GUILE_LOAD_COMPILED_PATH=... \
GNC_MODULE_PATH=... LD_LIBRARY_PATH=... \
  guile -s make-html.scm > report.html

# 2. render, headless
cmake -S . -B <build> -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build <build>
QT_QPA_PLATFORM=offscreen \
QTWEBENGINE_DISABLE_SANDBOX=1 \
QTWEBENGINE_CHROMIUM_FLAGS="--no-sandbox --disable-gpu" \
  <build>/spike-report report.html
```

The offscreen platform and sandbox flags are what make it runnable without a
display; without them WebEngine wants a Chromium sandbox and a window.

## Not verified

- **Windows.** `mingw-w64-x86_64-qt6-webengine` was not installed, and the
  Windows toolchain has no Guile, so the report half cannot run there at all.
  The event-loop spike covers the Windows Qt side.
- **CI.** No workflow runs this spike.
- Report *content* beyond a text object — tables, charts, option handling — is
  untested; this proves the transport, not the renderer's fidelity.
