# ADR 0004 — Separate URL handling from the GTK widget before adding a renderer

- **Status**: Accepted (2026-11-06)
- **Supersedes**: none
- **Related**: [0001](0001-qt6-presentation-tier.md)

## Context

`GncHtml` is a `GtkBin`. Its class structure mixes two concerns: the widget that
embeds a renderer, and the bookkeeping that turns a URL string into a
(type, location, label) triple. The URL type registry, protocol lookup, URL
building and URL parsing all lived in `gnc-html.cpp`, reachable only by linking
GTK, and the parser read the base type and base location out of widget-private
state.

A Qt front end needs the second concern and cannot use the first.

## Options

1. **Leave it until the Qt work starts.** Duplicate the parsing logic on the Qt
   side.
2. **Rewrite the whole HTML layer behind a new abstraction now.** Replace the
   vtable, both WebKit backends and every caller in one change.
3. **Extract the parts that are already widget-free**, leave the vtable and
   callers untouched, and defer the renderer.

## Decision

Option 3.

## Rationale

- Option 1 guarantees two parsers that drift, and the URL grammar is subtle
  enough (protocol, path, anchor, base resolution) that a divergence becomes a
  behaviour bug.
- Option 2 is a large, risky change that cannot be tested against a Qt renderer
  that does not exist yet, and it would churn every caller for no current gain.
- Option 3 removes the dependency that actually blocks reuse, keeps the
  `GncHtmlClass` vtable and all callers intact, and leaves the remaining
  GTK-shaped part visible instead of pretending it is gone.

## Consequences

- `gnc-html-url.{h,cpp}` now contains the registry, URL building and parsing,
  and includes only glib. `gnc_html_parse_url()` keeps its signature and
  delegates, so the vtable and callers are unchanged.
- Base type and base location are parameters, so a non-GTK front end supplies
  its own instead of widget state.
- The renderer, the document history and the `GtkBin` embedding are still
  GTK-bound. That is the next seam, and it is not hidden.
- The new unit is tested by a program that links `gnc-engine` and `gtest` only.
  If the unit ever re-acquires a GTK dependency, that test stops linking, which
  makes the invariant enforced rather than documented.

## Validation

`gnc-html` builds and links from the full GTK configure on Ubuntu 24.04, and
`nm -D` shows the moved symbols exported from `libgnc-html.so`.
`test-gnc-html-url` passes (12 cases: registry, protocol lookup including null
and unknown, jump URLs, relative and absolute resolution, URL building).
