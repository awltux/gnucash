# ADR 0015 — Creating a transaction from the register

Status: accepted, implemented in this stage
Branch: `webui/engine-server`
Related: `ADR 0013` (the writable session, the save, the lock and the backup),
`ADR 0014` (the register, the split edit, the two-step save model),
`doc/web-ui-menu.md`

## Context

ADR 0014 shipped the smallest honest register edit: the text fields of a split
(`description`, `num`, `memo`), committed to the engine and written by the
existing `POST /api/book/save`. It deliberately did not add, delete or void a
transaction, and it refused `amount` because the engine's setter can rebalance
splits in other accounts.

That leaves the register unable to record anything new. This stage adds one
way to create a transaction, in the same two-step shape the edit already uses:
the engine's copy of the book changes first, and the file changes only when the
user saves. It does **not** turn the register into a spanning grid, and it does
not add amount editing to existing rows.

## Decision

### 1. Creation is a form, not a row of the register

The desktop's register is a spanning grid: the blank line at the bottom is a
new transaction being typed in place, and the grid grows rows as splits are
added. The web register is a normal table of the account's existing splits, one
row per split, with an inline band for a transaction's other legs (ADR 0014).
It has no blank row, no cell that can hold a not-yet-existing split, and no
place to put a transaction-level date for a row that does not exist.

Growing the table into a spanning grid is a different and larger change: merged
cells per transaction, a split-row model inside a row, a cursor model that can
enter and leave a transaction being written, and a second rendering for every
row. That change is deferred as a whole. This ADR ships a form instead:

* a dialog in the register area (`#transaction-dialog`) with the transaction's
  date, number and description and a table of split lines;
* each line has an account picker, an amount and a memo; lines can be added and
  removed;
* the account picker's label carries the account's subtree total, computed by
  the same `account_total_text()`/`AccountFigures` call the account tree and the
  register header use, so the three figures cannot disagree.

The form is not a claim that this is the register's final form. It is the
smallest editor that can express a whole transaction honestly, and ADR 0014's
inline band remains the read-only view of an existing one.

### 2. The engine decides balance; the page does not add amounts

The form shows a running sum and a verdict, but neither is computed in
JavaScript. On every change (debounced) the page posts the body to
`POST /api/transaction/validate` and displays the server's answer: the signed
sum, the debit total, the currency, `balanced`, and the imbalance when it is
not balanced. The create route then repeats the whole check.

`POST /api/transaction` refuses an unbalanced body with `400 unbalanced` before
the engine is touched, and after it commits it asks `xaccTransIsBalanced()`
itself. If the engine disagrees with the pre-check, the transaction is removed
again with `xaccTransBeginEdit`/`xaccTransDestroy`/`xaccTransCommitEdit` and the
route refuses. So:

* the rule is implemented once, in the engine / the server's C++, not twice;
* the page cannot enable Create on a body the server would refuse;
* the engine's answer, not the page's, is the last word on what was created.

The split band's `balanced`/`imbalance` fields come from the same engine call
(`transaction_json()` uses `xaccTransIsBalanced()` and
`xaccTransGetImbalanceValue()`), so the band and the form use one vocabulary.

### 3. Two steps, labelled as two steps

Creating commits the transaction to the engine's in-memory book. It does not
write the file. The existing Save (`POST /api/book/save`, ADR 0013 §3-4) is
still the only writer, with the engine's lock, backup and post-save check.

The page therefore says so in three places, all tested:

* the form's own note: "Creating puts the transaction in the book and leaves it
  **unsaved**. Use Save on the register to write the book to its file.";
* after a successful create, the register's status line: "Created transaction
  … — not saved yet. Save writes it to the book.";
* the existing page-level dirty state ("Unsaved changes") and the new row's
  dirty marker, which save clears and which a failed save keeps (ADR 0014 §5).

The create response carries `book_dirty` (the engine's
`qof_book_session_not_saved()`) and the register rows the new splits produced,
so the page has the server's account of the state rather than inventing one.

### 4. Validation belongs to the server, and everything is checked first

The body is checked completely before the engine is asked for anything. A
refused request cannot create half a transaction, cannot dirty the book, and
cannot consume a split GUID. The checks, each a named `400` (the same style as
ADR 0014 §3):

| `error` | when |
|---|---|
| `bad-json` | the body is not JSON (the parser names the byte offset) |
| `bad-body` | the body is not a JSON object |
| `no-fields` | `{}` |
| `unknown-field` | a top-level field other than `date`, `number`, `description`, `splits` |
| `missing-field` | no date, no splits, no account or no amount on a line; `field` names it |
| `bad-type` | a field of the wrong JSON type |
| `bad-date` | not `YYYY-MM-DD`, or a day that does not exist |
| `too-few-splits` | zero or one split |
| `too-many-splits` | more than 50 splits |
| `bad-split` | a split that is not an object |
| `unknown-split-field` | a split field other than `account`, `amount`, `memo` |
| `unknown-account` | the GUID names no account |
| `placeholder-account` | the account is a placeholder and holds no splits of its own |
| `unsupported-account` | a trading account (a different balance rule; deferred) |
| `account-no-commodity` | the account cannot hold an amount |
| `mixed-currency` | the splits do not share one commodity |
| `bad-amount` | not a decimal number, more decimal places than the commodity's fraction, or a sum the engine cannot represent |
| `too-long` | a text field over 2048 bytes |
| `bad-value` | a control character, or invalid UTF-8 |
| `unbalanced` | a well-formed body whose splits do not sum to zero; carries `imbalance`, `sum`, `currency` |

`amount` is a decimal string in the split's account commodity, with an
optional sign. Amounts are converted to the commodity's fraction with
`GNC_HOW_DENOM_FIXED | GNC_HOW_RND_NEVER`, so `1.234` in USD is a named
`bad-amount`, never a silent rounding. All splits must share one commodity;
multi-currency splits are deferred (see below).

There is also a mode gate, ahead of the body checks and identical to the edit
and save routes: `403 read-only` in a process started without `--writable`
(naming the flag), `403 book-readonly` from the second line of defence, and
`409 locked` (naming the `.LCK`) when the book was opened read-only because
another process holds it. A body over 1 MiB is a `413 body-too-large` before it
is buffered. Every other HTTP verb is a named `405`.

The account picker comes from `GET /api/accounts/options`, a new read-only
route. It lists the accounts the create route accepts (not placeholders, not
trading accounts, with a commodity) and their subtree totals. It is a separate
route rather than more fields on `/api/accounts` because ADR 0013 keeps the
read-only bodies of the existing routes byte-identical.

### 5. The lock, including the case where it was lost after the form opened

The lock is acquired at open and held for the process's lifetime (ADR 0013
§5); there is no lock per request. Two honest cases follow:

* **The book was locked when this server started.** It re-opened read-only,
  `g_server.writable` is false, and both `POST /api/transaction` and the
  validate route answer `409 locked` naming the `.LCK` before the engine is
  touched. The page does not offer the form at all in this session (the
  register payload's `editable` is false and `/api/session` says why), so a
  user who sees the form is in a writable session; a request that arrives
  anyway gets the named refusal.
* **The lock was removed or overridden after the form opened.** This process
  still holds the engine's open book and the engine's save does not re-check
  the `.LCK` before writing (ADR 0013 §5a). A form opened before another
  process breaks the lock and writes can therefore create a transaction that a
  later save would write wholesale over the other process's changes. There is
  no engine mechanism to detect that, so this stage does not invent a
  server-side mtime/inode check that the desktop would not honour. The named
  409 and the pre-save backup remain the story, exactly as ADR 0013 §5a
  records. The ADR says this rather than pretending the form can be atomic
  with a process it cannot see.

The create route does not re-open or re-lock the book, for the same reason the
save route does not: the process owns one session, and two lock stories would
be worse than one.

### 6. Concurrency inside the process

The server is single-threaded (ADR 0012), so a create and a save cannot
interleave. The form's validate request is read-only (it creates nothing) and
cannot race a create into the same transaction. Two browser tabs are two
requests to the same process; each create is a complete transaction, and the
second tab's register re-read sees the first tab's uncommitted-to-file change
because it is in the same engine book. This stage does not add coordination
between tabs beyond that, and ADR 0014's dirty state stays per page.

### 7. What is deliberately not built

* **Multi-currency splits.** One transaction currency, all splits in it. A
  transaction with an exchange rate needs value/amount pairs, price handling and
  a register column model that can show them; refusing `mixed-currency` is
  better than writing a transaction whose values are guesses.
* **Scheduled transactions.** No scheduler, no template store, no "since last
  run" — the web tier has none of that book data (ADR 0013 §7).
* **Reconciliation state.** New splits are `n` (not reconciled); the form has
  no reconcile flag, and reconcile remains a desktop action.
* **Editing an amount on an existing row.** Still refused by ADR 0014 §2. The
  form creates whole transactions; it does not touch an existing one's money.
* **Deleting, voiding, duplicating, cut/copy/paste of transactions.** Not
  implied by creation and not added here.
* **The spanning grid itself.** The form is a dialog; the register's row model
  is unchanged.
* **A business-document or invoice link.** Creation makes a plain transaction;
  no owner/lot/document link is written.

### 8. The page's half

* `New Transaction` is a button in the register's actions row and the Actions
  menu's `Blank Transaction` item (the desktop register page's action, which
  the web menu table carries in Actions; its accelerator label is `Ctrl+B`, and
  the browser does not bind it, as with every other accelerator hint). The menu
  item is enabled only in a writable session with a register open; in a
  read-only session it is disabled with a reason naming `--writable`, and the
  button is inside the hidden actions row. Nothing is a silently inert
  control. The Transaction menu itself still has no creation item: its 14
  desktop items need the grid or an action the web tier does not have, and
  their note now points at Actions > Blank Transaction.
* The form is keyboard-first: `Enter` in a field adds a split line (buttons
  keep their native `Enter`), `Tab` moves through the fields in document order,
  `Escape` cancels. `Escape` and Cancel on a dirty form ask before discarding;
  dismissing the question keeps the form and its values.
* The verdict is live but debounced; the Create button is enabled only when the
  server says the body is balanced. A server-side `400` keeps the dialog open,
  shows the named error, and leaves the book untouched.
* On success the dialog closes, the register re-reads, the new row(s) are
  marked edited, the page-level dirty state is set, and the status line says
  the transaction is not saved yet.
* If the new transaction is not in the account whose register is open (all its
  splits are in other accounts), the register correctly shows no new row; the
  status line still says it was created, and Save still persists it. The form's
  first line defaults to the open account so the common case shows the row.

### 9. The stage-1 self-test stays

`POST /api/book/selftest` is still present for `book-save.sh` for the same
reason ADR 0014 §8.1 records. It is not a general transaction endpoint and the
create route does not use it.

## Consequences

* A writable session can record a transaction: create it from the form, see it
  in the register, save the book with the engine's backup. A read-only session
  is unchanged and cannot reach the route.
* Creating is two steps by design. A user who creates and then closes the tab
  without saving loses the transaction, exactly as ADR 0013 §2 says for edits;
  the form and the status line say so instead of implying otherwise.
* The form is not the desktop's spanning register. It does not support entering
  a transaction split-by-split in place, and it will not grow into that by
  accident: a later spanning-grid stage would replace the form, not extend it.
* Single-currency only. A book whose transactions need exchange rates can still
  be read and its text edited; it cannot create those transactions here.
* A second process's write after the form opened is not detected (ADR 0013 §5a),
  and the pre-save backup is the recovery.

## Tests

* `web/test/register-create.sh` — read-only refusal (403 named, book untouched,
  no lock), 405/404; the validate route's balanced/unbalanced verdicts; a
  balanced two-line create (transaction shape, `rows`, `book_dirty:true`, the
  register row, the read route, Save with a pre-create backup); a fresh server
  re-opens the saved transaction with both splits and memos; the named 400/413
  battery and a save proving none of them dirtied the book; the locked 409
  naming the `.LCK` and changing nothing.
* `web/test/register-create-ui.mjs` — the New Transaction entry, the form and
  its "unsaved" note, account options with subtree totals, the server's running
  sum and verdict, the verdict flipping and Create being refused, Enter/Tab,
  Escape on a dirty form, create → row + dirty + "not saved yet", Save clearing
  it, reload showing the transaction.
* `web/test/register-ui.mjs` — the read-only half: no New Transaction control,
  Transaction > Blank Transaction disabled with a reason naming `--writable`,
  and no `--debug-port` banner in a token-mode page.
* `web/test/debug-port-ui.mjs` — the `--debug-port` banner's box: it sits above
  the menu without intersecting it and is fully visible at 1280px and 800px,
  and its layout is a stylesheet rule rather than an inline style.

## Deferred

* The spanning grid, and with it amount editing on an existing row.
* Multi-currency transactions and exchange rates.
* Scheduled transactions, reconcile state, void/delete/duplicate.
* Conflict detection when another process writes the book (ADR 0013 §5a).
* Undo and autosave (ADR 0014's list stands).
