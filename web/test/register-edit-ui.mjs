// Headless browser test for the *editable* register (ADR 0014).
//
//   node web/test/register-edit-ui.mjs <url>
//
// <url> is a gnc-webd started with --writable on a copy of the demo book.
// The FIXTURE below declares exactly that (the demo book, writable), and a
// mismatch is refused before a check runs.  web/run-ui-test.sh runs this as
// its third phase.  The test drives a real browser and checks:
//
//   * a writable session shows the Save control, says nothing about being
//     read-only, and makes exactly the number, description and memo cells
//     editable (never the amount or the balance);
//   * clicking a cell edits it, Escape cancels without writing anything, and
//     blurring an editor cancels too;
//   * keyboard-only editing works end to end: Enter, type, Tab (commits and
//     opens the next cell), type, Enter;
//   * an edited row carries a dirty marker and the page says "Unsaved
//     changes"; Save is enabled, and File > Save says why it is disabled when
//     the book is clean;
//   * Save posts to /api/book/save, reports the path, the bytes and the
//     backup, and clears the markers;
//   * a reload of the page still shows the edit (it reached the file);
//   * a bad value is refused by name, the cell keeps the server's text, and
//     the row is not marked dirty;
//   * unsaved changes refuse in-page navigation until they are saved:
//     Cancel stays on the account, OK saves and only then opens the other
//     one; a synthetic beforeunload is prevented while dirty and not when
//     clean;
//   * no console errors or unexpected failed requests.
//
// Needs playwright-core on NODE_PATH, exactly like ui-test.mjs.

import { createRequire } from 'node:module';
import { requireFixture } from './fixture.mjs';

const { chromium } = createRequire(import.meta.url)('playwright-core');

export const FIXTURE = { book: 'demo', writable: true };

const url = process.argv[2];
if (!url) {
    console.error('usage: node register-edit-ui.mjs <url>');
    process.exit(2);
}

// Refuse a server that is not the demo book in a writable session, before a
// single check: this phase saves, so a read-only server would fail every edit
// check for the wrong reason.
await requireFixture('register-edit-ui.mjs', FIXTURE, url);

const browser = await chromium.launch({
    ...(process.env.PW_CHANNEL ? { channel: process.env.PW_CHANNEL } : {}),
    headless: true,
    args: process.env.PW_NO_SANDBOX ? ['--no-sandbox'] : [],
});
const page = await browser.newPage({ viewport: { width: 1280, height: 900 } });

const problems = [];
let expectingRefusal = false;
let expectingSaveRefusal = false;
page.on('console', (m) => {
    // The bad-value check makes one deliberate 400, and Chromium logs every
    // failed response to the console.  Only that message is expected.
    if (expectingRefusal && /status of 400/.test(m.text())) return;
    // The refused-save check makes one deliberate 409 the same way.
    if (expectingSaveRefusal && /status of 409/.test(m.text())) return;
    if (m.type() === 'error') problems.push(`console: ${m.text()}`);
});
page.on('requestfailed', (r) => {
    if (!/favicon\.ico$/.test(r.url()))
        problems.push(`request failed: ${r.url()} (${r.failure()?.errorText})`);
});
page.on('response', (r) => {
    // The bad-value check makes one deliberate 400; every other error status
    // is a problem.
    if (r.status() >= 400 && !/favicon\.ico$/.test(r.url()) &&
        !(expectingRefusal && r.url().includes('/api/split/')) &&
        !(expectingSaveRefusal && r.url().includes('/api/book/save')))
        problems.push(`http ${r.status()}: ${r.url()}`);
});

// Dialogs: the unsaved-changes guard uses window.confirm, and Chromium may
// ask before unload.  Each test sets the answer it wants; the default is to
// dismiss, so an unexpected dialog cannot silently navigate.
let dialogAnswer = 'dismiss';
const dialogs = [];
page.on('dialog', async (dialog) => {
    dialogs.push({ type: dialog.type(), message: dialog.message() });
    if (dialogAnswer === 'accept') await dialog.accept();
    else await dialog.dismiss();
});

let failures = 0;
function check(name, ok, detail = '') {
    console.log(`${ok ? 'ok  ' : 'FAIL'} ${name}${detail ? ` — ${detail}` : ''}`);
    if (!ok) failures++;
}

async function getJson(path) {
    const response = await fetch(new URL(path, url));
    if (!response.ok)
        throw new Error(`${path} answered ${response.status}: ${await response.text()}`);
    return response.json();
}

function* descendants(node, path = '') {
    for (const child of node.children || []) {
        const childPath = path ? `${path}:${child.name}` : child.name;
        yield [childPath, child];
        yield* descendants(child, childPath);
    }
}

// The account with the most rows, chosen from the API so the test follows the
// book the runner generated, plus any other leaf to navigate to.
const accounts = await getJson('api/accounts');
const leaves = [...descendants(accounts)];
let populated = null;
let other = null;
for (const [path, account] of leaves) {
    if (account.children.length > 0)
        continue;
    const register = await getJson(`api/register/${account.guid}`);
    if (!populated || register.splits.length > populated.register.splits.length)
        populated = { path, account, register };
}
other = leaves.find(([, account]) =>
    account.children.length === 0 && account.guid !== populated.account.guid) || null;
check('the demo book has an account with rows to edit',
      Boolean(populated) && populated.register.splits.length >= 3,
      populated ? `${populated.path}: ${populated.register.splits.length} rows`
                : 'none');
check('the demo book has a second account to navigate to', Boolean(other),
      other ? other[0] : 'none');
const otherRegister = other ? await getJson(`api/register/${other[1].guid}`) : null;
// The split the keyboard edit works on: the first row of the register as the
// server orders it before any edit.
const editedGuid = populated.register.splits[0].guid;

await page.goto(url);
await page.waitForFunction(() => document.querySelectorAll('#tree li').length > 0,
                           null, { timeout: 30000 });

// Stage 1: the tree is the pinned Accounts tab and a register is its own tab.
// A tree click therefore needs the Accounts tab on screen first.  The page
// also applies the URL asynchronously, so a navigation is settled first: a
// click must not race the route that is about to show the register tab.
const waitForRoute = () => page.waitForFunction(
    () => document.documentElement.dataset.routeReady === '1',
    null, { timeout: 30000 });

async function showAccountsTab() {
    await waitForRoute();
    if (await page.evaluate(() => document.getElementById('register').hidden))
        return;
    await page.locator('#tab-accounts').click();
    await page.waitForFunction(() => document.getElementById('register').hidden,
                               null, { timeout: 15000 });
}

async function openAccount(account, rows) {
    await showAccountsTab();
    // The register is unhidden before its re-read finishes, and the previous
    // grid can already have the expected row count; clicking a stale row right
    // then loses the click when the re-read replaces it.  Wait for the read
    // the click starts before looking at the rows.
    const reread = page.waitForResponse(
        (response) => response.url().includes(
            `/api/register/${account.guid}`) && response.status() === 200,
        { timeout: 30000 });
    await page.locator(`#tree button.account-name[data-guid="${account.guid}"]`).click();
    await reread;
    await page.waitForFunction(
        (expected) => {
            const section = document.getElementById('register');
            if (section.hidden) return false;
            return document.querySelectorAll('#register-grid > tbody > tr:not(.detail-band)').length === expected;
        }, rows, { timeout: 30000 });
}

async function cellText(row, column) {
    return page.evaluate(([r, c]) => {
        const tr = document.querySelectorAll('#register-grid > tbody > tr:not(.detail-band)')[r];
        return tr ? tr.children[c].textContent : null;
    }, [row, column]);
}

// A row is found by the split's GUID, not by its index: the engine's register
// order can change when a sort-key field (num, description) is edited, and the
// page re-reads the rows after every edit.
async function rowText(guid) {
    return page.evaluate((g) => {
        const tr = document.querySelector(
            `#register-grid > tbody > tr[data-guid="${g}"]`);
        if (!tr) return null;
        return {
            num: tr.children[1].textContent,
            description: tr.children[2].textContent,
            memo: tr.children[3].textContent,
        };
    }, guid);
}

const COLUMN = { num: 1, description: 2, memo: 3, amount: 6, balance: 7 };

async function clickCell(row, column) {
    await page.locator('#register-grid > tbody > tr:not(.detail-band)').nth(row)
        .locator('td').nth(column).click();
}

async function editorOpen() {
    return (await page.locator('#register-grid .cell-editor').count()) === 1;
}

// Type into a cell and commit it with Enter (the keyboard path the page
// documents), waiting for the editor to open and then to go away.
async function typeInto(row, column, text) {
    await clickCell(row, column);
    await page.waitForFunction(
        () => document.querySelectorAll('#register-grid .cell-editor').length === 1,
        null, { timeout: 15000 });
    await page.keyboard.type(text);
    await page.keyboard.press('Enter');
    await page.waitForFunction(
        () => document.querySelectorAll('#register-grid .cell-editor').length === 0,
        null, { timeout: 15000 });
}

/* ------------------------------------------- what a writable session offers */

await openAccount(populated.account, populated.register.splits.length);

const affordance = await page.evaluate(() => {
    const firstRow = document.querySelector('#register-grid > tbody > tr:not(.detail-band)');
    return {
        actionsHidden: document.getElementById('register-actions').hidden,
        readonlyHidden: document.getElementById('register-readonly').hidden,
        readonlyText: document.getElementById('register-readonly').textContent.trim(),
        saveDisabled: document.getElementById('register-save').disabled,
        fields: [...firstRow.children].map((cell) => cell.dataset.field || null),
        editableCells: [...document.querySelectorAll('#register-grid td.editable')].length,
        rowReadonly: firstRow.getAttribute('aria-readonly'),
        amountEditable: firstRow.children[6].classList.contains('editable'),
        balanceEditable: firstRow.children[7].classList.contains('editable'),
        rows: document.querySelectorAll('#register-grid > tbody > tr:not(.detail-band)').length,
    };
});
check('a writable session shows the Save control and no read-only note',
      !affordance.actionsHidden && affordance.readonlyHidden,
      `actions hidden=${affordance.actionsHidden}, note hidden=${affordance.readonlyHidden}`);
check('exactly the number, description and memo cells are editable',
      JSON.stringify(affordance.fields) ===
        JSON.stringify([null, 'num', 'description', 'memo', null, null, null, null]) &&
      affordance.editableCells === affordance.rows * 3,
      `${affordance.editableCells} editable cells in ${affordance.rows} rows`);
check('the amount and balance cells are not editable',
      !affordance.amountEditable && !affordance.balanceEditable);
check('the rows tell a screen reader that they are editable',
      affordance.rowReadonly === 'false', `aria-readonly=${affordance.rowReadonly}`);
check('Save starts disabled because the book is clean', affordance.saveDisabled);

// File > Save before any edit: disabled, and the reason names the state.
await page.locator('#menubar .menu[data-menu="file"] > .menu-title').click();
const cleanSave = await page.evaluate(() => {
    const item = document.querySelector('#menubar .menu[data-menu="file"] [data-item="save"]');
    return { disabled: item.getAttribute('aria-disabled'),
             reason: item.dataset.reason || '' };
});
await page.keyboard.press('Escape');
check('File > Save is disabled before an edit and says why',
      cleanSave.disabled === 'true' && /Nothing to save/.test(cleanSave.reason),
      cleanSave.reason);

/* ----------------------------------------------------- click, Escape, blur */

const original = {
    num: await cellText(0, COLUMN.num),
    description: await cellText(0, COLUMN.description),
    memo: await cellText(0, COLUMN.memo),
};

await clickCell(0, COLUMN.amount);
await page.waitForFunction(
    () => /not editable in this stage/.test(
        document.getElementById('register-edit-status').textContent),
    null, { timeout: 15000 });
const amountClick = await page.evaluate(() => ({
    editor: document.querySelectorAll('#register-grid .cell-editor').length,
    status: document.getElementById('register-edit-status').textContent,
}));
check('clicking the amount says why it is not editable, and opens no editor',
      amountClick.editor === 0 && /rebalances/.test(amountClick.status),
      amountClick.status);

await clickCell(0, COLUMN.description);
await page.waitForFunction(() => document.querySelectorAll('#register-grid .cell-editor').length === 1,
                           null, { timeout: 15000 });
check('clicking an editable cell opens an editor inside it', await editorOpen());

await page.keyboard.type('discarded text');
await page.keyboard.press('Escape');
await page.waitForFunction(() => document.querySelectorAll('#register-grid .cell-editor').length === 0,
                           null, { timeout: 15000 });
const afterEscape = await page.evaluate(() => ({
    text: document.querySelector('#register-grid > tbody > tr:not(.detail-band) td.description').textContent,
    marked: document.querySelectorAll('#register-grid > tbody > tr.edited').length,
    dirtyHidden: document.getElementById('register-dirty-state').hidden,
    saveDisabled: document.getElementById('register-save').disabled,
}));
check('Escape cancels the edit: the old text stays and nothing is dirty',
      afterEscape.text === original.description && afterEscape.marked === 0 &&
      afterEscape.dirtyHidden && afterEscape.saveDisabled,
      `cell=${JSON.stringify(afterEscape.text)} marked=${afterEscape.marked}`);

// Blurring the editor (clicking a cell that cannot be edited) also cancels
// rather than writing a half-typed value.
await clickCell(0, COLUMN.description);
await page.waitForFunction(() => document.querySelectorAll('#register-grid .cell-editor').length === 1,
                           null, { timeout: 15000 });
await page.keyboard.type('also discarded');
await clickCell(1, 0); // the date cell has no field, so no editor replaces it
await page.waitForFunction(() => document.querySelectorAll('#register-grid .cell-editor').length === 0,
                           null, { timeout: 15000 });
const afterBlur = await page.evaluate(() => ({
    text: document.querySelector('#register-grid > tbody > tr:not(.detail-band) td.description').textContent,
    marked: document.querySelectorAll('#register-grid > tbody > tr.edited').length,
}));
check('leaving the editor without Enter or Tab cancels it too',
      afterBlur.text === original.description && afterBlur.marked === 0,
      `cell=${JSON.stringify(afterBlur.text)} marked=${afterBlur.marked}`);

/* -------------------------------------------------- keyboard-only editing */

// Back to the first row, then never touch the mouse again: Enter edits the
// number, Tab commits and opens the description, Tab again the memo, Enter
// commits and returns to the row.
await page.keyboard.press('Home');
check('Home keeps the selected row focused for a keyboard edit',
      await page.evaluate(() => document.activeElement.getAttribute('role') === 'row'));

await page.keyboard.press('Enter');
await page.waitForFunction(() => document.querySelectorAll('#register-grid .cell-editor').length === 1,
                           null, { timeout: 15000 });
const editedLabel = await page.evaluate(() =>
    document.activeElement.getAttribute('aria-label'));
check('Enter opens the editor on the number cell',
      /number/.test(editedLabel || ''), editedLabel);

await page.keyboard.type('KBD-1');
await page.keyboard.press('Tab');
await page.waitForFunction(() =>
    document.querySelectorAll('#register-grid .cell-editor').length === 1 &&
    /description/.test(document.activeElement.getAttribute('aria-label') || ''),
    null, { timeout: 15000 });
check('Tab commits the number and opens the description cell',
      (await rowText(editedGuid)).num === 'KBD-1',
      (await rowText(editedGuid)).num);

await page.keyboard.type('keyboard description');
await page.keyboard.press('Tab');
await page.waitForFunction(() =>
    /memo/.test(document.activeElement.getAttribute('aria-label') || ''),
    null, { timeout: 15000 });
await page.keyboard.type('tab memo');
await page.keyboard.press('Enter');
await page.waitForFunction((g) => {
    const tr = document.querySelector(`#register-grid > tbody > tr[data-guid="${g}"]`);
    return document.querySelectorAll('#register-grid .cell-editor').length === 0 &&
        tr && tr.children[3].textContent === 'tab memo';
}, editedGuid, { timeout: 15000 });

const keyboardRow = await page.evaluate((g) => {
    const tr = document.querySelector(`#register-grid > tbody > tr[data-guid="${g}"]`);
    return {
        num: tr.children[1].textContent,
        description: tr.children[2].textContent,
        memo: tr.children[3].textContent,
        focused: document.activeElement.getAttribute('role'),
    };
}, editedGuid);
check('the three keyboard edits are on the row',
      keyboardRow.num === 'KBD-1' &&
      keyboardRow.description === 'keyboard description' &&
      keyboardRow.memo === 'tab memo',
      JSON.stringify(keyboardRow));
check('Enter commits and hands focus back to the row',
      keyboardRow.focused === 'row', String(keyboardRow.focused));

const fromApi = (await getJson(`api/register/${populated.account.guid}`))
    .splits.find((split) => split.guid === editedGuid);
check('the engine has the keyboard edits, not just the page',
      fromApi.num === 'KBD-1' && fromApi.description === 'keyboard description' &&
      fromApi.memo === 'tab memo',
      `${fromApi.num} / ${fromApi.description} / ${fromApi.memo}`);

/* ------------------------------------------------------------ dirty state */

const dirtyState = await page.evaluate((g) => {
    const tr = document.querySelector(`#register-grid > tbody > tr[data-guid="${g}"]`);
    return {
        marked: document.querySelectorAll('#register-grid > tbody > tr.edited').length,
        dirtyHidden: document.getElementById('register-dirty-state').hidden,
        dirtyText: document.getElementById('register-dirty-state').textContent.trim(),
        saveDisabled: document.getElementById('register-save').disabled,
        title: tr ? tr.title : '',
        markedThisRow: tr ? tr.classList.contains('edited') : false,
    };
}, editedGuid);
check('the edited row carries a dirty marker',
      dirtyState.marked === 1 && dirtyState.markedThisRow &&
      dirtyState.title.includes('not saved'),
      `${dirtyState.marked} marked: ${dirtyState.title}`);
check('the page-level dirty state is shown',
      !dirtyState.dirtyHidden && dirtyState.dirtyText === 'Unsaved changes',
      dirtyState.dirtyText);
check('Save is enabled once there are unsaved changes', !dirtyState.saveDisabled);

await page.locator('#menubar .menu[data-menu="file"] > .menu-title').click();
const enabledSave = await page.evaluate(() => {
    const item = document.querySelector('#menubar .menu[data-menu="file"] [data-item="save"]');
    return { disabled: item.getAttribute('aria-disabled'),
             reason: item.dataset.reason || '' };
});
check('File > Save becomes enabled and carries no reason',
      enabledSave.disabled === 'false' && enabledSave.reason === '',
      `disabled=${enabledSave.disabled}`);

/* -------------------------------------------------------------------- Save */

const saveResponse = page.waitForResponse(
    (response) => response.url().includes('/api/book/save') &&
                 response.request().method() === 'POST');
// The menu opened on its first item; walk down to Save with the arrow key.
for (let step = 0; step < 12; step++) {
    const onSave = await page.evaluate(() =>
        document.activeElement.dataset &&
        document.activeElement.dataset.item === 'save');
    if (onSave) break;
    await page.keyboard.press('ArrowDown');
}
const reachedSave = await page.evaluate(() =>
    document.activeElement.dataset &&
    document.activeElement.dataset.item === 'save');
check('the keyboard reaches File > Save', reachedSave);
await page.keyboard.press('Enter'); // activate File > Save from the keyboard
const saveHTTP = await saveResponse;
const saveBody = await saveHTTP.json();
await page.waitForFunction(() =>
    document.querySelectorAll('#register-grid > tbody > tr.edited').length === 0,
    null, { timeout: 15000 });

check('Save posts to /api/book/save and succeeds',
      saveHTTP.status() === 200 && saveBody.saved === true,
      `${saveHTTP.status()} ${JSON.stringify(saveBody).slice(0, 90)}`);
check('Save reports that it wrote the book and names the backup',
      saveBody.changed === true && typeof saveBody.path === 'string' &&
      saveBody.bytes > 0 && typeof saveBody.backup === 'string',
      `${saveBody.path} (${saveBody.bytes} bytes), backup ${saveBody.backup}`);

const afterSave = await page.evaluate(() => ({
    marked: document.querySelectorAll('#register-grid > tbody > tr.edited').length,
    dirtyHidden: document.getElementById('register-dirty-state').hidden,
    saveDisabled: document.getElementById('register-save').disabled,
    status: document.getElementById('register-edit-status').textContent,
}));
check('Save clears the row markers and the page-level dirty state',
      afterSave.marked === 0 && afterSave.dirtyHidden && afterSave.saveDisabled,
      `${afterSave.marked} marked, status ${JSON.stringify(afterSave.status)}`);
check('Save says where it wrote and what the backup is',
      afterSave.status.includes(saveBody.path) &&
      afterSave.status.includes(saveBody.backup || '\u0000') &&
      afterSave.status.includes(String(saveBody.bytes)),
      afterSave.status);

/* --------------------------------------- a refused save keeps the dirty state */

// The route's 409/500 cases are covered by the shell gate; what this checks is
// the page's half of the contract: a named refusal is shown and the edits are
// still marked unsaved, so the user cannot mistake a failed save for a good one.
// One route stays installed for the rest of the run and refuses exactly one
// save; every other save passes through, so the retry below is a real save.
let refuseNextSave = false;
await page.route('**/api/book/save', (route) => {
    if (!refuseNextSave) {
        route.continue();
        return;
    }
    refuseNextSave = false;
    route.fulfill({
        status: 409,
        contentType: 'application/json',
        body: JSON.stringify({
            error: 'locked',
            message: 'the book is locked by another GnuCash process, so this ' +
                     'server opened it read-only; the lock is ' +
                     '/tmp/refused.gnucash.LCK',
            lock: '/tmp/refused.gnucash.LCK',
        }),
    });
});

await typeInto(2, COLUMN.memo, 'kept through a refused save');
refuseNextSave = true;
expectingSaveRefusal = true;
await page.locator('#register-save').click();
await page.waitForFunction(
    () => /locked/.test(document.getElementById('register-edit-status').textContent),
    null, { timeout: 15000 });
const afterRefusal = await page.evaluate(() => ({
    status: document.getElementById('register-edit-status').textContent,
    isError: document.getElementById('register-edit-status')
        .classList.contains('error'),
    marked: document.querySelectorAll('#register-grid > tbody > tr.edited').length,
    dirtyHidden: document.getElementById('register-dirty-state').hidden,
    saveDisabled: document.getElementById('register-save').disabled,
}));
expectingSaveRefusal = false;
check('a refused save shows the named error and keeps the dirty state',
      /locked/.test(afterRefusal.status) && afterRefusal.isError &&
      afterRefusal.marked === 1 && !afterRefusal.dirtyHidden &&
      !afterRefusal.saveDisabled,
      afterRefusal.status);

// A retry after the refusal goes through and only then clears the state.
// The engine names the backup with a one-second timestamp (ADR 0013 §3), so a
// second dirty save inside the same second cannot make its backup and is
// refused with 500 backup-failed.  Wait out the second: this check is about the
// page's retry, not about that engine limitation (which ADR 0014 records).
await page.waitForTimeout(1100);
const retrySave = page.waitForResponse(
    (response) => response.url().includes('/api/book/save') &&
                 response.request().method() === 'POST');
await page.locator('#register-save').click();
const retryHTTP = await retrySave;
const retryBody = await retryHTTP.text();
await page.waitForFunction(
    () => document.getElementById('register-dirty-state').hidden,
    null, { timeout: 5000 }).catch(() => {});
const afterRetry = await page.evaluate(() => ({
    marked: document.querySelectorAll('#register-grid > tbody > tr.edited').length,
    dirtyHidden: document.getElementById('register-dirty-state').hidden,
    saveDisabled: document.getElementById('register-save').disabled,
    status: document.getElementById('register-edit-status').textContent,
}));
check('a retry after the refusal saves and clears the markers',
      retryHTTP.status() === 200 && afterRetry.dirtyHidden &&
      afterRetry.marked === 0 && afterRetry.saveDisabled,
      `retry ${retryHTTP.status()} ${retryBody.slice(0, 120)}; ` +
      `dirty hidden=${afterRetry.dirtyHidden} status=${afterRetry.status}`);

/* ----------------------------------------------------------- after a reload */

await page.reload();
await page.waitForFunction(() => document.querySelectorAll('#tree li').length > 0,
                           null, { timeout: 30000 });
await openAccount(populated.account, populated.register.splits.length);

const reloaded = await rowText(editedGuid);
const reloadedState = await page.evaluate(() => ({
    marked: document.querySelectorAll('#register-grid > tbody > tr.edited').length,
    saveDisabled: document.getElementById('register-save').disabled,
}));
check('after a reload the saved edit is still in the register',
      reloaded !== null && reloaded.num === 'KBD-1' &&
      reloaded.description === 'keyboard description' &&
      reloaded.memo === 'tab memo',
      JSON.stringify(reloaded));
check('the reloaded page starts clean',
      reloadedState.marked === 0 && reloadedState.saveDisabled,
      `marked=${reloadedState.marked}`);

/* ---------------------------------------------------- a bad value is a 400 */

const beforeBad = await cellText(2, COLUMN.description);
expectingRefusal = true;
await clickCell(2, COLUMN.description);
await page.waitForFunction(() => document.querySelectorAll('#register-grid .cell-editor').length === 1,
                           null, { timeout: 15000 });
await page.locator('#register-grid .cell-editor').fill('a'.repeat(2049));
await page.keyboard.press('Enter');
await page.waitForFunction(() =>
    /too-long/.test(document.getElementById('register-edit-status').textContent),
    null, { timeout: 15000 });
expectingRefusal = false;
const afterBad = await page.evaluate(() => ({
    text: document.querySelectorAll('#register-grid > tbody > tr:not(.detail-band)')[2]
        .children[2].textContent,
    marked: document.querySelectorAll('#register-grid > tbody > tr.edited').length,
    status: document.getElementById('register-edit-status').textContent,
    isError: document.getElementById('register-edit-status')
        .classList.contains('error'),
}));
check('a bad value is refused by name and the cell keeps the server text',
      afterBad.text === beforeBad && /too-long/.test(afterBad.status) &&
      afterBad.isError,
      `cell=${JSON.stringify(afterBad.text)} status=${afterBad.status}`);
check('a refused edit does not mark the row dirty', afterBad.marked === 0);

/* ------------------------------------- unsaved changes and in-page navigation */

// Make one more edit and leave it unsaved.
await clickCell(2, COLUMN.memo);
await page.waitForFunction(() => document.querySelectorAll('#register-grid .cell-editor').length === 1,
                           null, { timeout: 15000 });
await page.keyboard.type('kept until saved');
await page.keyboard.press('Enter');
await page.waitForFunction(() =>
    document.querySelectorAll('#register-grid > tbody > tr.edited').length === 1,
    null, { timeout: 15000 });

// A synthetic beforeunload runs the page's own listener; Chromium's real
// unload dialog needs a user gesture and cannot be inspected headlessly.
const unload = await page.evaluate(() => {
    const event = new Event('beforeunload', { cancelable: true });
    window.dispatchEvent(event);
    return event.defaultPrevented;
});
check('a dirty page prevents the beforeunload event', unload === true,
      `defaultPrevented=${unload}`);

const accountTitle = await page.evaluate(() =>
    document.getElementById('register-title').textContent);
const dialogsBefore = dialogs.length;
dialogAnswer = 'dismiss';
await showAccountsTab();
await page.locator(`#tree button.account-name[data-guid="${other[1].guid}"]`).click();
await page.waitForTimeout(400);
const afterDismiss = await page.evaluate(() => ({
    title: document.getElementById('register-title').textContent,
    marked: document.querySelectorAll('#register-grid > tbody > tr.edited').length,
    dirtyHidden: document.getElementById('register-dirty-state').hidden,
}));
const dismissed = dialogs.slice(dialogsBefore);
check('opening another account with unsaved changes asks first',
      dismissed.length === 1 && /unsaved changes/.test(dismissed[0].message),
      dismissed.length ? dismissed[0].message.split('\n')[0] : 'no dialog');
check('Cancel refuses the move: the same account stays with its dirty marker',
      afterDismiss.title === accountTitle && afterDismiss.marked === 1 &&
      !afterDismiss.dirtyHidden,
      `title=${JSON.stringify(afterDismiss.title)} marked=${afterDismiss.marked}`);

// Now accept: the guard saves first and only then opens the other account.
dialogAnswer = 'accept';
const guardSave = page.waitForResponse(
    (response) => response.url().includes('/api/book/save') &&
                 response.request().method() === 'POST');
await showAccountsTab();
await page.locator(`#tree button.account-name[data-guid="${other[1].guid}"]`).click();
const guardSaveHTTP = await guardSave;
await page.waitForFunction((expected) =>
    document.getElementById('register-title').textContent === expected,
    otherRegister.full_name, { timeout: 30000 });
const afterAccept = await page.evaluate(() => ({
    title: document.getElementById('register-title').textContent,
    dirtyHidden: document.getElementById('register-dirty-state').hidden,
    marked: document.querySelectorAll('#register-grid > tbody > tr.edited').length,
}));
check('OK saves first and then opens the other account',
      guardSaveHTTP.status() === 200 && afterAccept.dirtyHidden &&
      afterAccept.marked === 0 && afterAccept.title === otherRegister.full_name,
      `save ${guardSaveHTTP.status()}, title=${JSON.stringify(afterAccept.title)}, ` +
      `dirty hidden=${afterAccept.dirtyHidden}`);

const cleanUnload = await page.evaluate(() => {
    const event = new Event('beforeunload', { cancelable: true });
    window.dispatchEvent(event);
    return event.defaultPrevented;
});
check('a clean page lets the beforeunload event through', cleanUnload === false,
      `defaultPrevented=${cleanUnload}`);

/* --------------------------------------------------------------- the end */

for (const problem of problems) console.log(`page problem: ${problem}`);
check('no console errors or unexpected failed requests', problems.length === 0,
      `${problems.length} problems`);

if (process.env.UI_SCREENSHOT)
    await page.screenshot({ path: process.env.UI_SCREENSHOT });

await browser.close();
process.exit(failures === 0 ? 0 : 1);
