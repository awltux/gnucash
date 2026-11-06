# Event-loop spike

Standalone program behind the bridge decision in
[ADR 0002](../0002-event-loop-integration.md) and the results in
[ADR 0006](../0006-prebuilt-qt-for-development.md). It is not part of the build;
it exists so the claim can be re-run instead of trusted.

It proves both directions:

- **GLib to Qt.** A timeout on a dedicated `GMainContext`, pumped by
  `g_main_context_iteration(context, TRUE)` on its own thread, marshals each
  tick into the Qt thread with `QMetaObject::invokeMethod(..., Qt::QueuedConnection)`.
  The functor overload needs no `moc` pass.
- **Qt to GLib.** A `QTimer` in the Qt thread calls
  `g_main_context_invoke(context, ...)`.

## The trap it documents

`g_timeout_add()` attaches its source to the **global default** context, not the
thread-default one. A first version used it and behaved correctly on Linux while
delivering **zero** ticks on Windows, because nothing iterates the global context
here. Only the Qt-to-GLib direction kept working, since `g_main_context_invoke()`
names its context explicitly.

The program therefore attaches explicitly:

```cpp
GSource* tick = g_timeout_source_new(10);
g_source_set_callback(tick, glib_tick, &counters, nullptr);
g_source_attach(tick, context);
g_source_unref(tick);
```

Any Qt front end that owns a private `GMainContext` has to do the same.

## Running it

Linux, with distribution Qt:

```sh
g++ -std=c++17 -O1 main.cpp -o spike $(pkg-config --cflags --libs Qt6Core glib-2.0)
./spike
```

Windows, from an MSYS2 MINGW64 shell, with
`mingw-w64-x86_64-qt6-base` and `mingw-w64-x86_64-glib2` installed:

```sh
g++ -std=c++17 -O1 main.cpp -o spike.exe $(pkg-config --cflags --libs Qt6Core glib-2.0)
./spike.exe
```

## Expected output

```
glib ticks:      47
glib -> qt:      47
qt -> glib:      49
BRIDGE OK
```

Counts vary with scheduling; the exit status is 0 only when every counter
reached at least 5. Last observed: Qt 6.9.1 / GLib 2.84.2 on Windows
(47/47/49) and Qt 6.4.2 / GLib 2.80 on Linux (44/44/46).
