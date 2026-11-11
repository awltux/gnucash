# ADR 0013 — A writable web session, opt-in and separately guaranteed

Status: proposed
Branch: `webui/engine-server`
Related: `ADR 0011`, `ADR 0012`, `WEBUI.md`, `doc/web-ui-roadmap.md`

## Context

`gnc-webd` opened its book with `SESSION_READ_ONLY`
(`web/server/main.cpp`, `qof_session_begin (session, args->book, SESSION_READ_ONLY)`)
and had no write endpoint. That is why the register cannot exist and why most of
the menu bar is disabled. The register stage needs the engine to accept edits
and the book to be saved, and both change the safety contract the two earlier
ADRs made: "read-only, loopback, token".

This ADR records how the desktop writes — from the code, not from memory —
and then decides what the server may do with those mechanisms and what it may
not.

### How the desktop writes

**The save is one call into the backend.** `qof_session_save` →
`QofSessionImpl::save` returns immediately when `qof_book_session_not_saved()`
is false (a clean book is not rewritten), then calls `m_backend->sync(book)`
and turns the backend's error into the session's error
(`libgnucash/engine/qofsession.cpp:436-475`). The file backend's `sync`
re-checks `qof_book_is_readonly` and then calls `write_to_file(true)` followed
by `remove_old_files()` (`libgnucash/backend/xml/gnc-xml-backend.cpp:302-324`).
`qof_session_end` releases the lock and does **not** save
(`libgnucash/engine/qofsession.h:277-284`), so nothing is written implicitly:
every write is an explicit save.

`qof_session_safe_save` is a different operation: it calls `safe_sync`, which
the SQL backend implements by moving tables aside for a schema upgrade
(`libgnucash/engine/qofsession.cpp:477-492`;
`libgnucash/engine/qof-backend.hpp:244-246`). It is not the routine save and
this ADR does not use it.

**The XML backend makes a backup before it touches the original.**
`write_to_file` writes the book to `<book>.tmp-XXXXXX`, and only then *unlinks*
the original and links the temp file into its place
(`gnc-xml-backend.cpp:377-379`, `:410-411`, `:458-476`). Before any of the
destructive steps it calls `backup_file()`, and a failed backup aborts the save
with the original still in place (`:400-408`). `backup_file` creates
`<book>.<timestamp>.gnucash` by hard link, falling back to a byte copy on
filesystems that cannot link (`:591-632`, `:675-702`). The timestamp is
`YYYYMMDDHHMMSS` (`GncDateTime::timestamp`, `libgnucash/engine/gnc-datetime.cpp:639-645`),
which is why the cleanup pass recognises `\.[0-9]{14}` before it will delete a
file (`gnc-xml-backend.cpp:776-799`). Old backups are removed *after* a
successful save according to the file-retention preference
(`:801-828`). There is no `gnc_file_backup()`; the backup is
`GncXmlBackend::backup_file()`.

**The save is not atomic.** The sequence is: write temp → make backup →
`unlink(original)` → `link_or_make_backup(temp, original)` → `unlink(temp)`.
If the process dies or the link fails between the unlink and the link, the
book is *gone from its path* even though the complete pre-save copy exists as
the backup, and the error reported is `ERR_FILEIO_BACKUP_ERROR` with
"Failed to make backup file <path>" (`:468-476`). A rename would be atomic;
the engine does not use one. The desktop has always lived with this window.

**Locking is a `<book>.LCK` file, created at open, held for the session.**
`session_begin` returns before any lock work when the mode is
`SESSION_READ_ONLY`, so a read-only open neither honours nor creates a lock;
otherwise it sets `m_lockfile = <path>.LCK` and `get_file_lock` creates it
`O_RDWR|O_CREAT|O_EXCL`. `EEXIST` is `ERR_BACKEND_LOCKED`; `EACCES`/`EROFS`/
`ENOSPC` are `ERR_BACKEND_READONLY` with a message naming the directory problem
(`gnc-xml-backend.cpp:151-156`, `:634-672`). `SESSION_BREAK_LOCK` ignores an
existing lock (`:667-671`). `session_end` closes and unlinks the lock file
(`:159-197`). There is no PID in the lock file: it is created empty, so a
crashed process leaves a lock that cannot be attributed to a process. The old
`.LNK` lock files are only cleaned up as stale leftovers on a save
(`:750-761`); `gnc_lockfile.c` no longer exists in this tree. The SQL backend
has no file lock at all; it relies on the database's own transactions
(`gnc-sql-backend.cpp:474-540`).

**The desktop's user-facing rules.** On open, `gnc_file.c` passes
`SESSION_NORMAL_OPEN`, and on `ERR_BACKEND_LOCKED` or `ERR_BACKEND_READONLY` it
asks the user with four buttons: Open Read-Only (`SESSION_READ_ONLY`), Create
New File, Open Anyway (`SESSION_BREAK_LOCK`), Open Folder
(`gnucash/gnome-utils/gnc-file.c:882-999`). "Open Read-Only" also marks the
book read-only so the save path refuses later (`:1045-1050`). On save it calls
`qof_session_save`; a read-only book offers Save As instead
(`:1469-1480`); any error is shown as a dialog and then falls through to Save
As (`:1493-1503`). The dialogs name the file and the problem: "could not obtain
the lock for %s", "could not write to %s … read-only file system …", "Could not
make a backup of the file %s", "Could not write to file %s. Check that you have
permission …" (`:355-415`, `:493-503`). Auto-save is only a timer that calls
the same `gnc_file_save` after the book becomes dirty
(`gnucash/gnome-utils/gnc-autosave.c:53-78`, `:175-232`), so each auto-save
also produces a timestamped backup; it is not a second backup mechanism.

**The SQL backend is a different contract.** Its `sync` writes everything in
one database transaction and rolls back on failure
(`gnc-sql-backend.cpp:490-537`). It has no `.LCK` and no file backup; recovery
is the database's job. The engine's `safe_save`/`safe_sync` exists for its
schema migrations, not for ordinary saves.

## Decision

### 1. Writable mode is a separate, per-process, opt-in mode

`gnc-webd` stays read-only unless started with `--writable` (or
`GNC_WEBUI_WRITABLE=1` in the environment). With neither, the process opens
`SESSION_READ_ONLY` exactly as it does today: no `.LCK`, no backup, no write
route reachable, and every existing response byte-identical. There is no
per-request or per-book way to escalate: the mode is fixed at startup, so a
request cannot turn a read-only server writable. A writable process prints a
second startup line naming its mode (and the lock, when it could not get one);
a read-only process prints exactly what it printed before.

`--writable` is refused for a URI that is not a local filesystem book
(`gnc_uri_targets_local_fs`), because the lock-and-backup guarantees this ADR
makes are the XML backend's. A SQL book in writable mode would have neither a
`.LCK` nor a file backup; that is a different ADR and is deferred.

There are two lines of defence, not one. Every write route asks the mode first;
in addition, a process that is not writable marks its book read-only
(`qof_book_mark_readonly`), exactly as the desktop does after "Open Read-Only"
(`gnc-file.c:1045-1050`). The engine's own save paths check that flag
(`gnc-xml-backend.cpp:315`, `:364`), so a future route that forgot its gate still
cannot write a book this process opened read-only.

### 2. The engine saves; the server never writes the file

`POST /api/book/save` calls `qof_session_save` on the session the process
already owns and reports what the engine did. The server does not copy, rename,
truncate or "fix" the book itself, and it does not call `safe_save` (that is
the SQL migration path). This keeps one writer and one backup rule: the ones
the desktop uses.

Because the book is open for the life of the process, saving is "write the
session's state", not Save As. Save As, New Book and book switching are out of
scope (roadmap workstream 1).

Stopping the process does **not** save. `qof_session_end` only releases the
lock (`qofsession.h:277-284`), and a server has no dialog to ask with, so any
edit not followed by an explicit save is lost — the same thing that happens to
a desktop user who chooses "Discard" at the quit prompt, but without the prompt.
Save-on-exit is deferred rather than guessed at.

### 2a. One stage-1 route proves the path, and is scaffolding

`POST /api/book/selftest` exists only to prove mutation works end to end before
a register exists. In writable mode it creates one balanced two-split
transaction — one unit of the first commodity that two non-placeholder accounts
share — through the engine's instance edit/commit path, and returns the
transaction GUID and both accounts. It refuses with `409 no-accounts` when the
book has no such pair. It is not a general transaction endpoint: the register
API replaces it, and it is listed under Deferred.

A book-level setting would have been smaller, but it would not exercise the
path the register uses (begin edit, commit, backend commit, session save), so
the transaction is the smallest change that proves the right thing.

### 3. Backup policy: the engine's backup is the precondition, not a courtesy

The backup is made by `backup_file()` *before* the original is unlinked. If the
backup fails the save is refused and the book on disk is untouched — the server
surfaces that as `500 backup-failed`, naming the book, and does **not** retry
without a backup. A save that writes the book returns the backup path and its
size, so the user has a concrete, named artefact.

(ADR 0014 §10 adds one narrow exception: when the backup name is already taken
by a save in the same second, the server waits for the next second and retries
once, so the retry is still the engine's own backup under the engine's own
naming rule — never a save without one. A genuine backup failure still answers
the named `500` on the first attempt.)

The server makes no backup of its own: a second copy with a different naming
rule would be a second story to keep true, and would not match what a desktop
user finds next to the book.

### 4. Atomicity: accepted as it is, with an explicit post-save check

The XML save's unlink-then-link window is not atomic. This ADR accepts the
engine's sequence rather than inventing a shadow file protocol in the server,
because the backup is complete before the destructive step and the failure is
recoverable from the named backup. What the server adds is a **post-save
verification**: after a save that the engine reports as successful, it stats
the book path and requires a non-empty regular file. If the file is missing or
empty, the route answers `500 write-lost` and names both the book path and the
backup path — it never reports success for a book it cannot see. On a failed
save it also reports whether the original still exists, so a user is told
immediately whether they need the backup.

An atomic save (write temp, `fsync`, `rename`) is an engine change with its own
risk (permissions, ownership, Windows semantics). It is deferred, not assumed.

### 5. Locking: the engine's lock is the only concurrency control

In writable mode the book is opened with `SESSION_NORMAL_OPEN` (or
`SESSION_BREAK_LOCK` with `--break-lock`), so the server takes the same
`<book>.LCK` the desktop takes, at open, and holds it until it exits. Two
consequences, both intended:

* While a writable server runs, the desktop cannot open the book read-write
  (it gets the lock dialog) and vice versa.
* A second writable server on the same book gets `ERR_BACKEND_LOCKED` and is
  not allowed to write.

When the open fails because the book is locked, the server does **not** exit
and does **not** break the lock: it re-opens the book with `SESSION_READ_ONLY`
(the same fallback `gnc-file.c:975-979` uses for "Open Read-Only"), prints
which lock blocked it, and answers every write route with `409 locked`, naming
the `.LCK` path. There is no dialog in a server, so the safe branch is chosen
automatically and said out loud.

`--break-lock` exists only to mirror the desktop's explicit "Open Anyway"
(`SESSION_BREAK_LOCK`) and is documented as dangerous: it ignores another
process's claim on the book. It does not take the lock over: the other
process's `.LCK` stays exactly where it is, this process holds no lock of its
own, and `qof_session_end` therefore removes nothing. The `.LCK` file records
no PID, so a stale lock from a crash cannot be attributed; the recovery is the
same as the desktop's — be sure no other GnuCash has the book, then
`--break-lock` or remove the file.

The server installs a SIGINT/SIGTERM handler that quits its main loop and then
calls `qof_session_end`, so an ordinary Ctrl-C releases the lock. A `SIGKILL`
or a crash leaves it, exactly as a desktop crash does.

### 5a. When the desktop application has the same book open

The lock is decisive for the *default* path on both sides, and it cannot stop
an explicit override on either side. Concretely:

* **Desktop first, server second.** The desktop's `SESSION_NORMAL_OPEN` created
  `<book>.LCK`. A writable `gnc-webd` then gets `ERR_BACKEND_LOCKED`, falls back
  to `SESSION_READ_ONLY`, and every write route answers `409 locked`. The
  server cannot write while the desktop holds the book.
* **Server first, desktop second.** The server holds `<book>.LCK` for its whole
  lifetime. The desktop's open fails with `ERR_BACKEND_LOCKED` and it shows the
  four-button dialog; the safe button is "Open Read-Only", and that path also
  marks the book read-only so the desktop's own save refuses later
  (`gnc-file.c:975-979`, `:1045-1050`).
* **Both write, only if someone says so.** The desktop's "Open Anyway"
  (`SESSION_BREAK_LOCK`) ignores the existing lock rather than taking it over,
  and so does `gnc-webd --break-lock`. After that, both processes write: the
  XML save rewrites the whole book from the session in memory, so the last
  writer wins wholesale and the other process's changes are lost. There is no
  merge and no conflict detection, and the engine's save does not re-check the
  lock before writing — the lock is a claim made at open, not a guard held
  around the write.

**Decision: accept that risk, because the engine gives no mechanism to do
better.** This ADR does not add a second lock, an mtime/inode check, or a
"someone else wrote this" refusal: each would be a new invention that the
desktop does not honour, so it would protect only against itself. What the ADR
does decide is that the override is always explicit and never accidental (no
silent lock-breaking anywhere, including at startup), that the refusal is named
(`409 locked`, with the `.LCK` path), and that the pre-save backup is the
escape hatch when two writers do collide. Detecting an external write and
refusing or merging is listed under Deferred; it needs an engine-level answer,
not a server-side guess.

### 6. Error surface: named errors, the file, and the problem

Every write route is token-gated like the rest of the server (a request without
the token is a `404`) and method-checked (a non-`POST` answer is a `405` whose
body names the method). The bodies are JSON with an `error` slug and a
`message` naming the problem — and the book or lock file where there is one:

| status | `error` | when |
|---|---|---|
| 403 | `read-only` | the process was started read-only; the message says how to start it writable |
| 405 | — | not `POST`; the body says which method the route needs |
| 409 | `locked` | the book is locked; `lock` carries the `.LCK` path |
| 409 | `no-accounts` | the self-test found no two accounts to move money between |
| 500 | `backup-failed` | the engine refused to save because it could not make the backup; the book was not modified |
| 500 | `save-failed` | the engine reported a write error; `engine_error` is the `QofBackendError`, `path_exists` says whether the book is still on disk |
| 500 | `write-lost` | the engine claimed success but the book path is missing or empty; `backup` names the pre-save copy |

Messages are the engine's message when the backend set one (the lock and
write messages it already produces), otherwise they are built to say what
failed and on which path. The server never turns a failure into a `200`.

### 7. What stays read-only

This ADR decides only the write path listed above. The following remain
read-only and are not implied by `--writable`:

* schema or book-format upgrades (the SQL `safe_save` path, XML version
  conversion);
* price databases and online quotes;
* imports (QIF/OFX/CSV), exports, and "Save As";
* business documents (invoices, bills, customers, vendors) as editable objects;
* preferences and gsettings writes;
* book creation, opening and switching (a process still serves the one book it
  was given);
* the report-spec store, which already writes its own JSON file under
  `GNC_WEBUI_REPORTS` and is unaffected by this ADR.

The register UI, transaction editing endpoints, validation, undo and autosave
belong to the next stage; this ADR deliberately ships them no API.

### 8. The user's rollback story

Before the save, `<book>.<YYYYMMDDHHMMSS>.gnucash` is a complete copy of the
book as it was (a hard link or a byte copy — the same bytes either way). After
the save the route returns that exact path and size. To roll back: stop the
server (releasing `.LCK`), restore the backup over the book path, and start
again. Old backups are pruned by the engine's retention preference on later
saves, not by this server, so the rollback copy survives as long as the
preference says.

## Consequences

* Read-only users see no change at all: same open mode, no lock file, same
  bytes on every existing route, and the save route answers `403` with a
  message that names the missing flag.
* Writable users get exactly the desktop's guarantees — one lock, one
  timestamped backup, a save that can fail — and an HTTP surface that names
  what happened instead of hiding it.
* A crash during a save can still leave the book missing at its path with the
  backup present. The route makes that failure loud (`write-lost`), but it
  cannot prevent it; only an atomic engine save can, and that is deferred.
* A writable server holds the lock for its whole lifetime, including when
  nobody is editing. That is the desktop's behaviour too; the alternative
  (lock at first save) would leave a window where two processes both believe
  they may write.
* The token remains the only credential. A page that can read the token can
  also save; writable mode is therefore only as safe as the browser profile
  that holds the URL. Loopback binding keeps the socket off the network.
* `--break-lock` is a loaded gun with a safety, not a convenience.

## Deferred

* An atomic save in the engine (temp + fsync + rename), and whether the backup
  should be made by the same atomic step.
* Detecting that the book changed on disk while this process held it (mtime,
  size, inode) and refusing or merging — the lock is the whole story today.
* Writable SQL books: transactional save is real, but there is no file lock and
  no file backup, so the guarantees above do not transfer unchanged.
* Autosave, save-on-exit, dirty-state reporting and multi-tab coordination.
* Per-transaction validation, undo, and the register API itself; the
  `/api/book/selftest` route is removed when that API lands.
* CSRF hardening beyond the token (e.g. a required custom header), and whether
  report content can leak the token through a `Referer` to an external host.
