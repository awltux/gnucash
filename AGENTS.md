# AGENTS.md — GnuCash

Instructions for AI coding agents working in this repository. It is organised by
software-development-lifecycle stage, because the expensive mistakes in GnuCash
are process mistakes (forgotten dist lists, unmarked strings, silent data-format
changes), not syntax mistakes.

**Snapshot.** Verified against branch `stable` at tag 5.17 + 81 commits
(`0090906ddf`), roughly 30.6k commits. If `project (gnucash VERSION ...)` in
`CMakeLists.txt` no longer reads `5.17`, re-check the specifics below before
trusting them.

Human-facing docs: `README`, `README.git`, `README.dependencies`, `HACKING`,
`cmake/README_CMAKE.txt`, and <https://wiki.gnucash.org>.

---

## 0. Start here (TL;DR)

1. Work on a branch. `stable` is the default branch **and** the development line.
2. Build with presets: `cmake --preset linux-debug && cmake --build --preset linux-debug`.
3. Tests are not built by a plain `ninja`: use
   `cmake --build --preset linux-debug --target check`, or `testbuild` plus
   `ctest`.
4. Every added/moved file must also be registered in a `set_dist_list(...)`,
   or `ninja distcheck` (and therefore the release) breaks far from your change.
5. Never touch `po/*.po`, `borrowed/`, `NEWS`, the project version, or
   `GNUCASH_RESAVE_VERSION`.
6. User-visible strings must be wrapped for translation; bare literals block review.
7. Commit subject: `Bug NNNNNN - <summary>` for tracked fixes, otherwise
   `[area] <imperative summary>`.

---

## 1. What this project is

GnuCash is double-entry accounting software for personal and small-business use.

- **GUI**: GTK3 desktop application (`gnucash/`), with HTML report rendering.
- **Core**: `libgnucash/` — an accounting engine built on the QOF object
  framework, with XML and SQL persistence backends.
- **Extensibility**: Guile Scheme (report engine, scripting), Python bindings
  (optional), both generated with SWIG.
- **Licence**: GPL-2.0-or-later (`LICENSE`). Every source file carries the GPL
  header block; preserve it and only adjust `Copyright` lines you actually author.

Toolchain floors, from `CMakeLists.txt` and `README.dependencies`: C11
(`-D_GNU_SOURCE`), C++17 (`CMAKE_CXX_STANDARD 17`), CMake >= 3.14.5, GTK 3.22.30,
glib 2.68, Boost >= 1.67, ICU, libxml2 2.9.4, SWIG >= 3.0.12, googletest/gmock,
Guile 3.0 or 2.2 (2.0.9 still permitted), built with regex support.

**Dependency policy** (`README.dependencies`): depend only on library versions
that have been in the major distributions for six months. Do not add a library
to solve a local problem.

Platforms: GNU/Linux (x86_64, arm64), FreeBSD, OpenBSD, macOS, Windows via
MSYS2/MinGW. MSVC is explicitly out of scope (`cmake/README_CMAKE.txt`); do not
"fix" the build for it.

---

## 2. Map: where a change goes

| Layer | Path | Contains | Tests |
|---|---|---|---|
| Core engine | `libgnucash/engine/` | Account, Split, Transaction, Lot, Invoice, QOF (`qof*`), numerics, dates, options, KVP, features | `libgnucash/engine/test/` |
| Backends | `libgnucash/backend/{xml,sql,dbi}/` | XML v2 file format, libdbi triple store (SQLite3, MySQL, PostgreSQL), schema | `*/test/` |
| App services (no GUI) | `libgnucash/{app-utils,core-utils,tax,quotes,gnc-module}/` | preferences, expression parser, print helpers, locale tax tables, Finance::Quote wrapper | `*/test/` |
| GUI | `gnucash/{gnome,gnome-utils,gnome-search,register,html}/` | dialogs, assistants, plugins (`gnc-plugin-*.c`), register, HTML rendering | `gnucash/gnome/test/` |
| Reports | `gnucash/report/` | report engine (`report-core.scm`), the standard report set under `reports/standard/`, stylesheets, examples | `report/test/`, `reports/standard/test/` |
| Import/export | `gnucash/import-export/` | QIF, CSV, OFX, aqbanking, business import, log replay | `*/test/` |
| Bindings | `bindings/*.i`, `bindings/{guile,python}/` | SWIG interfaces and language wrappers | `bindings/guile/test/`, `bindings/python/tests/` |
| Build glue | `common/cmake_modules/`, `cmake/`, `CMakeLists.txt` | test registration, dist lists, SWIG commands, coverage, packaging | — |
| Translations | `po/` (~60 languages), `po/glossary/` | gettext catalogs, financial glossary | — |
| Vendored | `borrowed/` | chartjs, ctre, goffice, guile-json, libc, jenny | — |
| Misc | `data/`, `doc/`, `util/ci/`, `contrib/`, `test-templates/` | account templates, manpages, CI actions, artwork, legacy test templates | — |

Reading order for an unfamiliar change: the nearest `CMakeLists.txt`, then the
module's public header, then an existing test in the sibling `test/` directory,
then the Guile counterpart if the feature is scriptable.

---

## 3. Branch, integration and release model

- `stable` is both the default branch (`origin/HEAD -> origin/stable`) and the
  active development line. There is no `develop` or `maint` branch for the 5.x
  series.
- Releases are cut **quarterly** (March, June, September, December) from
  `stable`, tagged `5.N`, with a `Release GnuCash 5.N` commit. See
  `git log --grep='^Release GnuCash' --oneline`.
- Contributors work on their own fork and branch. Maintainers integrate with a
  merge commit titled `Merge <Name>'s '<branch>' into stable.`; translation
  batches arrive as `Merge latest translations from Weblate.` Roughly one commit
  in eight is such a merge.
- `4` and `2.4` carry previous major series and only see critical fixes;
  `future` holds work aimed at the next major series. Do not base work on them
  and do not modify them.
- CI (`.github/workflows/`): `ci-tests` (Ubuntu 22.04 with `ninja distcheck`, plus
  an AddressSanitizer job), `mac-tests` (macOS with a prebuilt dependency
  tarball), `ci-docker` (Arch Linux container), `coverage` (Debug + `-DCOVERAGE=ON`,
  lcov, Doxygen, GitHub Pages). The test jobs run on push and pull request;
  `coverage` runs on push only.

Consequences:

- Branch your work; never commit directly to `stable`, never force-push or
  rewrite published history.
- Every commit on `stable` must build and pass tests. `README.git` treats a
  broken build as the cardinal sin.
- A change is not done until it would survive `ninja distcheck` (§7).

---

## 4. Lifecycle stages

### Stage 0 — Intake: what is the change attached to?

- User-visible defects and RFEs are tracked in **Bugzilla** at
  <https://bugs.gnucash.org> (`PACKAGE_BUGREPORT` in `CMakeLists.txt`); open bug
  numbers are in the `799xxx` range. Fix commits are titled
  `Bug 799834 - <summary>`.
- Design discussion happens on the `gnucash-devel` mailing list; pull requests on
  `Gnucash/gnucash` are the integration path.
- Classify the change first, because the class dictates the workflow:

| Class | Extra obligations |
|---|---|
| Cosmetic or string fix | mark strings correctly; never edit catalogs |
| Bug fix in one module | add or extend a test in the sibling `test/`; `Bug NNNNNN - ...` subject |
| New user-visible feature | preference via gschema, translation markup, test where feasible, manpage if CLI changes |
| Stored-data or format change | §5 applies; highest-risk category |
| Build or dependency change | CI green on Linux, macOS, Arch; dependency policy in §1 |
| Scheme report or API change | Guile tests under `gnucash/report/test/`; check binding load tests |

### Stage 1 — Design: decide before editing

Stop and consult a human when the change would:

- alter stored data semantics, the XML schema, or the SQL schema (§5);
- add or bump a dependency, or change a toolchain floor;
- change a public C header that SWIG wraps (`bindings/*.i`,
  `bindings/python/CMakeLists.txt` include lists);
- add a build option, a loadable module, or an install destination;
- change preference or gsettings keys users already have stored;
- touch `borrowed/` (vendored third-party code: patch upstream, not here).

Small, local, behaviour-preserving changes need no gate.

### Stage 2 — Implement

Authoritative style is the wiki
(<https://wiki.gnucash.org/wiki/CodingStandard>); `HACKING` only points there.
There is no `.clang-format`, no `.editorconfig`, and no formatter config, so
"run the formatter" is not available. Match the file you are editing.

Observable conventions, verified in-tree:

- **Indentation**: four spaces in `.c/.cpp/.h/.hpp` (tabs survive only in old
  code). Keep new lines near 80 columns; long string literals are the standard
  exception.
- **Naming**: functions `gnc_foo_bar()`; C typedefs `gnc_foo` (struct tag
  `_gnc_foo`) or PascalCase object types such as `Account` and `Split`; C++
  classes `GncFoo`; macros and enums `GNC_FOO`; Scheme `gnc:foo-bar`; files
  `gnc-foo.c` with `gnc-foo.h`. QOF core types keep the `qof` / `Qof` prefix.
  `xacc*` names are legacy: do not introduce new ones.
- **Defensive exits**: `g_return_if_fail()` / `g_return_val_if_fail()` are used
  in roughly 250 source files; prefer them at public API boundaries over raw
  asserts.
- **Logging**: set `-DG_LOG_DOMAIN="gnc.<area>"` per target (for example
  `gnc.report.core`, `gnc.import.aqbanking`) and log through `DEBUG`, `PWARN`,
  `g_message`. No `printf` debugging.
- **Documentation**: Doxygen blocks (`@file`, `@brief`, `@param`,
  `@addtogroup`) on public headers are mandatory; they feed
  <https://code.gnucash.org/docs/MASTER/>. Private headers use the `-p.h` /
  `P.hpp` suffix.
- **C/C++ interop**: keep C headers valid C (`#ifdef __cplusplus extern "C" {`).
  The engine is mid-migration from C to C++, so neighbours may be either
  language; do not rewrite them.
- **GUI**: GTK3 plus `.glade` UI files in `gnucash/gtkbuilder/`. Menus and actions
  arrive through `gnc-plugin-*.c` registering pages; do not edit the main window
  to hook a feature in.
- **New report**: a file under `gnucash/report/reports/standard/` whose
  `(define-module (gnucash reports standard <name>))` matches its installed
  path, a `(gnc:define-report ...)` block, registration in
  `gnucash/report/reports/CMakeLists.txt`, and `test/test-<name>.scm`.
- **Line endings**: LF for sources (enforced by `.gitattributes`); CRLF only for
  `.bat`, `.cmd`, `.isl`, `.vbs`. No trailing whitespace.

### Stage 3 — Build

Use the presets in `CMakePresets.json`:

```sh
cmake --preset linux-debug
cmake --build --preset linux-debug
```

They require CMake 3.21 or newer; the project itself still configures with
3.14.5 through the manual form below. Available presets: `linux-debug`,
`linux-release`, `linux-asan`, `linux-coverage` (mirrors the coverage workflow),
`linux-core` (`WITH_GNUCASH=OFF`, no GTK or WebKit: the fastest loop for engine
and backend work), `macos-debug`, and `windows-msys2-debug` (run it from an
MSYS2 MINGW64 shell). Build trees land in `.build/<preset>`, which is gitignored
and hidden so gettext does not scan it; install prefixes stay under
`.build/inst/<preset>`, so nothing writes to `/usr/local`.

Without presets, or to build outside the source tree as `HACKING` prefers:

```sh
cmake -G Ninja -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_INSTALL_PREFIX=$HOME/gnc/inst \
      -S . -B ../gnucash-build
ninja -C ../gnucash-build
```

Options (`CMakeLists.txt`): `WITH_PYTHON` (OFF by default; CI and `distcheck`
turn it ON), `WITH_SQL`, `WITH_AQBANKING`, `WITH_OFX`, `WITH_GNUCASH` (OFF
builds libraries only), `DISABLE_NLS`, `COVERAGE`, `GUILE_COVERAGE`, `LEAKS`,
`ODR`. Build types: `Debug`, `Release`, `Asan`.

Targets: `check`, `testbuild`, `dist`, `distcheck`, `doc` (Doxygen), `uninstall`,
`ChangeLog`; with `-DCOVERAGE=ON` also `lcov-initialize`, `lcov-collect`,
`lcov-generate-html`.

If googletest is not installed system-wide, point `GTEST_ROOT` at its source
tree; a missing gtest or gmock is a hard configure error.

Windows note: this checkout may live on Windows, but a native build needs the
MSYS2/MinGW toolchain plus the full GTK stack. Do not infer "the build passes"
from a Windows-only attempt; configure and build in WSL or Linux, or state
plainly that the build was not run.

### Stage 4 — Test

Compiled test executables are `EXCLUDE_FROM_ALL`, so `ninja` alone does not
build them:

```sh
cmake --build --preset linux-debug --target check   # builds every test, then runs ctest
ctest --preset linux-debug                          # re-run without rebuilding
ctest --test-dir .build/linux-debug -R test-gnc-numeric --output-on-failure
```

- Roughly 100 ctest entries across 17 `test/` directories, using three harnesses:
  - **googletest/gmock**: the default for new tests (`gtest-*.cpp`, for example
    `libgnucash/engine/test/gtest-gnc-numeric.cpp`).
  - **GLib g_test**: legacy C tests (`test-*.c`), helpers in
    `common/test-core/test-stuff.h` and `unittest-support.h`.
  - **Guile Scheme**: `test-*.scm` exporting `(define (run-test) ...)`, registered
    through the `gnc_add_scheme_test*` macros.
  - **Python bindings**: one ctest entry named `python-bindings`, only when
    `WITH_PYTHON=ON`.
- Registration macros live in `common/cmake_modules/GncAddTest.cmake`:
  `gnc_add_test(<name> <sources> <INCLUDE_VAR> <LIBS_VAR> [ENV=...])`,
  `gnc_add_test_with_guile(...)`, and the Scheme variants. They inject
  `GNC_UNINSTALLED=YES` and `GNC_BUILDDIR` automatically.
- Tests must be hermetic. CI pins `TZ=America/Los_Angeles`, but do not depend on
  the ambient locale, timezone, wall clock, network, or filesystem order; pass
  locale and timezone explicitly. Never `sleep()` to synchronise.
- A test that only passes when run alone, or that writes into the source tree,
  will fail `distcheck`.

### Stage 5 — Internationalise and document

- Wrap user-visible strings: `_("...")` at the point of use, `N_("...")` in
  static initialisers, `G_("...")` where gettext must not yet see the text; in
  Scheme `(_ "...")` and `(N_ "...")`. Bare UI literals are a review blocker.
- **Never edit `po/*.po` or `po/glossary/*.po`.** Translation happens on
  Weblate; batches land as `Merge latest translations from Weblate.`
- `po/POTFILES.in` is generated into the source tree by the CMake configure step
  and is gitignored. Do not hand-write it; expose new translatable files through
  the module's CMake lists, and use `POTFILES.skip` / `POTFILES.ignore` to
  exclude files deliberately.
- `po/glossary/gnc-glossary.txt` defines terminology; reuse its wording.
- Documentation: Doxygen for API; manpage templates `doc/gnucash.1.in` and
  `doc/gnucash-cli.1.in`; end-user manuals live in the separate `gnucash-docs`
  repository, not here. `NEWS` is written by maintainers at release time; do not
  add entries unless asked.

### Stage 6 — Review and commit

Commit subjects on `stable` follow a small set of shapes:

| Shape | Example | When |
|---|---|---|
| `Bug NNNNNN - <summary>` | `Bug 799834 - In a report page, "Back" button no longer returns to the previous report.` | fix for a tracked bug |
| `[area] <imperative summary>` | `[bindings/python] Floor GncNumeric // and make int() exact` | targeted change (about one in eight commits) |
| `Merge <Name>'s '<branch>' into stable.` | | maintainer integration only |
| `Release GnuCash 5.N`, `Translation update ...` | | release and Weblate automation only |

- One logical change per commit. The body explains *why* and references the bug;
  this project's history carries no `Signed-off-by` trailer.
- Never mix formatting churn with behaviour change: it defeats review and
  backporting.
- What reviewers check, and therefore what you must satisfy: a test exists or the
  absence is justified; strings are translatable; new files are in a dist list;
  data-format impact is considered; nothing unrelated is touched; history is
  bisectable.

### Stage 7 — Release (maintainers only; agents must not perform it)

The sequence visible in history: bump `project (gnucash VERSION ...)` in
`CMakeLists.txt`; write the `NEWS` section (bugfixes citing `Bug NNNNNN`, then
"fixes not associated with bug reports"); commit `Release GnuCash 5.N`; tag
`5.N`; publish the tarball produced by `ninja dist` and validated by
`ninja distcheck`.

`distcheck` extracts the tarball, configures it with `-DWITH_PYTHON=YES`, builds,
installs, runs `check`, then builds `dist` again. That is why every source file
must appear in `set_dist_list(...)`, or in `dist_add_generated` /
`dist_add_configured` for generated files, in its directory's `CMakeLists.txt`.
**A file added without a dist-list entry creates a release-time failure that no
local `ninja check` reveals.**

### Stage 8 — Maintenance

- Fixes land on `stable` first. There is no 5.x maintenance branch; the `4`
  branch is effectively archival.
- Bug numbers in commit subjects are permanent references; copy them exactly.

---

## 5. Stored-data safety (read before touching engine or backend)

Three things persist real users' data and are effectively permanent: the **XML
file format**, the **SQL triple store**, and **KVP slots** attached to objects.

- XML: `GnuCash File Backend Version 2`
  (`libgnucash/backend/xml/gnc-backend-xml.cpp`), SAX-based `sixtp` handlers.
  Every object type has a parser/generator pair that must stay in sync.
- SQL: libdbi-backed, one `<foo>-sql.cpp` per object, table versioning through
  the `Gnucash` and `Gnucash-Resave` rows
  (`libgnucash/backend/sql/gnc-sql-backend.cpp`).
- `GNUCASH_RESAVE_VERSION` (`CMakeLists.txt`, currently `19920`) is compiled into
  the build and compared with those rows: newer code than database offers a
  resave, older code than database opens **read-only**. Bumping it makes every
  earlier release refuse to write the book. Treat it as maintainer territory.
- The sanctioned, narrower mechanism is the features table: when you begin
  *writing* data an older release cannot interpret, call
  `gnc_features_set_used(book, GNC_FEATURE_...)` and add the string in
  `libgnucash/engine/gnc-features.h`. Older versions then refuse to open the book
  instead of corrupting it.
- New KVP slots, new gsettings keys, and new XML attributes are
  backward-compatible by default; changing the meaning of an existing field is
  not.
- Any change here needs a round-trip test (save, load, compare) in
  `libgnucash/backend/xml/test/` or `libgnucash/backend/sql/test/`, plus an
  explicit note in the change description.

---

## 6. Recipes

**Add a source file to a module**

1. Place it beside its siblings, named per §2, with the GPL header block copied
   from a neighbour.
2. Add it to `set(<module>_SOURCES ...)`, or `*_HEADERS` / `*_noinst_HEADERS`, in
   that directory's `CMakeLists.txt`.
3. Add it to `set_dist_list(<module>_DIST ...)`.
4. If the API is installed or wrapped, update the SWIG side
   (`bindings/*.i`, `bindings/python/CMakeLists.txt` `GNUCASH_CORE_C_INCLUDES`).

**Add a C++ unit test**

1. `gtest-<thing>.cpp` in the module's `test/` directory.
2. In `test/CMakeLists.txt`, declare or reuse the include and library variable
   lists, then call
   `gnc_add_test(test-<name> gtest-<thing>.cpp INCLUDE_VAR LIBS_VAR)`; the libs
   list must contain `gtest`.
3. Add the file to that directory's `set_dist_list(...)`.

**Add a report**

1. `gnucash/report/reports/standard/<name>.scm` with a module declaration
   matching its installed path and a `(gnc:define-report ...)`.
2. Register the file in `gnucash/report/reports/CMakeLists.txt`
   (`reports_standard_SCHEME`).
3. Add `test/test-<name>.scm` and its entry in
   `gnucash/report/reports/standard/test/CMakeLists.txt`.

**Add a preference**

1. New key in the appropriate
   `gnucash/gschemas/org.gnucash.GnuCash.*.gschema.xml.in`.
2. Read it through the `gnc-gsettings` / option machinery, not ad-hoc
   `g_settings` calls, so the option database and the test harness stay coherent.

---

## 7. Definition of done

Run what you can and report honestly which steps you ran and which you could not:

```sh
cmake --preset linux-debug
cmake --build --preset linux-debug
cmake --build --preset linux-debug --target check
ninja -C .build/linux-debug distcheck   # required if you added, moved or renamed a file
```

- [ ] Build is clean; no new `-Werror` warnings (Unix builds use
      `-Werror -Wall -Wmissing-declarations`, C also `-Wmissing-prototypes`).
- [ ] New or changed behaviour is covered by a test in the sibling `test/`.
- [ ] `set_dist_list` / `dist_add_generated` updated for every added or generated
      file.
- [ ] User-visible strings wrapped; no `.po` edits.
- [ ] Public headers documented; `.i` files updated if the C API changed.
- [ ] Stored-data changes: features table and `GNUCASH_RESAVE_VERSION` considered,
      round-trip test added.
- [ ] Commit subject matches a shape in Stage 6; body explains why; no unrelated
      churn.
- [ ] `git status` shows only intended files: no build directories, no
      `ChangeLog`, no `po/POTFILES.in`, no `*.pot`.

---

## 8. Guardrails — do not

- Do not commit to `stable`, force-push, rewrite history, or create tags.
- Do not bump `project (gnucash VERSION ...)` or `GNUCASH_RESAVE_VERSION`, and do
  not write `NEWS` entries.
- Do not edit `po/*.po`, `po/POTFILES.in`, `ChangeLog*`, `AUTHORS`,
  `DOCUMENTERS`, or anything under `borrowed/` without explicit instruction.
- Do not modernise unrequested code: no GTK4 idioms, no `std::`-ification of C
  files, no formatter sweeps, no copyright-header rewrites.
- Do not reformat or renumber a whole file to fix one line.
- Do not delete `#ifdef` platform branches because they look dead on your
  platform.
- Do not add dependencies, build options, or install destinations without human
  approval.
- Do not disable, skip, or delete failing tests to make CI green, and do not add
  `continue-on-error` to workflows.
- Do not run CMake inside the source tree or commit generated artifacts.
- Do not report a build or test result you did not observe.

---

## 9. Known traps and stale docs

- `HACKING` is thin and defers to the wiki. `cmake/README_CMAKE.txt` still says
  CMake 3.0 although `CMakeLists.txt` requires 3.14.5. `test-templates/` is
  autotools-era (it mentions `Makefile.am` and `configure.ac`): ignore it and
  copy a modern `gtest-*.cpp` instead.
- There is no `CONTRIBUTING.md` and no formatter config; process lives in
  `README*`, the wiki, and commit convention.
- Guile modules are found by installed path. A Scheme file in the wrong directory
  registers as a different module name, and tests run with `GNC_UNINSTALLED=YES`
  plus the `GUILE_LOAD_PATH` that CMake sets.
- CMake policies are pinned deliberately, for example `CMP0167 OLD` keeps the
  legacy `FindBoost` for the 5.x series; changing them breaks older
  distributions.
- The version string is derived from `git describe` at configure time
  (`cmake/git2version-info.cmake`, `util/gnc-vcs-info`). A shallow clone changes
  the reported version, not the code.
- `po/POTFILES.in` and `ChangeLog` appearing as untracked files after a configure
  or build run is normal; both are generated and gitignored.
- Windows hygiene: `.gitattributes` forces LF for source files. If your editor
  writes CRLF, `git diff` will show whole-file changes; check
  `git config core.autocrlf` before blaming the file.
