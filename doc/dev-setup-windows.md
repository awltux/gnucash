# Windows development setup for the Qt 6 tier

Windows development uses **MinGW-w64 through MSYS2**. This is the same toolchain
the existing GnuCash Windows build uses, so the project keeps one Windows
toolchain instead of two. See
[ADR 0007](adr/0007-windows-toolchain-mingw.md) for why.

Qt and GLib are **installed, not built**. Building Qt from source takes hours and
is not a workflow this project supports.

## Install

From an MSYS2 **MINGW64** shell:

```sh
pacman -S --needed \
    mingw-w64-x86_64-gcc \
    mingw-w64-x86_64-pkgconf \
    mingw-w64-x86_64-qt6-base \
    mingw-w64-x86_64-glib2
```

Versions verified on 2026-11-06:

| Package | Version |
|---|---|
| `mingw-w64-x86_64-qt6-base` | 6.9.1 |
| `mingw-w64-x86_64-glib2` | 2.84.2 |

The project's floor is **Qt 6.4**, because that is what current LTS
distributions ship (Ubuntu 24.04 has 6.4.2). Newer is fine; do not raise the
floor without checking distributions first.

## Smoke test

The event-loop spike is the cheapest end-to-end check that Qt, GLib and the
compiler all work together:

```sh
cd doc/adr/spike-event-loop
g++ -std=c++17 -O1 main.cpp -o spike.exe $(pkg-config --cflags --libs Qt6Core glib-2.0)
./spike.exe
```

It should print three counters and `BRIDGE OK`, and exit 0.

## What this does not give you

- **A GnuCash build.** MSYS2 has no mingw-w64 Guile and no libdbi, so a full
  Windows GnuCash build still needs the out-of-tree `gnucash-on-windows`
  environment. Qt tier work happens against the engine through the normal
  jhbuild shell once that environment exists.
- **MSVC.** A MinGW-built Qt cannot link against MSVC objects, so MSVC stays out
  of scope, as `cmake/README_CMAKE.txt` has said all along.

## Mixing happens silently, so don't

If `pkg-config` resolves Qt from an MSYS2 installation while the compiler is
MSVC, the failure appears at link time as a wall of unresolved symbols. If you
see that, check which compiler is first on `PATH` before chasing anything else.
