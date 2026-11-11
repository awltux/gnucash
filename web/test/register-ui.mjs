// Headless browser test for the read-only register page.
//
//   node web/test/register-ui.mjs <url>
//
// <url> is a gnc-webd address (with token) whose book is the generated demo
// book: its account with the most rows must have at least twenty, which the
// keyboard checks (PageDown/PageUp, End/Home) need.  web/run-ui-test.sh
// generates that book with web/test/make-demo-book.py for this phase.  The
// FIXTURE below is checked before any check runs, so a sample1 server is
// refused with one line instead of failing the checks that need rows.  The
// test drives a real browser and checks the grid, the row model, keyboard
// navigation, the read-only transaction panel, and the two menu actions this
// stage enables.
//
// Needs playwright-core on NODE_PATH, exactly like ui-test.mjs.

import { createRequire } from 'node:module';
import { requireFixture } from './fixture.mjs';

const { chromium } = createRequire(import.meta.url)('playwright-core');

export const FIXTURE = { book: 'demo', writable: false };

const url = process.argv[2];
if (!url) {
    console.error('usage: node register-ui.mjs <url>');
    process.exit(2);
}

// Refuse a server that is not the declared fixture before a single check.
await requireFixture('register-ui.mjs', FIXTURE, url);

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

// The tree prints the engine's amount string, which may carry Unicode bidi
// isolates around a currency amount.  They are invisible but they are part of
// textContent, so compare figures with them removed.
const stripIsolates = (text) => (text || '').replace(/[\u2066\u2069]/g, '');

const treeTotal = async (guid) => stripIsolates(await page.locator(
    `#tree li[data-guid="${guid}"] > .account-line > span.total`).textContent());

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
            contextHidden: document.getElementById('register-context').hidden,
            contextText: document.getElementById('register-context').textContent.trim(),
            childLinks: [...document.querySelectorAll(
                '#register-context button.link')].map((link) => ({
                guid: link.dataset.guid,
                text: link.textContent.trim(),
            })),
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

// Stage 1: the account tree is the Book page's pinned Accounts tab and a
// register is a tab of its own, so a tree click needs the Accounts tab on
// screen first.  This is the page model, not a weaker check: the click still
// selects the same account and waits for its register.
const waitForRoute = () => page.waitForFunction(
    () => document.documentElement.dataset.routeReady === '1',
    null, { timeout: 30000 });
await waitForRoute();

async function showAccountsTab() {
    if (await page.evaluate(() => document.getElementById('register').hidden))
        return; // the Accounts tab is already showing
    await page.locator('#tab-accounts').click();
    await page.waitForFunction(
        () => document.getElementById('register').hidden,
        null, { timeout: 15000 });
}

async function clickAccount(guid) {
    await showAccountsTab();
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
const leafTreeTotal = await treeTotal(populated.account.guid);
check('a leaf shows one balance, not a subtree total',
      !state.balanceText.includes('including sub-accounts') &&
      stripIsolates(state.balanceText) ===
        `Balance: ${populated.register.balance}`,
      state.balanceText);
check('a leaf\'s subtree total matches what the tree shows for it',
      stripIsolates(populated.register.subtree_balance) === leafTreeTotal,
      `${populated.register.subtree_balance} vs ${leafTreeTotal}`);
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

/* ------------------------------------- split transactions: the way in */

// The demo book has both kinds of row: two-split transactions (Transfer names
// the other account) and split transactions (the engine's placeholder).  The
// row model says which is which with split_count, so the checks do not depend
// on a display string.
const registerRows = populated.register.splits;
const multiIndex = registerRows.findIndex((row) => row.split_count > 2);
const singleIndex = registerRows.findIndex((row) => row.split_count === 2);
check('the register has a split transaction to open', multiIndex >= 0,
      multiIndex >= 0 ? `row ${multiIndex}` : 'none');
check('the register also has a two-split row to compare with',
      singleIndex >= 0, singleIndex >= 0 ? `row ${singleIndex}` : 'none');

async function clickRow(index) {
    await page.locator('#register-grid tbody tr').nth(index).locator('td').first()
        .click();
    await page.waitForFunction(
        (i) => document.querySelectorAll('#register-grid tbody tr')[i]
            ?.getAttribute('aria-selected') === 'true',
        index, { timeout: 15000 });
}

// Click the transfer control on `index`, the way a reader opens the panel,
// and wait for the panel to have content.
async function openSplitPanel(index) {
    await page.locator('#register-grid tbody tr').nth(index)
        .locator('td').nth(4).locator('button.split-link').click();
    await page.waitForFunction(
        () => !document.getElementById('register-detail').hidden,
        null, { timeout: 15000 });
}

async function waitForPanelClosed() {
    await page.waitForFunction(
        () => document.getElementById('register-detail').hidden,
        null, { timeout: 15000 });
}

const transfers = await page.evaluate(() =>
    [...document.querySelectorAll('#register-grid tbody tr')].map((tr) => {
        const cell = tr.children[4];
        const button = cell.querySelector('button.split-link');
        return {
            hasButton: Boolean(button),
            text: cell.textContent.trim(),
            label: button ? button.getAttribute('aria-label') : '',
        };
    }));
check('only a split transaction\'s transfer cell is a control',
      transfers.every((cell, index) =>
          cell.hasButton === (registerRows[index].split_count > 2)),
      `${transfers.filter((cell) => cell.hasButton).length} controls, ` +
      `${registerRows.filter((row) => row.split_count > 2).length} ` +
      `split transactions`);
check('the control keeps the placeholder and says how many splits it opens',
      transfers[multiIndex] &&
      transfers[multiIndex].text === '-- Split Transaction --' &&
      transfers[multiIndex].label.includes(
          String(registerRows[multiIndex].split_count)),
      `${transfers[multiIndex]?.text} / ${transfers[multiIndex]?.label}`);

const multiRow = registerRows[multiIndex];
const multiTransaction = await getJson(
    `api/transaction/${multiRow.transaction_guid}`);

await clickRow(multiIndex);
await openSplitPanel(multiIndex);

const splitPanel = await page.evaluate(() => {
    const panel = document.getElementById('register-detail');
    const table = document.getElementById('register-detail-table');
    return {
        open: !panel.hidden,
        role: panel.getAttribute('role'),
        labelledby: panel.getAttribute('aria-labelledby'),
        meta: document.getElementById('register-detail-meta').textContent.trim(),
        total: document.getElementById('register-detail-total')
            .textContent.trim(),
        tableRole: table.getAttribute('role'),
        tableLabel: table.getAttribute('aria-label'),
        rowcount: table.getAttribute('aria-rowcount'),
        rows: [...document.querySelectorAll('#register-detail-body tr')]
            .map((tr) => ({
                account: tr.children[0].textContent.trim(),
                accountGuid: tr.children[0].querySelector('a')
                    ?.dataset.accountGuid || '',
                href: tr.children[0].querySelector('a')
                    ?.getAttribute('href') || '',
                memo: tr.children[1].textContent.trim(),
                amount: tr.children[4].textContent.trim(),
            })),
    };
});
check('clicking the transfer control opens the panel with every split',
      splitPanel.open &&
      splitPanel.rows.length === multiTransaction.splits.length,
      `${splitPanel.rows.length} splits, expected ` +
      `${multiTransaction.splits.length}`);
check('the panel names the whole transaction',
      splitPanel.meta.includes(multiTransaction.date) &&
      splitPanel.meta.includes(multiTransaction.num) &&
      splitPanel.meta.includes(multiTransaction.description),
      splitPanel.meta);
check('every split shows its account, memo and amount',
      splitPanel.rows.every((row, index) =>
          row.account === multiTransaction.splits[index].account &&
          row.memo === multiTransaction.splits[index].memo &&
          row.amount === multiTransaction.splits[index].amount),
      splitPanel.rows.map((row) => `${row.account} ${row.amount}`).join('; '));
check('each split account links to its own register',
      splitPanel.rows.every((row, index) =>
          row.accountGuid === multiTransaction.splits[index].account_guid &&
          row.href.endsWith(
              `/register/${multiTransaction.splits[index].account_guid}`)),
      splitPanel.rows.map((row) => row.href).join(' '));
check('the panel says the total and whether the transaction balances',
      splitPanel.total.includes(multiTransaction.total) &&
      splitPanel.total.includes(multiTransaction.balanced
          ? 'balances' : 'does not balance') &&
      (multiTransaction.balanced ||
       splitPanel.total.includes(multiTransaction.imbalance)),
      splitPanel.total);
check('the panel and its split table carry their roles and names',
      splitPanel.role === 'region' &&
      splitPanel.labelledby === 'register-detail-title' &&
      splitPanel.tableRole === 'table' &&
      splitPanel.tableLabel.includes(multiTransaction.num) &&
      splitPanel.rowcount === String(splitPanel.rows.length + 1),
      `panel ${splitPanel.role}, table ${splitPanel.tableRole} ` +
      `${splitPanel.tableLabel}, rows ${splitPanel.rowcount}`);

// A split's account link is the way into the other legs: it opens that
// account's own register tab (or switches to it) and the panel closes.
const otherSplitIndex = multiTransaction.splits.findIndex(
    (split) => split.account_guid !== populated.account.guid);
const otherSplit = multiTransaction.splits[otherSplitIndex];
const otherRegister = await getJson(`api/register/${otherSplit.account_guid}`);
await page.locator('#register-detail-body tr').nth(otherSplitIndex)
    .locator('a.account-link').click();
await page.waitForFunction(
    (expected) => {
        const section = document.getElementById('register');
        return !section.hidden &&
            document.getElementById('register-detail').hidden &&
            document.getElementById('register-title').textContent.trim() ===
                expected.title &&
            document.querySelectorAll('#register-grid tbody tr').length ===
                expected.rows;
    },
    { title: otherRegister.full_name, rows: otherRegister.splits.length },
    { timeout: 30000 });
check('a split account link opens that account\'s register tab',
      new URL(page.url()).pathname.endsWith(
          `/register/${otherSplit.account_guid}`),
      `${otherRegister.full_name}: ${page.url()}`);

// Escape returns focus to the row the panel came from, not just to whatever
// row happens to be selected after the link navigation.
await clickAccount(populated.account.guid);
await clickRow(multiIndex);
await openSplitPanel(multiIndex);
await page.keyboard.press('Escape');
await waitForPanelClosed();
state = await gridState();
check('Escape returns focus to the row the panel was opened from',
      state.selected === multiIndex && state.activeIndex === multiIndex,
      `selected=${state.selected} active=${state.activeIndex}, ` +
      `expected ${multiIndex}`);

// Tab moves through the split links, so the panel is usable without a mouse.
await openSplitPanel(multiIndex);
const focusAt = () => page.evaluate(() => {
    const node = document.activeElement;
    return {
        tag: node?.tagName || '',
        guid: node?.dataset?.accountGuid || '',
    };
});
await page.keyboard.press('Tab');
const firstLink = await focusAt();
check('Tab moves from the panel to the first split link',
      firstLink.tag === 'A' &&
      firstLink.guid === multiTransaction.splits[0].account_guid,
      `${firstLink.tag} ${firstLink.guid}`);
await page.keyboard.press('Tab');
const secondLink = await focusAt();
check('Tab reaches the next split link',
      secondLink.tag === 'A' &&
      secondLink.guid === multiTransaction.splits[1].account_guid,
      `${secondLink.tag} ${secondLink.guid}`);
await page.keyboard.press('Escape');
await waitForPanelClosed();

// A two-split row has nothing to disclose: its cell stays a label, and a
// click on it selects the row without opening the panel.
await clickRow(singleIndex);
const singleTransfer = await page.evaluate((index) => {
    const cell = document.querySelectorAll('#register-grid tbody tr')[index]
        .children[4];
    return { buttons: cell.querySelectorAll('button').length,
             text: cell.textContent.trim() };
}, singleIndex);
check('a two-split row keeps the transfer cell as plain text',
      singleTransfer.buttons === 0 &&
      singleTransfer.text === registerRows[singleIndex].transfer_account,
      `${singleTransfer.buttons} controls, "${singleTransfer.text}"`);
await page.locator('#register-grid tbody tr').nth(singleIndex)
    .locator('td').nth(4).click();
await page.waitForTimeout(150);
check('clicking a two-split row\'s transfer cell opens no panel',
      await page.evaluate(
          () => document.getElementById('register-detail').hidden));

// The demo book is balanced, so the unbalanced branch is shown one substituted
// response rather than a specially broken book: the panel must say it does
// not balance, and by how much, instead of claiming it does.
const unbalancedIndex = registerRows.findIndex((row, index) =>
    row.split_count > 2 && index !== multiIndex);
check('the register has a second split transaction for the unbalanced case',
      unbalancedIndex >= 0,
      unbalancedIndex >= 0 ? `row ${unbalancedIndex}` : 'none');
if (unbalancedIndex >= 0) {
    const unbalancedGuid = registerRows[unbalancedIndex].transaction_guid;
    const unbalancedPattern = `**/api/transaction/${unbalancedGuid}`;
    await page.route(unbalancedPattern, async (route) => {
        const body = await (await route.fetch()).json();
        body.balanced = false;
        body.imbalance = '$0.01';
        await route.fulfill({ json: body });
    });
    await clickRow(unbalancedIndex);
    await openSplitPanel(unbalancedIndex);
    const unbalancedText =
        await page.locator('#register-detail-total').textContent();
    check('the panel says when a transaction does not balance, and by how much',
          unbalancedText.includes('does not balance') &&
          unbalancedText.includes('$0.01'), unbalancedText);
    await page.unroute(unbalancedPattern);
    await page.keyboard.press('Escape');
    await waitForPanelClosed();
}

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

/* ------------------------------------------ a placeholder / parent account */

// Assets is a placeholder on both fixtures this suite runs against: it has no
// splits of its own, and its money is in its children.  The register must show
// the account's own balance and the tree's subtree total, say why the grid is
// empty, and link to the children.
const placeholder = (accounts.children || []).find(
    (account) => account.name === 'Assets');
check('the book has the Assets placeholder account with children',
      Boolean(placeholder) && placeholder.children.length > 0,
      placeholder ? `${placeholder.children.length} children` : 'none');

const assets = placeholder
    ? await getJson(`api/register/${placeholder.guid}`) : null;
check('the Assets register reports its own balance and subtree total',
      Boolean(assets) && assets.placeholder === true &&
      assets.has_children === true &&
      assets.own_equals_subtree === false &&
      assets.subtree_balance !== assets.balance,
      assets ? `${assets.balance} / ${assets.subtree_balance}` : 'none');

await clickAccount(placeholder.guid);
state = await gridState();
check('a placeholder shows its own balance and the subtree total',
      state.balanceText.startsWith('this account ') &&
      state.balanceText.includes(' · including sub-accounts ') &&
      stripIsolates(state.balanceText) ===
        `this account ${assets.balance} · including sub-accounts ` +
        `${stripIsolates(assets.subtree_balance)}`,
      state.balanceText);
const assetsTreeTotal = await treeTotal(placeholder.guid);
check('a placeholder\'s subtree figure matches what the tree shows for it',
      stripIsolates(assets.subtree_balance) === assetsTreeTotal,
      `${assets.subtree_balance} vs ${assetsTreeTotal}`);
check('a placeholder explains why its register is empty',
      !state.contextHidden && /placeholder/i.test(state.contextText) &&
      /transactions are in its sub-accounts/i.test(state.contextText),
      state.contextText);
check('a placeholder links to its sub-accounts',
      state.childLinks.length === assets.children.length &&
      state.childLinks.every((link, index) =>
          link.guid === assets.children[index].guid &&
          link.text === assets.children[index].name),
      state.childLinks.map((link) => link.text).join(', '));

const firstChild = assets.children[0];
const childRegister = await getJson(`api/register/${firstChild.guid}`);
await page.locator(
    `#register-context button.link[data-guid="${firstChild.guid}"]`).click();
await page.waitForFunction(
    (expected) => {
        const section = document.getElementById('register');
        return !section.hidden &&
            document.getElementById('register-title').textContent.trim() ===
                expected.title &&
            document.querySelectorAll('#register-grid tbody tr').length ===
                expected.rows;
    },
    { title: childRegister.full_name, rows: childRegister.splits.length },
    { timeout: 30000 });
state = await gridState();
check('a sub-account link opens that child\'s register',
      state.title === childRegister.full_name &&
      state.rows === childRegister.splits.length &&
      new URL(page.url()).pathname.endsWith(`/register/${firstChild.guid}`),
      `${state.title}, ${state.rows} rows, ${page.url()}`);

for (const problem of problems) console.log(`page problem: ${problem}`);
check('no console errors or failed requests', problems.length === 0,
      `${problems.length} problems`);

if (process.env.UI_SCREENSHOT)
    await page.screenshot({ path: process.env.UI_SCREENSHOT });

await browser.close();
process.exit(failures === 0 ? 0 : 1);
