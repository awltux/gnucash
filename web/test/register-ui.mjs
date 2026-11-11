// Headless browser test for the read-only register page.
//
//   node web/test/register-ui.mjs <url>
//
// <url> is a gnc-webd address (with token) whose book has an account with
// enough rows to page through.  web/run-ui-test.sh generates the demo book
// with web/test/make-demo-book.py for this phase.  The test drives a real
// browser and checks the grid, the row model, keyboard navigation, the
// read-only transaction panel, and the two menu actions this stage enables.
//
// Needs playwright-core on NODE_PATH, exactly like ui-test.mjs.

import { createRequire } from 'node:module';

const { chromium } = createRequire(import.meta.url)('playwright-core');

const url = process.argv[2];
if (!url) {
    console.error('usage: node register-ui.mjs <url>');
    process.exit(2);
}

const browser = await chromium.launch({
    ...(process.env.PW_CHANNEL ? { channel: process.env.PW_CHANNEL } : {}),
    headless: true,
    args: process.env.PW_NO_SANDBOX ? ['--no-sandbox'] : [],
});
const page = await browser.newPage({ viewport: { width: 1200, height: 900 } });

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

// Pick the leaf account with the most register rows, and one with none.  The
// account is chosen from the API rather than hard-coded, so the test follows
// whatever book the runner generated.
const accounts = await getJson('api/accounts');
const leaves = [...descendants(accounts)];
let populated = null;
let empty = null;
for (const [path, account] of leaves) {
    if (account.children.length > 0)
        continue;
    const register = await getJson(`api/register/${account.guid}`);
    if (register.splits.length > 0 && (!populated ||
        register.splits.length > populated.register.splits.length)) {
        populated = { path, account, register };
    }
    if (register.splits.length === 0 && !empty)
        empty = { path, account, register };
}
check('the demo book has an account with rows to page through',
      Boolean(populated) && populated.register.splits.length >= 20,
      populated ? `${populated.path}: ${populated.register.splits.length} rows`
                : 'none');
check('the demo book has a deliberately empty account',
      Boolean(empty), empty ? empty.path : 'none');

await page.goto(url);
await page.waitForFunction(() => document.querySelectorAll('#tree li').length > 0,
                           null, { timeout: 30000 });

async function gridState() {
    return page.evaluate(() => {
        const grid = document.getElementById('register-grid');
        const rows = [...grid.querySelectorAll('tbody tr')];
        const selected = rows.findIndex(
            (row) => row.getAttribute('aria-selected') === 'true');
        const active = document.activeElement;
        return {
            hidden: document.getElementById('register').hidden,
            frameHidden: document.getElementById('view').hidden,
            title: document.getElementById('register-title').textContent.trim(),
            countText: document.getElementById('register-count').textContent.trim(),
            balanceText: document.getElementById('register-balance').textContent.trim(),
            rows: rows.length,
            selected,
            selectedCount: rows.filter(
                (row) => row.getAttribute('aria-selected') === 'true').length,
            activeIndex: rows.indexOf(active),
            activeIsRow: Boolean(active) && active.getAttribute('role') === 'row',
            lastBalance: rows.length
                ? rows[rows.length - 1].querySelector('td:last-child').textContent.trim()
                : '',
            outline: selected >= 0
                ? getComputedStyle(rows[selected]).outlineWidth : '',
            role: grid.getAttribute('role'),
            ariaRowcount: grid.getAttribute('aria-rowcount'),
            ariaColcount: grid.getAttribute('aria-colcount'),
            rowRoles: rows.every((row) => row.getAttribute('role') === 'row'),
            cellRoles: rows.every((row) => [...row.children].every(
                (cell) => cell.getAttribute('role') === 'gridcell')),
            firstCellRole: rows.length
                ? rows[0].children[0].getAttribute('role') : '',
            editable: [...document.querySelectorAll(
                '#register input, #register textarea, #register select, ' +
                '#register button')]
                .filter((node) => node.offsetParent !== null)
                .map((node) => node.textContent.trim())
                .filter((text) => /save|edit|new|delete/i.test(text)),
            actionsHidden: document.getElementById('register-actions').hidden,
            readonlyHidden: document.getElementById('register-readonly').hidden,
            readonlyText: document.getElementById('register-readonly').textContent.trim(),
            editableCells: document.querySelectorAll('#register-grid td.editable').length,
            contentEditableCells: document.querySelectorAll(
                '#register-grid [contenteditable="true"]').length,
            cellEditors: document.querySelectorAll('#register-grid .cell-editor').length,
        };
    });
}

async function clickAccount(guid) {
    await page.locator(`#tree button.account-name[data-guid="${guid}"]`).click();
    await page.waitForFunction(
        (expected) => {
            const section = document.getElementById('register');
            if (section.hidden) return false;
            const rows = document.querySelectorAll('#register-grid tbody tr').length;
            return rows === expected;
        }, guid === populated.account.guid
            ? populated.register.splits.length : 0,
        { timeout: 30000 });
}

/* ---------------------------------------------- clicking an account opens it */

await clickAccount(populated.account.guid);
let state = await gridState();
check('clicking an account opens the register in the content area',
      !state.hidden && state.frameHidden, `register hidden=${state.hidden}, ` +
      `report frame hidden=${state.frameHidden}`);
check('the heading is the account\'s full name',
      state.title === populated.register.full_name, state.title);
check('the row count is on screen and matches the JSON',
      state.countText === `${populated.register.row_count} row` +
        (populated.register.row_count === 1 ? '' : 's') &&
      state.rows === populated.register.splits.length,
      `${state.rows} rows, heading says "${state.countText}"`);
check('the last running balance equals the heading balance',
      state.lastBalance === populated.register.balance &&
      state.balanceText === `Balance: ${populated.register.balance}`,
      `last row ${state.lastBalance}, heading ${state.balanceText}`);
check('the grid has honest grid/row/gridcell roles',
      state.role === 'grid' && state.rowRoles && state.cellRoles &&
      state.ariaRowcount === String(state.rows + 1) &&
      state.ariaColcount === '8',
      `role=${state.role} rowcount=${state.ariaRowcount} ` +
      `colcount=${state.ariaColcount}`);
check('exactly one row is selected and it is the focused row',
      state.selectedCount === 1 && state.selected === 0 &&
      state.activeIndex === 0 && state.activeIsRow,
      `selected=${state.selected} active=${state.activeIndex}`);
check('the selected row has a visible focus outline',
      state.outline !== '0px' && state.outline !== '', state.outline);
check('the register is read-only: no visible save or edit control',
      state.editable.length === 0 && state.actionsHidden,
      state.editable.join(', '));
check('the read-only session says why the cells cannot be edited',
      !state.readonlyHidden && /read-only/i.test(state.readonlyText) &&
      /--writable/.test(state.readonlyText),
      state.readonlyText);
check('no cell offers an editor in a read-only session',
      state.editableCells === 0 && state.contentEditableCells === 0 &&
      state.cellEditors === 0,
      `${state.editableCells} editable cells`);

// A click on an editable-looking cell must not start an editor either.
await page.locator('#register-grid tbody tr').first().locator('td').nth(2).click();
await page.waitForTimeout(150);
check('clicking a cell in a read-only session opens no editor',
      (await page.locator('#register-grid .cell-editor').count()) === 0);

/* ------------------------------------------------------------ keyboard */

const at = async (key) => {
    await page.keyboard.press(key);
    return gridState();
};

state = await at('ArrowDown');
check('ArrowDown moves the selection and focus',
      state.selected === 1 && state.activeIndex === 1,
      `selected=${state.selected} active=${state.activeIndex}`);

state = await at('ArrowUp');
check('ArrowUp moves the selection back and focus follows',
      state.selected === 0 && state.activeIndex === 0,
      `selected=${state.selected} active=${state.activeIndex}`);

state = await at('End');
check('End selects the last row', state.selected === state.rows - 1,
      `selected=${state.selected} of ${state.rows}`);

state = await at('Home');
check('Home selects the first row', state.selected === 0,
      `selected=${state.selected}`);

const beforePageDown = state.selected;
state = await at('PageDown');
check('PageDown moves forward by a page and keeps one row selected',
      state.selected > beforePageDown && state.selected < state.rows - 1 &&
      state.selectedCount === 1 && state.activeIndex === state.selected,
      `selected=${state.selected} of ${state.rows}`);

const beforePageUp = state.selected;
state = await at('PageUp');
check('PageUp moves back and keeps focus on the selected row',
      state.selected < beforePageUp && state.activeIndex === state.selected,
      `selected=${state.selected}`);

// A screen reader sees the selection move: aria-selected is on the selected
// row only, before and after a move.
const ariaMoved = await page.evaluate(() => {
    const rows = [...document.querySelectorAll('#register-grid tbody tr')];
    const selected = rows.findIndex(
        (row) => row.getAttribute('aria-selected') === 'true');
    return {
        selected,
        count: rows.filter(
            (row) => row.getAttribute('aria-selected') === 'true').length,
        others: rows.every((row, index) =>
            (row.getAttribute('aria-selected') === 'true') === (index === selected)),
    };
});
check('aria-selected follows the selection exactly once',
      ariaMoved.count === 1 && ariaMoved.others, `${ariaMoved.count} selected`);

/* ------------------------------------------------- transaction panel */

// Select a row whose transaction is known, then open its panel.
state = await at('Home');
await at('ArrowDown');
state = await gridState();
const selectedRow = populated.register.splits[state.selected];
const transaction = await getJson(`api/transaction/${selectedRow.transaction_guid}`);

await page.keyboard.press('Enter');
await page.waitForFunction(
    () => !document.getElementById('register-detail').hidden,
    null, { timeout: 15000 });
const detail = await page.evaluate(() => ({
    open: !document.getElementById('register-detail').hidden,
    title: document.getElementById('register-detail-title').textContent.trim(),
    splits: document.querySelectorAll('#register-detail-body tr').length,
    focused: document.activeElement.id === 'register-detail',
}));
check('Enter opens the transaction panel with every split',
      detail.open && detail.splits === transaction.splits.length &&
      detail.title.includes(transaction.description),
      `${detail.splits} splits, expected ${transaction.splits.length}: ${detail.title}`);
check('the panel takes focus so a screen reader reads it',
      detail.focused, `focus=${detail.focused}`);

await page.keyboard.press('Escape');
await page.waitForFunction(
    () => document.getElementById('register-detail').hidden,
    null, { timeout: 15000 });
state = await gridState();
check('Escape closes the panel and returns focus to the selected row',
      state.selected === 1 && state.activeIndex === 1,
      `selected=${state.selected} active=${state.activeIndex}`);

/* --------------------------------------------------------- menu actions */

// Edit > Open Account: page-scoped in the desktop, enabled here once a tree
// account is selected.  It re-opens the same register.
await page.locator('#menubar .menu[data-menu="edit"] > .menu-title').click();
const openAccount = await page.evaluate(() => {
    const item = document.querySelector(
        '#menubar .menu[data-menu="edit"] [data-item="open-account"]');
    return { disabled: item.getAttribute('aria-disabled'),
             label: item.textContent.trim() };
});
check('Edit > Open Account is enabled for the selected account',
      openAccount.disabled === 'false', `${openAccount.label}: ${openAccount.disabled}`);
const reopened = page.waitForResponse(
    (response) => response.url().includes('/api/register/') &&
                 response.status() === 200);
await page.locator('#menubar .menu[data-menu="edit"] [data-item="open-account"]')
    .click();
await reopened;
await page.waitForFunction(
    (expected) => !document.getElementById('register').hidden &&
        document.querySelectorAll('#register-grid tbody tr').length === expected,
    populated.register.splits.length, { timeout: 15000 });
state = await gridState();
check('Edit > Open Account keeps the register on the same account',
      state.title === populated.register.full_name && state.rows ===
        populated.register.splits.length,
      `${state.rows} rows, ${state.title}`);

// View > Refresh re-reads the book and the open register.  Wait for the
// register request the refresh makes, not just for the old rows to still be
// there.
await page.locator('#menubar .menu[data-menu="view"] > .menu-title').click();
const refreshed = page.waitForResponse(
    (response) => response.url().includes('/api/register/') &&
                 response.status() === 200);
await page.locator('#menubar .menu[data-menu="view"] [data-item="refresh"]').click();
await refreshed;
await page.waitForFunction(
    (expected) => document.querySelectorAll('#register-grid tbody tr').length === expected,
    populated.register.splits.length, { timeout: 30000 });
state = await gridState();
check('View > Refresh keeps the open register with the same rows',
      !state.hidden && state.rows === populated.register.splits.length,
      `${state.rows} rows`);

/* ------------------------------------------------------- empty account */

await clickAccount(empty.account.guid);
state = await gridState();
check('an account with no splits shows an empty grid, not an error',
      !state.hidden && state.rows === 0 &&
      state.countText === '0 rows' && state.selectedCount === 0,
      `${state.rows} rows, "${state.countText}"`);
const emptyMessage = await page.evaluate(() =>
    document.getElementById('register-message').hidden);
check('the empty register has no error message', emptyMessage, `hidden=${emptyMessage}`);

for (const problem of problems) console.log(`page problem: ${problem}`);
check('no console errors or failed requests', problems.length === 0,
      `${problems.length} problems`);

if (process.env.UI_SCREENSHOT)
    await page.screenshot({ path: process.env.UI_SCREENSHOT });

await browser.close();
process.exit(failures === 0 ? 0 : 1);
