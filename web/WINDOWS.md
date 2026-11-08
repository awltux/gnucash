# gnc-webd on Windows — status

`gnc-webd` is the one part of this branch that is genuinely platform-independent:
no GTK, no WebKit, just the engine, the report engine, Guile and GIO. On Linux it
builds and serves. On Windows it does not build yet, and the reason is not the
web tier.

## The blocker: there is no usable Guile for MSYS2 mingw64

GnuCash's `CMakeLists.txt` makes Guile a hard requirement (`pkg_check_modules
GUILE3 guile-3.0 …` / `FATAL_ERROR "Neither guile 3.0, guile 2.2, nor guile 2.0
were found"`), so configure cannot even start without it.

What was tried, in order:

| route | result |
|---|---|
| `pacman` packages | **no mingw64 Guile exists.** `pacman -Ss guile` returns only `msys/guile`, `msys/libguile`, `msys/libguile-devel` — Cygwin ABI, unlinkable into a mingw64 binary. `guile-json` needs no package: it is vendored in `borrowed/` |
| Guile 3.0.11 from source | builds and links after three fixups (64-bit calls, gnulib `rpl_timeval`, `ffs`), but aborts at init: its psyntax bootstrap needs prebuilt `.go` files that upstream ships only for Linux targets |
| Guile 2.2.7.3 + `gnucash-on-windows` patches | `guile.exe` runs Scheme correctly (`(+ 1 2)`, 200k-cons loop, GC stress). Fails deterministically at `bootstrap/ice-9/psyntax-pp.go`: `bytevector-u32-native-set!: Wrong type argument in position 3: 555227496` — a pointer reaching the objcode writer while it serialises the largest Scheme source. Identical with `-O2`, `-O0`, a 32 MB stack |
| upstream bdw-gc 8.2.8 (`--enable-threads=no --disable-parallel-mark`), libguile confirmed relinked | **same failure, bit-identical value.** Not an allocator problem |

Two things that are *not* in the upstream patch set but are required with a
current mingw-w64: `libguile/posix-w32.h` must declare `pid_t start_child(…)`
(mingw-w64's `pid_t` is `long long` now), and `HAVE_GETHOSTNAME` must be
undefined (gnulib poisons the name on Windows and gcc defaults to C23 where an
implicit declaration is an error).

The Guile 2.2.7.3 tarball is no longer on ftp.gnu.org; the one that project
builds is `guile-2.2.7.3-5ef31.tar.xz` from the GnuCash SourceForge dependencies.

## What would unblock it

1. **The toolchain GnuCash itself ships.** Its Windows build uses a 32-bit
   mingw32 environment and its bundle contains 411 compiled `.go` files
   including `ice-9`. That is a bespoke jhbuild setup (`gnucash-on-windows`),
   not a `pacman` install, and it is days of work.
2. **A fix in Guile 2.2's objcode writer** — `module/system/vm/assembler.scm`,
   the `process-relocs` / `pack-u8-s24` path that is handed a pointer. A real
   upstream bug, not a build misconfiguration.

Neither is a property of `gnc-webd`.

## Repository changes already made

- `web/server/main.cpp`: `#include <dlfcn.h>` removed. No such header on mingw64,
  and nothing in the server uses `dlopen`/`dlsym` since backends are found
  through `GNC_MODULE_PATH`.
- `web/CMakeLists.txt`: `target_link_options(gnc-webd PRIVATE -mconsole)` under
  `WIN32`. GnuCash sets `-mwindows` in `CMAKE_EXE_LINKER_FLAGS`, which would link
  the server as a GUI-subsystem program: its `webd listening on http://…` line
  would never reach stdout, and that line is how a user and the tests find it.

Neither could be verified by a Windows build, because configure is never
reached. Both are wrong independently of that.

## Known, unverified, waiting on a build

- `CMakeLists.txt:133` detects MSYS2 with `string(FIND ${CMAKE_C_COMPILER}
  "msys2" …)`; a stock install is `C:/msys64`, so `MINGW64` stays false, `SHELL`
  becomes `${CMAKE_PREFIX_PATH}/mingw/msys/1.0/bin/bash.exe` (nonexistent) and
  `GncAddSchemeTargets.cmake` uses `create_symlink` instead of `copy`.
- On MINGW64 with Guile ≥ 2.2, configure aborts unless `GUILE_LOAD_PATH` and
  `GUILE_LOAD_COMPILED_PATH` are exported.
- `CMakeLists.txt:647` adds `-Werror` for MINGW; gcc 16 will promote new
  warnings to errors.
