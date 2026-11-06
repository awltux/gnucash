# Architecture decision records

One file per decision, numbered, immutable once accepted.

## Conventions

- **An ADR records a decision, not progress.** Phases, status, blockers and
  effort live in [the migration plan](../qt6-migration-plan.md). If you find
  yourself updating an ADR every week, it is a plan wearing an ADR's clothes.
- **Do not edit an accepted ADR.** Record the new decision in the next number and
  add `Supersedes: NNNN`. Editing history destroys the only thing that makes the
  set useful: knowing what was believed, and when.
- **Status vocabulary**: `Proposed`, `Accepted`, `Superseded by NNNN`,
  `Deprecated`.
- **Write the alternatives down.** An ADR that lists no rejected options is a
  description, and the next person will re-litigate it.
- **Say what was verified and what was not.** "Resolves on Windows" and "builds
  on Windows" are different claims; conflating them is how a decision record
  becomes fiction.
- Numbering is sequential and never reused. Title files
  `NNNN-short-slug.md`.

## Index

| # | Decision | Status |
|---|---|---|
| [0001](0001-qt6-presentation-tier.md) | Qt 6 replaces the presentation tier; the accounting engine is kept | Accepted |
| [0002](0002-event-loop-integration.md) | Bridge GLib and Qt with a pumped main context thread | Accepted |
| [0003](0003-windows-dependencies-vcpkg.md) | Windows dependencies come from a vcpkg manifest | Superseded by 0007 |
| [0004](0004-html-layer-separation.md) | Separate URL handling from the GTK widget before adding a renderer | Accepted |
| [0005](0005-windows-ci-scope.md) | Windows CI verifies the manifest, not the build | Superseded by 0008 |
| [0006](0006-prebuilt-qt-for-development.md) | Prebuilt Qt for development; the manifest stays the contract | Partly superseded by 0007 |
| [0007](0007-windows-toolchain-mingw.md) | Windows toolchain is MinGW-w64 through MSYS2 | Accepted |
| [0008](0008-windows-ci-exercises-setup.md) | Windows CI exercises the documented setup and the spike | Accepted |
| [0009](0009-backend-registration-in-qt.md) | A Qt process registers a backend by loading its module | Accepted |
| [0010](0010-qt-tier-enters-the-build.md) | The Qt tier enters the build as an opt-in target | Accepted |
