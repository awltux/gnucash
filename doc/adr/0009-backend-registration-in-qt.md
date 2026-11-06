# ADR 0009 — A Qt process registers a storage backend by loading its module

- **Status**: Accepted (2026-11-06)
- **Supersedes**: none
- **Related**: [0001](0001-qt6-presentation-tier.md),
  [0002](0002-event-loop-integration.md)

## Context

Phase 1 needs a Qt process to open a book. The engine's own tests do this by
having the backend linked into the test binary, which is not available to an
application: CMake builds `gncmod-backend-xml` as a `MODULE`, so there is no
library to link against in the ordinary way, and linking one backend in
permanently is the wrong shape for an app that also needs SQL.

Nothing in the code documents how a standalone process is supposed to make a
backend known. The answer had to be found by experiment.

## Options

1. **Link the module's objects into the Qt executable.** Works, but pins one
   backend at link time and duplicates what GnuCash's module system already
   does.
2. **`dlopen()` the module and call its init entry point.** This is what
   `gnc_module_load()` does underneath.
3. **Initialise `gnc-module` and call `gnc_module_load("gnucash/backend/xml")`.**
   The most faithful to GnuCash, but it brings in module search paths,
   `GNC_MODULE_PATH`, and the module dependency chain at startup.

## Decision

Option 2: `dlopen(module, RTLD_NOW | RTLD_GLOBAL)`, resolve
`qof_backend_module_init` (falling back to `gnc_module_init_backend_xml`), and
call it before opening a session.

## Rationale

- It is exactly what the module system does, minus the search-path machinery, so
  behaviour cannot diverge from GnuCash's own.
- It keeps backend choice out of link time, which matters because a book can be
  XML or SQL.
- Option 3 remains available later: if the Qt app needs the full module graph,
  this code is replaced by `gnc_module_system_init()` and
  `gnc_module_load()`, and the rest of the startup sequence does not change.

## Consequences

- The application must know where modules are installed, and must set
  `LD_LIBRARY_PATH` (or the platform equivalent) in development trees. This is
  the same requirement GnuCash already has.
- `dlerror()` output must be surfaced. A silent `dlopen` failure presents as
  "cannot open book", which is the least useful error in an accounting program.
- One backend is registered per call; registering two is harmless but pointless.
- `qof_init()` and `cashobjects_register()` must run first, before the module
  init, as they do in the engine's own tests.

## Validation

A 90-line spike (`doc/adr/spike-engine-qt/`) links `gnc-engine`, starts a
`QCoreApplication`, loads `libgncmod-backend-xml.so` by the mechanism above, and
opens `libgnucash/backend/xml/test/test-files/load-save/sample1.gnucash`:

```
accounts under root: 104
```

exit 0. That establishes three things at once: Qt and the engine coexist in one
process, the registration path works outside GnuCash, and a real book loads.

**Not verified:** the same spike on Windows, where the module is a `.dll` and
the loader call is `LoadLibrary` rather than `dlopen`.
