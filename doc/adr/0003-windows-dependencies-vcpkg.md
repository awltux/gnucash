# ADR 0003 — Windows dependencies come from a vcpkg manifest

- **Status**: Accepted (2026-11-06)
- **Supersedes**: none
- **Related**: [0001](0001-qt6-presentation-tier.md),
  [0005](0005-windows-ci-scope.md)

## Context

Windows builds currently depend on the out-of-tree `gnucash-on-windows`
environment: jhbuild compiles Guile 2.2, libdbi and a patched WebKit from source
over several hours. That works for producing installers but is hostile to
contributors and impossible to reproduce casually.

Qt cannot be built that way. It needs a package manager, and any Windows path for
the Qt tier has to be reproducible from a checkout.

## Options

1. **Keep jhbuild.** Extend the moduleset with Qt and keep building everything
   from source.
2. **MSYS2 packages only.** Use `pacman` for everything.
3. **vcpkg manifest.** Declare the Windows dependencies in `vcpkg.json`, pinned
   to a baseline commit.
4. **Conan 2.** Same idea as vcpkg with a different dependency graph.

## Decision

Option 3, with two constraints:

- the manifest is **scoped to the Windows triplet**; Linux and macOS take Qt
  from the distribution or Homebrew;
- the manifest lists **ports only**, never feature lists, because the ports
  already carry their own platform-conditional defaults.

## Rationale

- vcpkg resolves Qt reliably on Windows, which is the one platform where a
  hand-built dependency stack is unacceptable.
- Scoping to Windows avoids a second source of Qt on Linux, where distributions
  already package it and CI already relies on that.
- Omitting features is not laziness: `qtbase`'s default features already include
  widgets, `sql`, `sql-sqlite`, `sql-psql` and `testlib`. Re-listing them would
  duplicate upstream policy and drift.

## Consequences

- Pinning `builtin-baseline` means the pinned commit must exist locally. A
  shallow clone does not contain it, so CI fetches that commit explicitly.
- `qtwebengine` is declared but deliberately not part of the first build: it is
  large, slow to compile, and only needed once the report pane exists.
- A manifest that nobody resolves rots. See
  [0005](0005-windows-ci-scope.md) for the check that keeps it honest.

## Validation

Checked against a real vcpkg clone (2026-10-29) rather than assumed: the port
manifests were read to confirm feature names, the baseline is the clone's
commit, and `vcpkg install --dry-run --triplet x64-windows` on Windows resolves
to `qtbase[...widgets,sql,sql-sqlite,testlib,windeployqt]:x64-windows@6.11.3`
and `qtwebengine[pdf,webengine]:x64-windows@6.11.3`. The same command with
`x64-linux` requests no packages, confirming the platform gate.

**Not verified:** installing, which builds Qt from source and has not been
completed here.
