# ADR 0017 — Hidden accounts get a shared GSettings key

- **Status**: Proposed (2026-11-07)
- **Related**: [0013](0013-coexistence-with-gtk.md) — rule 3, no shared mutable
  UI state
- **Related**: [0016](0016-ledger-core-boundary.md) — the same "decide the
  boundary before writing the view" pattern

## Context

Phase 2's gate checklist has one row left that needs a decision rather than
code: whether hidden accounts are listed. GTK and the Qt tier currently
disagree, and they disagree because the setting lives in the wrong place.

GTK does not keep this in GSettings. `gnc-tree-view-account.c` holds a filter
flag (`avi->show_hidden`), toggled from the account-tree filter dialog
(`gppat_filter_show_hidden_toggled_cb`, line 2093) and persisted in the
per-user view-state file under the key `Show_Hidden`
(`gnc-tree-view-account.c:2381`, saved at `:2477`, restored at `:2574`):

- a fresh profile has `show_hidden = FALSE`, so hidden accounts are **not**
  listed;
- a saved state file with `Show_Hidden` is honoured;
- a saved state file *without* the key logs a warning and falls back to
  `TRUE` — an old-file quirk, not a policy.

The Qt account tree lists every account, because it reads the engine and
nothing else. ADR 0013 rule 3 forbids the Qt tier from reading or writing
GTK's view-state file, so "make Qt read `Show_Hidden`" is not available. The
gate cannot be closed by copying GTK's file format, and it should not be
closed by pretending the difference does not exist.

## Options

1. **A shared GSettings key.** Add `show-hidden-accounts` (bool, default
   `false`) to the existing account-tree schema both tiers already read
   (`org.gnucash.GnuCash.window.pages.account-tree.summary`, the same schema
   that holds the period keys the Qt period column uses). GTK's filter toggle
   writes it; both trees read it.
2. **Do nothing, record the difference.** The Qt tier lists all accounts and
   the gate accepts that as a known difference.
3. **A Qt-only setting.** Rejected by ADR 0013: two stores for one behaviour
   would drift, and the user would have to set the same thing twice.

## Decision

*Proposed:* option 1.

The key goes in the account-tree summary schema, not in `general`, because it
is a property of that page in both tiers and the page already owns the period
preferences:

```xml
<key name="show-hidden-accounts" type="b">
  <default>false</default>
  <summary>Show hidden accounts</summary>
  <description>Whether the account tree lists accounts marked hidden. Both the
    GTK and Qt tiers read this key, so the two views agree.</description>
</key>
```

Default `false` matches a fresh GTK profile today, so a new user sees no
change in either tier.

### Behaviour, both tiers

- The account tree lists an account unless it is marked hidden *and* the key
  is `false`. Hiding is per row, as GTK's filter is: a hidden parent is not
  listed, its children are.
- GTK's filter dialog toggle writes the key instead of only the view-state
  file. The state-file key stays readable for one release as a migration
  source, then goes.
- The Qt tree reads the key at startup, re-reads on its change signal, and
  filters rows without rebuilding the model.

### Migration

Users who had `Show_Hidden = TRUE` saved would otherwise see hidden accounts
disappear on upgrade. `gnc_gsettings_get_user_value()` distinguishes "the user
set this" from "this is the default", so GTK can migrate once, when it first
loads the account tree and the new key has no user value: if the state file
has `Show_Hidden`, write it to the new key before saving the state. No new
migration machinery is needed — the helper already exists
(`gnc-gsettings.cpp:619`).

## Consequences

- The two tiers agree, and the setting becomes visible in both: GTK's filter
  dialog and the Qt preferences dialog show the same checkbox.
- GTK changes: the filter toggle writes GSettings; restore reads the state key
  only when the new key has no user value.
- The Qt change is small: filter `xaccAccountIsHidden` rows when the key is
  false, and re-read on change.
- One more schema key is installed; adding a key is backward compatible and
  older releases ignore it, so no `GNC_FEATURE_*` entry and no resave version
  change is involved.
- The gate row closes as "done" once both tiers honour it.

## Validation

Nothing implemented: this is the proposal the gate needs, not the change.
Verified by inspection only — the GTK code paths quoted above, the schema the
period keys live in, and the existence of `gnc_gsettings_get_user_value`.
The next step, if accepted, is a schema key plus the GTK write-through and the
Qt filter, each covered by the existing self-test harness.
