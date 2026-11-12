// Headless browser test for creating a transaction (ADR 0015).
//
//   node web/test/register-create-ui.mjs <url>
//
// <url> is a gnc-webd started with --writable on a copy of the demo book.
// The FIXTURE below declares exactly that, and a mismatch is refused before a
// check runs.  web/run-ui-test.sh runs this as its own phase, with its own
// copy of the book, so its creations cannot leak into the edit phase or into
// phase 2's read-only book.  It checks:
//
//   * a writable register offers New Transaction, and the form it opens says
//     the transaction is not saved until Save;
//   * the form starts with two lines, the first pointing at the register's own
//     account, and every account option carries its subtree total;
//   * the balance verdict is the server's: the page shows a running sum, the
//     verdict flips to "does not balance" when an amount changes, Create is
//     refused, and balancing again re-enables it;
//   * Enter adds a line, Tab moves to the next field, Escape on a dirty form
//     asks first and keeps the form when the discard is cancelled;
//   * Create puts the transaction in the book and leaves it unsaved: the new
//     row appears, the row and page dirty state are set, and the status line
//     says it is not saved yet;
//   * Save writes the book and clears the dirty state; a reload shows the
//     transaction with its splits;
//   * no console errors or unexpected failed requests.
//
// Needs playwright-core on NODE_PATH, exactly like ui-test.mjs.

import { createRequire } from 'node:module';
import { requireFixture } from './fixture.mjs';

const { chromium } = createRequire(import.meta.url)('playwright-core');

export const FIXTURE = { book: 'demo', writable: true };

const url = process.argv[2];
if (!url) {
    console.error('usage: node register-create-ui.mjs <url>');
    process.exit(2);
}

await requireFixture('register-create-ui.mjs', FIXTURE, url);

const browser = await chromium.launch({
    ...(process.env.PW_CHANNEL ? { channel: process.env.PW_CHANNEL } : {}),
    headless: true,
    args: process.env.PW_NO_SANDBOX ? ['--no-sandbox'] : [],
});
const page = await browser.newPage({ viewport: { width: 1280, height: 900 } });

const problems = [];
page.on('console', (m) => {
    if (m.type() === 'error') problems.push(`console: ${m.text()}`);
});
page.on('requestfailed', (r) => {
    if (!/favicon\.ico$/.test(r.url()))
        problems.push(`request failed: ${r.url()} (${r.failure()?.errorText})`);
});
page.on('response', (r) => {
    if (r.status() >= 400 && !/favicon\.ico$/.test(r.url()))
        problems.push(`http ${r.status()}: ${r.url()}`);
});

// The discard guard is a window.confirm; each check sets the answer it wants.
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

const stripIsolates = (text) => (text || '').replace(/[\u2066\u2069]/g, '');

const accounts = await getJson('api/accounts');
const options = await getJson('api/accounts/options');
const leaves = [...descendants(accounts)];
let populated = null;
for (const [path, account] of leaves) {
    if (account.children.length > 0) continue;
    const register = await getJson(`api/register/${account.guid}`);
    if (register.splits.length > 0 &&
        (!populated || register.splits.length > populated.register.splits.length))
        populated = { path, account, register };
}
check('the demo book has an account with rows to create into',
      Boolean(populated) && populated.register.splits.length >= 3,
      populated ? `${populated.path}: ${populated.register.splits.length} rows`
                : 'none');

const chosen = options.accounts.find((a) => a.guid === populated.account.guid);
const other = options.accounts.find((a) =>
    a.guid !== populated.account.guid && a.commodity === chosen.commodity);
check('the creation form has a second account in the same commodity',
      Boolean(other), other ? other.full_name : 'none');
const description = 'browser created transaction';
const amount = '10.00';

await page.goto(url);
await page.waitForFunction(() => document.querySelectorAll('#tree li').length > 0,
                           null, { timeout: 30000 });

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
    const reread = page.waitForResponse(
        (response) => response.url().includes(`/api/register/${account.guid}`) &&
                     response.status() === 200,
        { timeout: 30000 });
    await page.locator(`#tree button.account-name[data-guid="${account.guid}"]`).click();
    await reread;
    await page.waitForFunction(
        (expected) => {
            const section = document.getElementById('register');
            if (section.hidden) return false;
            return document.querySelectorAll(
                '#register-grid > tbody > tr:not(.detail-band)').length === expected;
        }, rows, { timeout: 30000 });
}

await openAccount(populated.account, populated.register.splits.length);

const COLUMN = { description: 2, memo: 3, amount: 6 };
const rowCount = () => page.locator(
    '#register-grid > tbody > tr:not(.detail-band)').count();

/* ------------------------------------------------------------- the entry */

const entry = await page.evaluate(() => {
    const button = document.getElementById('register-new');
    return {
        visible: button ? button.offsetParent !== null : false,
        text: button ? button.textContent.trim() : '',
        dialogOpen: document.getElementById('transaction-dialog')?.open || false,
    };
});
check('a writable register offers New Transaction', entry.visible &&
      entry.text === 'New Transaction' && !entry.dialogOpen, entry.text);

await page.locator('#register-new').click();
await page.waitForFunction(() => document.getElementById('transaction-dialog')?.open,
                           null, { timeout: 15000 });

const opened = await page.evaluate(() => {
    const date = document.getElementById('transaction-date').value;
    const lines = [...document.querySelectorAll('#transaction-lines tbody tr')];
    const selects = lines.map((tr) => tr.querySelector('.transaction-account'));
    return {
        title: document.getElementById('transaction-dialog-title').textContent.trim(),
        note: document.getElementById('transaction-save-note').textContent,
        date,
        lineCount: lines.length,
        firstAccount: selects[0] ? selects[0].value : '',
        secondAccount: selects[1] ? selects[1].value : '',
        createDisabled: document.getElementById('transaction-create').disabled,
        verdict: document.getElementById('transaction-verdict').textContent,
    };
});
check('the form says creating does not save the book',
      /unsaved/i.test(opened.note) && /Save/.test(opened.note),
      opened.note.trim());
check('the form starts with two split lines and today\'s date',
      opened.lineCount === 2 && /^\d{4}-\d{2}-\d{2}$/.test(opened.date),
      `${opened.lineCount} lines, date ${opened.date}`);
check('the first line points at the register\'s own account',
      opened.firstAccount === populated.account.guid, opened.firstAccount);
check('Create starts disabled while the amounts are missing',
      opened.createDisabled);

const optionText = await page.evaluate((guid) => {
    const select = document.querySelector('#transaction-lines tbody tr .transaction-account');
    const option = [...select.options].find((candidate) => candidate.value === guid);
    return option ? option.textContent : null;
}, populated.account.guid);
check('the account picker shows the subtree total',
      optionText !== null &&
      stripIsolates(optionText).includes(stripIsolates(chosen.total)) &&
      optionText.startsWith(chosen.full_name),
      optionText || 'no option');

/* -------------------------------------------------- amounts and the verdict */

async function setLine(index, values) {
    const tr = page.locator('#transaction-lines tbody tr').nth(index);
    if (values.account !== undefined)
        await tr.locator('.transaction-account').selectOption(values.account);
    if (values.amount !== undefined)
        await tr.locator('.transaction-amount').fill(values.amount);
    if (values.memo !== undefined)
        await tr.locator('.transaction-memo').fill(values.memo);
}

await page.locator('#transaction-number').fill('BROWSER-1');
await page.locator('#transaction-description').fill(description);
await setLine(0, { amount, memo: 'browser memo one' });
await setLine(1, { account: other.guid, amount: `-${amount}`,
                   memo: 'browser memo two' });

const waitBalanced = () => page.waitForFunction(() =>
    /Balanced:/.test(document.getElementById('transaction-verdict').textContent),
    null, { timeout: 15000 });
await waitBalanced();
const balanced = await page.evaluate(() => ({
    verdict: document.getElementById('transaction-verdict').textContent,
    createDisabled: document.getElementById('transaction-create').disabled,
    createReason: document.getElementById('transaction-create').title,
}));
check('the server\'s running sum and balanced verdict are shown',
      /running sum/i.test(balanced.verdict) && /debits/i.test(balanced.verdict),
      balanced.verdict);
check('Create is enabled when the transaction balances',
      !balanced.createDisabled);

// Change one amount: the verdict must flip and creation must be refused.  The
// form never adds the amounts itself; this text is the server's answer.
await setLine(1, { amount: '-5.00' });
await page.waitForFunction(() =>
    /Does not balance/.test(document.getElementById('transaction-verdict').textContent),
    null, { timeout: 15000 });
const unbalanced = await page.evaluate(() => ({
    verdict: document.getElementById('transaction-verdict').textContent,
    createDisabled: document.getElementById('transaction-create').disabled,
    createReason: document.getElementById('transaction-create').title,
    messageHidden: document.getElementById('transaction-message').hidden,
}));
check('an unbalanced transaction is refused with the server\'s reason',
      /off by/i.test(unbalanced.verdict) && unbalanced.createDisabled &&
      /does not balance/i.test(unbalanced.createReason),
      `${unbalanced.verdict} | ${unbalanced.createReason}`);
check('the refused state creates nothing (no message from a request)',
      unbalanced.messageHidden);

await setLine(1, { amount: `-${amount}` });
await waitBalanced();
check('balancing again re-enables Create',
      !(await page.locator('#transaction-create').isDisabled()));

/* ----------------------------------------------------------- keyboard form */

const linesBeforeEnter = await page.locator('#transaction-lines tbody tr').count();
await page.locator('#transaction-lines tbody tr').nth(1)
    .locator('.transaction-memo').focus();
await page.keyboard.press('Enter');
await page.waitForFunction((expected) =>
    document.querySelectorAll('#transaction-lines tbody tr').length === expected,
    linesBeforeEnter + 1, { timeout: 15000 });
check('Enter in a field adds a split line',
      (await page.locator('#transaction-lines tbody tr').count()) ===
          linesBeforeEnter + 1);
await page.locator('#transaction-lines tbody tr').last()
    .locator('.transaction-remove').click();
await page.waitForFunction((expected) =>
    document.querySelectorAll('#transaction-lines tbody tr').length === expected,
    linesBeforeEnter, { timeout: 15000 });

await page.locator('#transaction-lines tbody tr').nth(0)
    .locator('.transaction-amount').focus();
await page.keyboard.press('Tab');
const afterTab = await page.evaluate(() => ({
    className: document.activeElement.className,
    label: document.activeElement.getAttribute('aria-label'),
}));
check('Tab moves to the next field in the form',
      afterTab.className === 'transaction-memo' &&
      /Memo for line 1/.test(afterTab.label || ''),
      `${afterTab.className} ${afterTab.label}`);

// Escape on a dirty form asks first; dismissing the question keeps the form
// and the typed values.
const dialogsBefore = dialogs.length;
dialogAnswer = 'dismiss';
await page.keyboard.press('Escape');
await page.waitForTimeout(200);
const afterEscape = await page.evaluate(() => ({
    open: document.getElementById('transaction-dialog').open,
    description: document.getElementById('transaction-description').value,
}));
const asked = dialogs.slice(dialogsBefore);
check('Escape on a dirty form asks before discarding',
      asked.length === 1 && /Discard/.test(asked[0].message),
      asked.length ? asked[0].message : 'no dialog');
check('cancelling the discard keeps the dirty form and its values',
      afterEscape.open && afterEscape.description === description,
      `open=${afterEscape.open}`);

/* --------------------------------------------------------------- create */

const rowsBefore = await rowCount();
const createWait = page.waitForResponse((response) =>
    response.url().endsWith('/api/transaction') &&
    response.request().method() === 'POST');
await page.locator('#transaction-create').click();
const createHTTP = await createWait;
const created = await createHTTP.json();
check('Create posts to /api/transaction and succeeds',
      createHTTP.status() === 200 && typeof created.guid === 'string' &&
      created.balanced === true,
      `${createHTTP.status()} ${JSON.stringify(created).slice(0, 100)}`);
check('the create response carries the new register rows and a dirty book',
      Array.isArray(created.rows) && created.rows.length === 2 &&
      created.book_dirty === true,
      `${created.rows?.length} rows, dirty=${created.book_dirty}`);

const ownSplit = created.splits.find(
    (split) => split.account_guid === populated.account.guid);
check('the created transaction has both splits with their memos',
      created.splits.length === 2 &&
      created.splits.some((split) => split.memo === 'browser memo one') &&
      created.splits.some((split) => split.memo === 'browser memo two'),
      created.splits.map((split) => split.memo).join(', '));

await page.waitForFunction((expected) =>
    document.querySelectorAll('#register-grid > tbody > tr:not(.detail-band)').length
        === expected, rowsBefore + 1, { timeout: 15000 });
const afterCreate = await page.evaluate((guid) => {
    const tr = document.querySelector(
        `#register-grid > tbody > tr[data-guid="${guid}"]`);
    return {
        dialogOpen: document.getElementById('transaction-dialog').open,
        row: tr ? {
            description: tr.children[2].textContent,
            memo: tr.children[3].textContent,
            amount: tr.children[6].textContent,
            marked: tr.classList.contains('edited'),
        } : null,
        dirtyHidden: document.getElementById('register-dirty-state').hidden,
        dirtyText: document.getElementById('register-dirty-state').textContent.trim(),
        saveDisabled: document.getElementById('register-save').disabled,
        status: document.getElementById('register-edit-status').textContent,
    };
}, ownSplit.guid);
check('the created row appears in the register',
      afterCreate.row !== null && afterCreate.row.description === description &&
      afterCreate.row.memo === 'browser memo one' &&
      afterCreate.row.amount.includes(amount),
      JSON.stringify(afterCreate.row));
check('creating leaves the register dirty and not saved',
      afterCreate.row.marked && !afterCreate.dirtyHidden &&
      afterCreate.dirtyText === 'Unsaved changes' &&
      !afterCreate.saveDisabled &&
      /not saved yet/i.test(afterCreate.status),
      `${afterCreate.dirtyText}; ${afterCreate.status}`);
check('the form closes after the transaction is created', !afterCreate.dialogOpen);

/* ----------------------------------------------------------------- save */

const saveWait = page.waitForResponse((response) =>
    response.url().includes('/api/book/save') &&
    response.request().method() === 'POST');
await page.locator('#register-save').click();
const saveHTTP = await saveWait;
const saved = await saveHTTP.json();
await page.waitForFunction(() =>
    document.getElementById('register-dirty-state').hidden,
    null, { timeout: 15000 });
check('Save writes the created transaction',
      saveHTTP.status() === 200 && saved.saved === true && saved.changed === true &&
      typeof saved.backup === 'string',
      `${saveHTTP.status()} changed=${saved.changed}`);
const afterSave = await page.evaluate(() => ({
    marked: document.querySelectorAll('#register-grid > tbody > tr.edited').length,
    status: document.getElementById('register-edit-status').textContent,
}));
check('Save clears the new row\'s dirty marker',
      afterSave.marked === 0, `${afterSave.marked} marked`);

/* --------------------------------------------------------------- reload */

await page.reload();
await page.waitForFunction(() => document.querySelectorAll('#tree li').length > 0,
                           null, { timeout: 30000 });
await openAccount(populated.account, rowsBefore + 1);
const reloaded = await page.evaluate((guid) => {
    const tr = document.querySelector(
        `#register-grid > tbody > tr[data-guid="${guid}"]`);
    return {
        present: tr !== null,
        description: tr ? tr.children[2].textContent : '',
        memo: tr ? tr.children[3].textContent : '',
        amount: tr ? tr.children[6].textContent : '',
    };
}, ownSplit.guid);
check('after a reload the saved transaction is in the register',
      reloaded.present && reloaded.description === description &&
      reloaded.memo === 'browser memo one' &&
      reloaded.amount.includes(amount),
      JSON.stringify(reloaded));

/* --------------------------------------------------------------- the end */

for (const problem of problems) console.log(`page problem: ${problem}`);
check('no console errors or unexpected failed requests', problems.length === 0,
      `${problems.length} problems`);

if (process.env.UI_SCREENSHOT)
    await page.screenshot({ path: process.env.UI_SCREENSHOT });

await browser.close();
process.exit(failures === 0 ? 0 : 1);
