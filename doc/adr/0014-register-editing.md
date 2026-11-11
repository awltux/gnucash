# ADR 0014 — Editing a split in the register, and saving the book

Status: accepted, implemented in this stage
Branch: `webui/engine-server`
Related: `ADR 0013` (the writable session and the save), `ADR 0011` (reports in
the server process), `ADR 0012` (one thread), `doc/web-ui-menu.md`

## Context

Stage 2a shipped `GET /api/register/<account-guid>` and a read-only grid: rows,
keyboard navigation, and a transaction panel. ADR 0013 shipped the machinery
this stage needs and deliberately deferred the register API itself:

> The register UI, transaction editing endpoints, validation, undo and autosave
> belong to the next stage; this ADR deliberately ships them no API.

This ADR is that next stage, in the smallest slice that is honest: the text
fields of a split, a save that already exists, and a page that never offers an
edit the server would refuse. It does not add undo, autosave or a general
transaction editor.

## Decision

### 1. Three editable fields, chosen one by one

| field | engine setter | scope | why |
|---|---|---|---|
| `description` | `xaccTransSetDescription` | the transaction, so every row of it in every account | a free string with no effect on any amount |
| `num` | `xaccTransSetNum` | the transaction | likewise a free string; the register shows it as the Num column |
| `memo` | `xaccSplitSetMemo` | the split | likewise a free string |
| `amount` | — | — | **not accepted**; see §2 |

The two transaction-level fields go through the transaction's edit
(`xaccTransBeginEdit` … `xaccTransCommitEdit`), the split field through the
transaction's edit as the header requires, and the server never writes XML.
Because none of the three is a numeric field, an edit cannot move a balance:
`register-edit.sh` asserts that every amount and running balance is unchanged
across a text edit, and that the book's later save round-trips the text.

### 2. `amount` is refused, by name, with a reason

`xaccSplitSetAmount()` does not set one number. Its header says it "always
maintain[s] balance: invoking any of them will cause other splits in the
transaction to be modified so that the net value of the transaction is zero"
(`libgnucash/engine/Split.h:236-240`). One cell edit can therefore rewrite rows
in accounts this register is not showing, and in rows of this register that are
not on screen. A register that silently rebalanced other rows would be worse
than one that does not edit amounts.

A narrower rule (only a two-split, same-commodity, no-lot transaction) would be
safe for those transactions but would be a second rule the page cannot explain:
the same cell would be editable in one row and not the next, for reasons the
grid has no column for. That is deferred rather than guessed at.

`POST /api/split/<guid>` with `{"amount": …}` answers `400 unsupported-field`,
names the field, and says why. The amount and balance cells are not editable in
the page and carry `aria-readonly="true"`; the menu items that would edit an
amount stay disabled with their own reasons.

### 3. One route, one shape

`POST /api/split/<split-guid>`, under the existing token. The body is a JSON
object of the fields being changed; at least one field is required; unknown
fields are refused rather than ignored. The response is the updated row — the
same object the register's `splits` array holds, recomputed by the same code —
so the page has one row model, not a second.

The route asks the same questions as the save route, in the same order, and
gives the same answers:

| status | `error` | when |
|---|---|---|
| 403 | `read-only` | the process was started read-only; the message names `--writable` |
| 403 | `book-readonly` | the second line of defence: the engine's book is marked read-only |
| 409 | `locked` | the book is locked; `lock` carries the `.LCK` path |
| 405 | `method` | not `POST`; the body names the method |
| 404 | — | no token (the same answer every route gives) |
| 413 | `body-too-large` | the body is over 1 MiB; refused before it is buffered |
| 400 | `bad-json` | the body is not JSON; the parser's byte offset is in the message |
| 400 | `bad-body` | the body is not an object |
| 400 | `no-fields` | the object names no field |
| 400 | `unknown-field` | names the field and the three that are accepted |
| 400 | `bad-type` | names the field and the type it got |
| 400 | `too-long` | names the field, its size and the 2048-byte limit |
| 400 | `bad-value` | control character, or not valid UTF-8 |
| 400 | `unsupported-field` | `amount`, with the reason in §2 |
| 400 | `unknown-split` | names the GUID asked for |
| 500 | `row-unavailable` | the edit committed but the row could not be recomputed |

Every 400 is a named refusal, never a silent coercion: the fields are checked
before the engine is touched, so a body with one good field and one bad one
changes neither. That is asserted (`register-edit.sh`), as is the empty string
being a real value: it clears the field rather than being ignored.

An edit that sets the value the field already had is not a change: the engine
is not asked to edit, the book stays clean, and a following save answers
`changed:false`. Without that, every keypress that ended where it started would
have dirtied the book.

### 4. The page learns the mode from the server, not from a guess

`GET /api/register/<guid>` gains two fields: `editable` (the same gate the edit
route uses) and `edit_reason` (a sentence naming read-only, or the lock and its
path). The page renders editable cells only when `editable` is true, and shows
`edit_reason` in a paragraph when it is false. A control that would be refused
is therefore never offered, and the read-only case says why in the server's own
words.

A new `GET /api/session` publishes the mode before a register is opened, so
File > Save and the About text are right on the first render. It is a new route
rather than new fields on `/api/book` because ADR 0013 keeps the read-only
bodies of the existing routes byte-identical; the register payload carries the
same two fields for the register's own use.

### 5. Dirty state is the engine's, shown per row and once per page

The edits are committed to the engine's copy of the book as they are made; only
the file is unsaved. The page therefore keeps:

* a set of split GUIDs edited since the last save, which marks those rows
  (`tr.edited`, a dot, and a title saying "Edited in this session; not saved
  yet") and which is cleared only by a save — a re-read of the register (View >
  Refresh, or the re-read after an edit) redraws the markers from that set;
* one page-level `bookDirty` flag, shown next to the Save control as "Unsaved
  changes" and used by File > Save's enablement.

Save is the existing `POST /api/book/save`: the engine writes, backs up and
verifies (ADR 0013 §3-4). On success the page clears the markers and shows the
path, the size, the backup path and its size. On `409 locked` or a `500` it
shows the named error and **keeps** the dirty state: the page never reports a
save that did not happen. That distinction is tested in the browser phase for
the success path and in the shell gate for the locked path.

### 6. The register's order is the engine's, so an edit re-reads the rows

`xaccSplitOrder` compares the date and then the number and description, so
editing `num` or `description` can move the row. The page does not assume the
rows stayed put: after every successful edit it re-reads
`GET /api/register/<guid>`, keeps the cursor on the edited split by GUID (not by
index), and re-renders. The alternative — patching the one row in place — would
show an order the server would not return on the next read.

### 7. Navigating away with unsaved changes is refused until it is saved

The dangerous navigation is closing or reloading the tab: the engine does not
save on exit (ADR 0013 §2), so unsaved edits are lost. In-page navigation is a
different case — switching account leaves the edits in the engine — but the
page can only keep the save one click away while it is still on the register,
so it treats it as dangerous too.

* **In-page** (clicking another account, Edit > Open Account): a confirm says
  how many rows are unsaved and offers "OK saves and continues" / "Cancel stays
  here". OK runs the save and opens the account only if the save succeeded; a
  failed save leaves the page where it was with the error shown. Cancel does
  nothing, not even moving the tree's `aria-current`.
* **Tab close or reload**: a `beforeunload` listener sets `returnValue` while
  dirty. Chromium only shows that dialog after a user interaction, which an edit
  always is; the listener itself is what the browser test can check, and it does
  (with a synthetic event), alongside the real dialog for the in-page case.

A `beforeunload` guard alone would not cover the in-page case, so both exist and
both are exercised.

### 8. Keyboard and pointer behaviour on a cell

* Click or `Enter` starts editing the cell (click on the cell; `Enter` on the
  row edits the cursored cell, or the row's first editable one).
* `Escape` cancels. Clicking elsewhere also cancels: a value is written only by
  an explicit `Enter` or `Tab`, so a half-typed value can never reach the
  engine by accident.
* `Tab` commits and opens the next editable cell (row after row when the row's
  fields run out); `Shift+Tab` goes back. `Enter` commits and returns focus to
  the row, so the arrow keys keep navigating.
* `←`/`→` move the cell cursor between the editable cells, `↑`/`↓`/`Home`/
  `End`/`PageUp`/`PageDown` keep their row navigation. None of this exists in a
  read-only session, where the cells are text and `Enter` still opens the
  transaction panel.
* `Shift+Enter` opens the transaction panel in a writable session; the panel
  itself stays read-only (it is a second view of the same transaction, and it
  has no editor).

### 8.1 The stage-1 self-test route stays

ADR 0013 said `/api/book/selftest` would be removed when the register API
landed. It has not been removed, and this is the record of that: `book-save.sh`
uses it to exercise the save path (a dirty book, the backup, the reported sizes,
the locked 409) without also depending on the edit route, and
`register-edit.sh` exercises the edit route on its own. Removing the route
would delete a test node, not a user-facing surface, and ADR 0013's §7 list of
what stays read-only is unaffected. It is still not a general transaction
endpoint.

### 9. What stays read-only

Not implied by this stage: amounts, dates, accounts, reconcile/void state,
adding or deleting splits and transactions, cut/copy/paste of register cells,
undo, autosave, multi-tab coordination, Save As and book switching, business
documents, prices and imports (ADR 0013 §7). Every menu item that still cannot
act carries the reason that is true in a writable session too, and the items
whose old reason said "the session is read-only" were reworded rather than left
to become false.

### 10. Two saves in one second: wait out the backup name, retry once

The engine names the pre-save backup `<book>.<YYYYMMDDHHMMSS>.gnucash`
(ADR 0013 §3) and `backup_file()` refuses a name that already exists, so two
dirty saves inside one second collide: the second used to answer
`500 backup-failed` and leave the book untouched. ADR 0013 forbids a second
backup naming rule, so the server must not invent a copy under another name.

The server now recognises the collision instead of reporting it as a failure.
After `ERR_FILEIO_BACKUP_ERROR` it asks the engine's own
`gnc_date_timestamp()` for the name this save would have used; if that file
exists **and the book's directory is still writable**, the save collided (an
unwritable directory leaves the name free and must not pause before failing;
a full disk passes the writability check and is retried, which is harmless).
It then sleeps until the timestamp changes and retries `qof_session_save`
**once**. The retry names a new file, so it is still the engine's naming rule
and still the engine's backup. If the retry fails too, the `500` is not
swallowed: it names the backup path and the reason ("a backup with this
second's name already exists", or "the engine could not create it … check that
the book's directory is writable"), keeps `path_exists`, and the book is
untouched.

Why waiting rather than serialising: saves are already serial — the server has
one thread — so serialisation cannot fix this. The collision is with the
*previous save's own* backup name, not with a concurrent request. The wait is
bounded (a 10 ms poll until the second changes, hard-capped at two seconds) and
only happens on a collision. It blocks the single-threaded server for that
second, which is the cost; it is acceptable for a local single-user server
because the alternative is a failure for a save that is about to become
possible. The retry is one, not a loop: a name still taken after the second
boundary is a real failure.

`web/test/register-edit.sh` covers it: two back-to-back edit/save pairs both
answer `200` and name distinct backups; a planted same-second name makes the
second save retry, leaves the planted file untouched, and names the retry's new
backup; an unwritable directory still answers the named `500`, with the backup
path, no retry, and the book unchanged.

## Consequences

* A writable session can now do the smallest useful thing: fix a description, a
  number or a memo, and save it with the engine's own backup. A read-only
  session is untouched: the same grid, no Save control, and a sentence saying
  how to start a writable one.
* A transaction-level edit changes rows in every account that holds the
  transaction, but only the rows of the register on screen are marked. The
  server has no "tell me what else changed" route, and adding one would be a
  different stage; the ADR records the gap rather than hiding it.
* Two processes may still both write the book (ADR 0013 §5a); this stage does
  not add conflict detection. The `.LCK` and the pre-save backup remain the
  whole story.
* Stopping the server with unsaved edits loses them; the tab guard is the only
  warning, and it is the browser's dialog, not a server promise.
* **Two dirty saves in the same second no longer fail on the backup name.**
  The engine still names the backup `<book>.<YYYYMMDDHHMMSS>.gnucash` and still
  refuses a name that exists (ADR 0013 §3), but the server now recognises that
  collision, waits for the next second and retries once (§10).  If the retry
  fails too, the route answers `500 backup-failed` naming the backup path and
  the reason, with `path_exists:true`; the book is untouched.  ADR 0013's "does
  not retry without a backup" still holds: the retry waits for the engine to be
  able to make the backup, it never saves without one.  The cost is that a
  collision blocks the single-threaded server for up to a second.

## Tests

* `web/test/register-edit.sh` — the route in both modes: every field refused
  with a named 403 in read-only mode and the book byte-identical; the 405 and
  the tokenless 404; the named 400s and the 413; a refused edit does not dirty
  the book; a good edit returns the row, the register reflects it and no amount
  or balance moves; a save names a backup byte-identical to the pre-edit book; a
  fresh server re-opens the book with the edits; a same-value edit leaves the
  book clean; two back-to-back saves succeed with distinct backups and the
  final book carries both edits; a planted same-second backup name is waited
  out and retried once; an unwritable directory still fails with the named
  `500`, the backup path and no retry; a locked book is a 409 naming the
  `.LCK` and changes nothing.
* `web/test/register-edit-ui.mjs` — the writable browser phase: the editable
  cells and the non-editable amount; click/Escape/blur; keyboard-only editing
  through Tab; the row and page dirty state; File > Save before and after an
  edit; Save's report, the markers clearing, a reload showing the persisted
  edit; a 400 keeping the server's text and not marking the row; the in-page
  guard refusing and then saving; the beforeunload guard while dirty and clean;
  no console errors.
* `web/test/register-ui.mjs` — the read-only phase gained the negative checks:
  no visible Save control, no editable cell, no editor on click, and the reason
  on screen.

## Deferred

* Amount editing, with a way to show which other splits a rebalance moved (or a
  rule the register can explain per row).
* Adding, deleting and voiding transactions and splits; cut/copy/paste; undo;
  autosave; save-on-exit.
* Conflict detection when another process writes the book.
* A dirty indicator that covers edits made in another tab of the same process.
