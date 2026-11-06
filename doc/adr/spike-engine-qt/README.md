# Qt + libgnucash spike

Evidence for [ADR 0009](../0009-backend-registration-in-qt.md): a Qt process can
start the engine, register a storage backend and open a real book.

It answers, in the order things fail:

1. do Qt and the engine coexist in one process?
2. how does a non-GnuCash process register a backend?
3. does a real book load and expose its objects?

## The registration trick

`gncmod-backend-xml` is built as a CMake `MODULE`, so there is nothing to link
against. The spike `dlopen`s it and calls its init entry point, which is what
`gnc_module_load()` does underneath:

```cpp
void* handle = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
auto init = reinterpret_cast<InitFn>(dlsym(handle, "qof_backend_module_init"));
// falls back to gnc_module_init_backend_xml
init();
```

`qof_init()` and `cashobjects_register()` must run first.

## Building it

Against a configured build tree (`build` below is the CMake binary directory):

```sh
g++ -std=c++17 -fPIC -O1 \
    -I"$SRC/libgnucash/engine" -I"$SRC/common" -I"$BUILD/common" \
    $(pkg-config --cflags Qt6Core glib-2.0 gobject-2.0) \
    main.cpp -o spike-engine \
    -L"$BUILD/lib" -lgnc-engine -lgnc-core-utils \
    $(pkg-config --libs Qt6Core glib-2.0 gobject-2.0) -ldl
```

The backend module itself has to be built first:
`cmake --build "$BUILD" --target gncmod-backend-xml`.

## Running it

```sh
LD_LIBRARY_PATH="$BUILD/lib:$BUILD/lib/gnucash" \
  ./spike-engine "$BUILD/lib/gnucash/libgncmod-backend-xml.so" \
  "$SRC/libgnucash/backend/xml/test/test-files/load-save/sample1.gnucash"
```

## Expected output

```
accounts under root: 104
```

Last observed with the debug build on Ubuntu 24.04, exit 0.

## Windows

Not attempted. The module is a `.dll` there and the loader call is
`LoadLibrary`/`GetProcAddress`, and the Windows toolchain (ADR 0007) has no
Guile or libdbi, so a full engine build is not available to test against yet.
