# ADR 0007 — Windows toolchain is MinGW-w64 through MSYS2

- **Status**: Accepted (2026-11-06)
- **Supersedes**: [0003](0003-windows-dependencies-vcpkg.md) as a dependency
  contract, and the "the manifest stays the contract" part of
  [0006](0006-prebuilt-qt-for-development.md)
- **Related**: [0001](0001-qt6-presentation-tier.md),
  [0005](0005-windows-ci-scope.md)

## Context

[0006](0006-prebuilt-qt-for-development.md) settled *how* to obtain Qt — prebuilt,
not compiled — but left *which toolchain* implicit. That choice is not
cosmetic, because a Qt built with MinGW cannot link against MSVC-compiled
objects. The three candidates disagreed:

| Source | Qt | Toolchain |
|---|---|---|
| Ubuntu 24.04 packages | 6.4.2 | GCC |
| MSYS2 packages | 6.9.1 | MinGW-w64 |
| vcpkg manifest as written | 6.11.3 | MSVC (`x64-windows`) |

Two facts then decide it:

1. **GnuCash's existing Windows build is MinGW.** `gnucash-on-windows` is a
   MinGW/jhbuild environment, and the dependencies it builds from patched source
   — Guile 2.2, libdbi — are MinGW builds. There is no MSVC GnuCash on Windows
   today.
2. **MSVC would mean rebuilding that stack.** Choosing MSVC makes Qt easy and
   everything else harder: the patched Guile and libdbi, and any dependency that
   only exists as a MinGW package, would need MSVC equivalents.

The vcpkg manifest was written against the MSVC assumption. That assumption came
from ADR 0001's remark that Qt "makes MSVC a supported compiler", which was
speculation and has not been tested.

## Decision

**The supported Windows toolchain is MinGW-w64 through MSYS2.** The Qt tier takes
Qt and GLib from MSYS2 packages, installed rather than built. The floor is Qt 6.4,
matching current LTS distributions.

`vcpkg.json` is **not** the Windows dependency contract. It is retained as a
documented experiment for a possible future MSVC path, and its CI check is kept
only so the experiment cannot rot silently.

## Consequences

- One Windows toolchain, matching the one the project already ships.
- MSYS2 package versions become the floor for Windows Qt work; see
  [dev-setup-windows.md](../dev-setup-windows.md).
- MSVC stays out of scope, consistent with `cmake/README_CMAKE.txt`. ADR 0001's
  suggestion that Qt would change that is not being pursued.
- The vcpkg manifest no longer gates anything material. Anyone reading it should
  treat it as an experiment, not a requirement.
- A contributor who installs Qt with MSVC and then compiles with MinGW — or the
  reverse — gets unresolved symbols at link time. The setup document calls this
  out because the failure mode is opaque.

## Validation

The documented setup was executed on 2026-11-06 with
`mingw-w64-x86_64-qt6-base` 6.9.1 and `mingw-w64-x86_64-glib2` 2.84.2. The
event-loop spike built with `g++` and `pkg-config` and ran successfully: 47 GLib
ticks, 47 delivered into Qt, 49 back, exit 0.
