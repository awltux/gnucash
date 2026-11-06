# ADR 0008 — CI exercises the documented setup and the spike

- **Status**: Accepted (2026-11-06)
- **Supersedes**: [0005](0005-windows-ci-scope.md)
- **Related**: [0006](0006-prebuilt-qt-for-development.md),
  [0007](0007-windows-toolchain-mingw.md)

## Context

[0005](0005-windows-ci-scope.md) concluded that Windows CI could only verify
that the vcpkg manifest resolved, because stock MSYS2 cannot build GnuCash (no
mingw-w64 Guile, no libdbi). That was true and cheap, but it guarded the wrong
artefact: the manifest was an experiment, and the thing contributors actually do
— install MSYS2 packages and build against them — had no check at all.

[0007](0007-windows-toolchain-mingw.md) then made MSYS2 the supported toolchain,
which turned the gap into the main risk: a package disappearing from the
repository, or the documented versions drifting from reality, would be discovered
by a contributor rather than by CI.

## Decision

CI runs **`windows-msys2.yml`**, which:

1. installs exactly the packages `doc/dev-setup-windows.md` names;
2. asserts the Qt floor that document states (6.4);
3. builds and runs the event-loop spike.

The vcpkg manifest check (`windows-manifest.yml`) is retained as a **canary for
the experiment**, not as a gate: it is cheap, and a pinned baseline that silently
stops resolving is exactly the failure that stays invisible for months.

## Consequences

- A vanished package or a stale document now fails CI, which is the failure mode
  that matters.
- The job still does not build GnuCash, because it cannot: Guile and libdbi are
  not available as mingw-w64 packages. That gap closes only when the Windows
  build environment changes, and it is visible rather than assumed.
- Two Windows workflows exist for one purpose each. If the MSVC experiment is
  abandoned, `windows-manifest.yml` and `vcpkg.json` should be deleted together,
  and this record superseded again.

## Validation

Every command in the workflow was executed locally: the package set installs
from MSYS2, the floor check parses Qt 6.9.1 as major 6 / minor 9 and passes, and
the spike builds and exits 0.

**Not verified:** the workflow has not been observed running on GitHub.
