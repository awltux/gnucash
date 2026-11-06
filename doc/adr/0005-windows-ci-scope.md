# ADR 0005 — Windows CI verifies the manifest, not the build

- **Status**: Accepted (2026-11-06), expected to be superseded once the Qt tier
  builds on Windows
- **Supersedes**: none
- **Related**: [0003](0003-windows-dependencies-vcpkg.md)

## Context

The project has no Windows CI job. Adding one requires deciding what it can
actually do, and the answer is constrained by the dependency situation.

Checked against MSYS2's real package database rather than assumed: there is **no
mingw-w64 Guile** — the only Guile packages are MSYS-runtime, which cannot be
linked into a mingw-w64 binary — and **no libdbi at all**. Both are mandatory for
a GnuCash build, and both are exactly why `gnucash-on-windows` builds them from
patched source with jhbuild.

## Options

1. **Full build in CI via jhbuild.** Reproduces the official environment.
2. **Build from MSYS2 packages.** Impossible for the reason above.
3. **Verify the dependency manifest and the presets only.**
4. **Build the Qt tier with vcpkg and MSVC.** Needs Qt code that does not exist
   yet.

## Decision

Option 3 until the Qt tier can be built; Option 4 becomes the natural successor.

## Rationale

- A workflow that installs packages and then fails at configure is worse than no
  workflow: it trains people to ignore red builds.
- Option 1 is hours per run and inherits every fragility of the jhbuild
  environment; it is how installers get made, not how pull requests get checked.
- The manifest is the artefact that rots silently. A pinned baseline that no
  longer resolves breaks the Windows story without breaking any other build, so
  it is worth a cheap check on every push.

## Consequences

- The job clones vcpkg, fetches the pinned `builtin-baseline` commit explicitly
  (a depth-1 clone does not contain an older commit), resolves the manifest for
  `x64-windows`, asserts that `qtbase` and `qtwebengine` appear in the plan, and
  checks the Windows configure preset still exists.
- It does **not** compile GnuCash on Windows, so it cannot catch a Windows build
  break. That gap closes in Phase 1 or later.
- When the Qt tier is buildable, this ADR should be superseded by one that
  replaces it with a real MSVC build.

## Validation

The same vcpkg version and command were run locally on Windows and resolved
`qtbase` 6.11.3 and `qtwebengine` 6.11.3. The workflow parses as YAML with one
job and five steps. The GitHub-hosted run itself has not been observed.
