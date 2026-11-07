# ADR 0014 — The account tree is a model over engine objects, not a snapshot

- **Status**: Accepted (2026-11-06)
- **Related**: [0011](0011-register-decoupling.md), [0012](0012-gui-thread-owns-the-engine.md)

## Context

The Phase 2 shell fills a `QTreeWidget` once, when the book loads. That is
enough to prove the engine can be displayed, and wrong as a foundation:

- the tree never notices a new account, a rename, or a deletion;
- balancing columns need values the tree does not fetch;
- selection and expansion are lost whenever anything forces a rebuild.

The alternative is to rebuild the tree on every `QofEvent`, which is simpler to
write and cannot scale: a burst of edits (the very thing the load test produces)
would rebuild the widget N times and drop the user's selection each time.

## Decision

The account tree is a `QAbstractItemModel` over engine objects:

- Items are references to `Account` objects resolved through the engine, never
  copies of their fields beyond the strings currently displayed.
- Children are fetched lazily from `gnc_account_get_children()`, so a large
  chart is not walked on load.
- `QofEvent` notifications map to targeted `dataChanged`, `beginInsertRows` and
  `beginRemoveRows` signals for the affected parent — not to a model reset.
- Anything cached (balances, display strings) is invalidated by GUID-keyed
  lookup, so a stale pointer can never be dereferenced after a deletion.

## Consequences

- The model must treat `Account*` as untrusted between calls. GnuCash account
  objects can be destroyed; a cached pointer outliving its account is a crash,
  so the model stores GUIDs alongside pointers.
- Sorting follows the engine's own order plus user-defined sort keys, not the
  view's default; a proxy handles the display order.
- Balance columns are computed on demand and cached per event generation. Naive
  per-cell recomputation during painting would be quadratic in the tree size.
- The register will want the same patterns (ADR 0011), so getting this right in
  the tree is cheaper than getting it wrong twice.

## Validation

Design only; the shell still uses a `QTreeWidget` snapshot. Nothing here is
implemented or measured yet, and the performance claims (lazy children, cached
balances) are unverified. The first implementation should carry a headless test
that mutates the engine and asserts the model emits edits rather than resets.
