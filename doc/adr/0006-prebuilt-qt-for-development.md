# ADR 0006 — Prebuilt Qt for development; the vcpkg manifest stays the contract

- **Status**: Accepted (2026-11-06)
- **Supersedes**: part of [0003](0003-windows-dependencies-vcpkg.md) — the
  assumption that declaring a vcpkg dependency is a cheap way to obtain it
- **Related**: [0003](0003-windows-dependencies-vcpkg.md),
  [0005](0005-windows-ci-scope.md)

## Context

[ADR 0003](0003-windows-dependencies-vcpkg.md) chose vcpkg as the Windows
dependency manager and was validated with `--dry-run`. A dry-run resolves; it
does not build. Building it turned out to be the whole cost:

- `qtbase` alone is **1585 CMake build steps for the debug configuration**, and
  vcpkg builds debug and release for every port. `VCPKG_BUILD_TYPE` is a triplet
  setting, so the environment variable used to try to halve this had no effect.
- Reducing the dependency to `qtbase[core]` removed Gui, Widgets and Sql and cut
  the graph substantially, but the run then failed in a dependency: `libiconv`'s
  configure exited 77, and the retry hung on the same port for over ten minutes
  with no log growth.
- Elapsed before abandoning: roughly four hours, with no Qt installed.

Meanwhile prebuilt Qt is available in at least three forms: the Qt online
installer, `aqtinstall` (headless), and distribution packages — MSYS2 ships
`mingw-w64-x86_64-qt6-base` and `mingw-w64-x86_64-glib2`.

The mistake was not choosing vcpkg. It was treating "declared" as "obtained", and
therefore treating a source build as the only path to a working environment.

## Decision

1. **Development and spike work use prebuilt Qt.** On Windows that is currently
   MSYS2's `mingw-w64-x86_64-qt6-base` plus `mingw-w64-x86_64-glib2`; on Linux,
   the distribution packages.
2. **The vcpkg manifest remains the declared dependency contract** and continues
   to be kept honest by the CI job in
   [0005](0005-windows-ci-scope.md), but it is used with a binary cache. Building
   Qt from source per checkout is explicitly *not* a supported workflow.

## Consequences

- Contributors need a documented prebuilt path; the manifest alone will not
  produce a working tree on Windows in reasonable time.
- Binary caching becomes a requirement, not a nicety: `--binarysource` with a
  NuGet, GitHub Actions or object-store backend, or a vendored Qt build for
  release engineering.
- Two Qt sources now exist (prebuilt for development, vcpkg-declared for the
  contract). They must agree on the major version, or the manifest check and the
  development environment drift apart silently.
- Reinstating a source build is possible but must be justified by a need for a
  specific configuration, not by default.

## Validation

The spike was rebuilt and run against prebuilt libraries on both platforms:

| Platform | Qt | GLib | Result |
|---|---|---|---|
| Windows (MSYS2 MINGW64) | 6.9.1 | 2.84.2 | 47 ticks, 47 into Qt, 49 back, exit 0 |
| Linux (Ubuntu 24.04) | 6.4.2 | 2.80 | 44 ticks, 44 into Qt, 46 back, exit 0 |

The Windows run also caught a real defect that the Linux run had hidden:
`g_timeout_add()` attaches its source to the global default context rather than
the thread-default one, so the timeout never fired under Windows (0 ticks) while
`g_main_context_invoke()`, which names the context explicitly, kept working.
Attaching with `g_source_attach(source, context)` fixed it — see
[0002](0002-event-loop-integration.md).
