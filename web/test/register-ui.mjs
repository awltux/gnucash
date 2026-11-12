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
// navigation, the inline transaction detail band under its row, and the two
// menu actions this stage enables.
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

// Cents from an engine amount like "$1,234.56", "-$12.00" or "(£3.00)".  The
// chooser compares figures as numbers where the printer's formatting is not
// what is under test (a zero balance, say).
function cents(text) {
    let value = stripIsolates(text).trim();
    let negative = false;
    if (value.startsWith('(') && value.endsWith(')')) {
        negative = true;
        value = value.slice(1, -1);
    }
    value = value.replace(/[^0-9,.\-]/g, '').replace(/,/g, '');
    if (value.startsWith('-')) {
        negative = !negative;
        value = value.slice(1);
    }
    if (!value) return null;
    const amount = Math.round(Number(value) * 100);
    return negative ? -amount : amount;
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
        // The register's own rows are the direct rows that are not detail
        // bands.  Bands are rows of the grid too, so they are counted apart.
        const allRows = [...grid.querySelectorAll(':scope > tbody > tr')];
        const rows = allRows.filter((row) => !row.classList.contains('detail-band'));
        const bands = allRows.filter((row) => row.classList.contains('detail-band'));
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
                // The chooser's label is the name span; the amount is a
                // separate span beside it, never part of the name.
                name: (link.querySelector('.account-choice-name') || link)
                    .textContent.trim(),
                total: (link.querySelector('.account-choice-total') || {})
                    .textContent?.trim() ?? '',
                text: link.textContent.trim(),
                ariaLabel: link.getAttribute('aria-label') || '',
                nameWeight: link.querySelector('.account-choice-name')
                    ? getComputedStyle(link.querySelector(
                        '.account-choice-name')).fontWeight : '',
                totalWeight: link.querySelector('.account-choice-total')
                    ? getComputedStyle(link.querySelector(
                        '.account-choice-total')).fontWeight : '',
            })),
            rows: rows.length,
            bands: bands.length,
            openBands: bands.filter((band) => !band.hidden).length,
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
            bottomPanel: document.getElementById('register-detail') !== null,
            oldPanelIds: document.querySelectorAll('[id^="register-detail"]').length,
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
            const rows = document.querySelectorAll(
                '#register-grid > tbody > tr:not(.detail-band)').length;
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
check('the old bottom transaction panel is gone, with no hidden duplicate',
      !state.bottomPanel && state.oldPanelIds === 0,
      `panel=${state.bottomPanel} old ids=${state.oldPanelIds}`);
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
await page.locator('#register-grid > tbody > tr:not(.detail-band)').first()
    .locator('td').nth(2).click();
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
    const rows = [...document.querySelectorAll(
        '#register-grid > tbody > tr:not(.detail-band)')];
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

/* ------------------------------------- split transactions: the inline band */

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

// The transfer cell of a split transaction is the disclosure control; a
// two-split row keeps a label.  The row model's split_count, not a display
// string, says which is which.
const transfers = await page.evaluate(() =>
    [...document.querySelectorAll(
        '#register-grid > tbody > tr:not(.detail-band)')].map((tr) => {
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

// The register's own rows are the direct tbody children that are not detail
// bands.  A band is a row of the grid too, so every query that means "the
// account's rows" must say so; counting `tbody tr` would count the bands.
const rowLocator = (index) => page.locator(
    `#register-grid > tbody > tr[data-index="${index}"]`);

async function clickRow(index) {
    await rowLocator(index).locator('td').first().click();
    await page.waitForFunction(
        (i) => document.querySelector(
            `#register-grid > tbody > tr[data-index="${i}"]`)
            ?.getAttribute('aria-selected') === 'true',
        index, { timeout: 15000 });
}

async function clickSplitControl(index) {
    await rowLocator(index).locator('td').nth(4)
        .locator('button.split-link').click();
}

async function waitForBand(index, open) {
    await page.waitForFunction(({ i, wantOpen }) => {
        const row = document.querySelector(
            `#register-grid > tbody > tr[data-index="${i}"]`);
        const band = row?.nextElementSibling;
        if (!band || !band.classList.contains('detail-band')) return false;
        if (!wantOpen) return band.hidden;
        return !band.hidden &&
            band.querySelectorAll('table > tbody > tr').length > 0;
    }, { i: index, wantOpen: open }, { timeout: 15000 });
}

async function openSplitBand(index) {
    await clickSplitControl(index);
    await waitForBand(index, true);
}

// Everything the checks need to know about one row and the band that follows
// it, read from the DOM in one go.  `immediate` is the DOM position of the
// band, not merely whether it is visible: band.nextSibling must be the row.
const bandInfo = (index) => page.evaluate((i) => {
    const row = document.querySelector(
        `#register-grid > tbody > tr[data-index="${i}"]`);
    const band = row?.nextElementSibling;
    const button = row?.querySelector('button.split-link');
    const grid = document.getElementById('register-grid');
    const dataRows = [...grid.querySelectorAll(':scope > tbody > tr')]
        .filter((tr) => !tr.classList.contains('detail-band'));
    const bands = [...grid.querySelectorAll(':scope > tbody > tr.detail-band')];
    const table = band?.querySelector('table');
    const isBand = Boolean(band) &&
        band.classList.contains('detail-band');
    return {
        rowFound: Boolean(row),
        bandFound: isBand,
        immediate: Boolean(row && isBand && row.nextElementSibling === band),
        bandFollowsRow: Boolean(band && band.previousElementSibling === row),
        hidden: isBand ? band.hidden : null,
        bandId: band?.id || '',
        buttonExpanded: button?.getAttribute('aria-expanded') ?? null,
        buttonControls: button?.getAttribute('aria-controls') ?? null,
        bandRole: band?.getAttribute('role') || '',
        bandRowIndex: band?.getAttribute('aria-rowindex') || '',
        bandCells: band
            ? [...band.children].map((cell) => ({
                role: cell.getAttribute('role'),
                colindex: cell.getAttribute('aria-colindex'),
                colspan: cell.getAttribute('aria-colspan'),
                htmlColspan: cell.getAttribute('colspan'),
            }))
            : [],
        openBands: bands.filter((candidate) => !candidate.hidden).length,
        dataRows: dataRows.length,
        ariaRowcount: grid.getAttribute('aria-rowcount'),
        sourceSelected: row?.getAttribute('aria-selected') || '',
        title: band?.querySelector('.detail-title')?.textContent.trim() || '',
        meta: band?.querySelector('.detail-meta')?.textContent.trim() || '',
        total: band?.querySelector('.detail-total')?.textContent.trim() || '',
        tableRole: table?.getAttribute('role') || '',
        tableLabel: table?.getAttribute('aria-label') || '',
        tableRowcount: table?.getAttribute('aria-rowcount') || '',
        splits: band
            ? [...band.querySelectorAll('table > tbody > tr')].map((tr) => ({
                account: tr.children[0].textContent.trim(),
                accountGuid: tr.children[0].querySelector('a')
                    ?.dataset.accountGuid || '',
                href: tr.children[0].querySelector('a')
                    ?.getAttribute('href') || '',
                memo: tr.children[1].textContent.trim(),
                action: tr.children[2].textContent.trim(),
                reconcile: tr.children[3].textContent.trim(),
                amount: tr.children[4].textContent.trim(),
            }))
            : [],
    };
}, index);

const focusAt = () => page.evaluate(() => {
    const node = document.activeElement;
    return {
        tag: node?.tagName || '',
        className: node?.className || '',
        guid: node?.dataset?.accountGuid || '',
        transactionGuid: node?.dataset?.transactionGuid || '',
        rowIndex: node?.closest?.('tr[data-index]')?.dataset.index ?? '',
    };
});

/* ------------------------------------------- Enter opens the row's band */

// The old bottom panel opened from anywhere for any row.  The band belongs to
// a split transaction's row, so the keyboard check selects that row: Enter is
// then the same toggle the transfer button performs.
if (multiIndex !== 0)
    await clickRow(multiIndex);
const beforeEnter = await gridState();
await page.keyboard.press('Enter');
await waitForBand(multiIndex, true);
const entered = await bandInfo(multiIndex);
const enteredTransaction = await getJson(
    `api/transaction/${registerRows[multiIndex].transaction_guid}`);
check('Enter expands the split transaction\'s band with every split',
      entered.immediate && !entered.hidden &&
      entered.splits.length === enteredTransaction.splits.length &&
      entered.title.includes(enteredTransaction.description),
      `${entered.splits.length} splits, expected ` +
      `${enteredTransaction.splits.length}: ${entered.title}`);
check('the band is the row immediately after its source row',
      entered.immediate && entered.bandFollowsRow,
      `band ${entered.bandId} follows row ${multiIndex}`);
check('the transfer button says it controls this band, and that it is open',
      entered.buttonExpanded === 'true' &&
      entered.buttonControls === entered.bandId &&
      entered.bandId.startsWith('split-detail-') &&
      entered.sourceSelected === 'true',
      `expanded=${entered.buttonExpanded} controls=${entered.buttonControls} ` +
      `band=${entered.bandId}`);

const openState = await gridState();
check('expanding does not change the register\'s own rows or their count',
      openState.rows === beforeEnter.rows &&
      openState.countText === beforeEnter.countText,
      `${openState.rows} rows ("${openState.countText}") was ` +
      `${beforeEnter.rows} ("${beforeEnter.countText}")`);
check('the grid\'s row count gains only the open band',
      openState.ariaRowcount === String(openState.rows + 2) &&
      entered.bandRowIndex === String(multiIndex + 3),
      `aria-rowcount=${openState.ariaRowcount} band row=${entered.bandRowIndex}`);

await page.keyboard.press('Escape');
await waitForBand(multiIndex, false);
const afterEscape = await bandInfo(multiIndex);
state = await gridState();
check('Escape collapses the band and returns focus to the source row',
      afterEscape.hidden && afterEscape.openBands === 0 &&
      state.selected === multiIndex && state.activeIndex === multiIndex,
      `hidden=${afterEscape.hidden} selected=${state.selected} ` +
      `active=${state.activeIndex}`);
check('the transfer button reports collapsed after Escape',
      afterEscape.buttonExpanded === 'false' &&
      afterEscape.buttonControls === afterEscape.bandId,
      `expanded=${afterEscape.buttonExpanded} controls=${afterEscape.buttonControls}`);

/* ------------------------------------------ clicking the transfer control */

// The demo book's first split transaction: its transfer control is the way in.
await clickRow(multiIndex);
await openSplitBand(multiIndex);
const multiTransaction = await getJson(
    `api/transaction/${registerRows[multiIndex].transaction_guid}`);
const splitBand = await bandInfo(multiIndex);
check('clicking the transfer control expands the band with every split',
      splitBand.immediate && !splitBand.hidden &&
      splitBand.splits.length === multiTransaction.splits.length,
      `${splitBand.splits.length} splits, expected ` +
      `${multiTransaction.splits.length}`);
check('the band names the whole transaction',
      splitBand.title.includes(multiTransaction.description) &&
      splitBand.meta.includes(multiTransaction.date) &&
      splitBand.meta.includes(multiTransaction.num) &&
      splitBand.meta.includes(multiTransaction.description),
      `${splitBand.title} / ${splitBand.meta}`);
check('every split shows its account, memo, action, reconcile state and amount',
      splitBand.splits.every((row, index) => {
          const split = multiTransaction.splits[index];
          return row.account === split.account &&
              row.memo === split.memo &&
              row.action === split.action &&
              row.reconcile === ({
                  y: 'R', c: 'C', f: 'F', v: 'V',
              }[split.reconcile] || '') &&
              row.amount === split.amount;
      }),
      splitBand.splits.map((row) =>
          `${row.account} ${row.memo} ${row.action} ` +
          `${row.reconcile} ${row.amount}`).join('; '));
check('each split account links to its own register',
      splitBand.splits.every((row, index) =>
          row.accountGuid === multiTransaction.splits[index].account_guid &&
          row.href.endsWith(
              `/register/${multiTransaction.splits[index].account_guid}`)),
      splitBand.splits.map((row) => row.href).join(' '));
check('the band says the total and whether the transaction balances',
      splitBand.total.includes(multiTransaction.total) &&
      splitBand.total.includes(multiTransaction.balanced
          ? 'balances' : 'does not balance') &&
      (multiTransaction.balanced ||
       splitBand.total.includes(multiTransaction.imbalance)),
      splitBand.total);
check('the band is a grid row with one cell across every column',
      splitBand.bandRole === 'row' &&
      splitBand.bandCells.length === 1 &&
      splitBand.bandCells[0].role === 'gridcell' &&
      splitBand.bandCells[0].colindex === '1' &&
      splitBand.bandCells[0].colspan === '8' &&
      splitBand.bandCells[0].htmlColspan === '8',
      `role=${splitBand.bandRole} cells=` +
      JSON.stringify(splitBand.bandCells));
check('the split table is labelled and counts every split and its header',
      splitBand.tableRole === 'table' &&
      splitBand.tableLabel.includes(multiTransaction.num) &&
      splitBand.tableRowcount === String(splitBand.splits.length + 1),
      `table ${splitBand.tableRole} "${splitBand.tableLabel}" rows ` +
      `${splitBand.tableRowcount}`);
check('the transfer control and the band agree on the same id',
      splitBand.buttonExpanded === 'true' &&
      splitBand.buttonControls === splitBand.bandId,
      `controls=${splitBand.buttonControls} band=${splitBand.bandId}`);

// The same control is a toggle, not just an opener.
await clickSplitControl(multiIndex);
await waitForBand(multiIndex, false);
const toggledClosed = await bandInfo(multiIndex);
check('clicking the same transfer control again collapses the band',
      toggledClosed.hidden && toggledClosed.openBands === 0 &&
      toggledClosed.buttonExpanded === 'false',
      `hidden=${toggledClosed.hidden} expanded=` +
      `${toggledClosed.buttonExpanded}`);
await openSplitBand(multiIndex);

// One open at a time: opening a second row's band closes the first.
const otherMultiIndex = registerRows.findIndex(
    (row, index) => row.split_count > 2 && index !== multiIndex);
check('the register has a second split transaction to open',
      otherMultiIndex >= 0,
      otherMultiIndex >= 0 ? `row ${otherMultiIndex}` : 'none');
if (otherMultiIndex >= 0) {
    await openSplitBand(otherMultiIndex);
    const first = await bandInfo(multiIndex);
    const second = await bandInfo(otherMultiIndex);
    check('opening another row\'s band closes the previous one',
          first.hidden && !second.hidden && second.openBands === 1 &&
          first.buttonExpanded === 'false' &&
          second.buttonExpanded === 'true',
          `open bands=${second.openBands} first=` +
          `${first.buttonExpanded} second=${second.buttonExpanded}`);
    // Back to the first, so the link and keyboard checks below use it.
    await openSplitBand(multiIndex);
}

/* ------------------------------------------ a split link opens its register */

// A split's account link is the way into the other legs: it opens that
// account's own register tab (or switches to it) and the band closes.
const otherSplitIndex = multiTransaction.splits.findIndex(
    (split) => split.account_guid !== populated.account.guid);
const otherSplit = multiTransaction.splits[otherSplitIndex];
const otherRegister = await getJson(`api/register/${otherSplit.account_guid}`);
await page.locator('#register-grid tr.detail-band:not([hidden]) ' +
                    'table > tbody > tr').nth(otherSplitIndex)
    .locator('a.account-link').click();
await page.waitForFunction(
    (expected) => {
        const section = document.getElementById('register');
        return !section.hidden &&
            document.querySelectorAll(
                '#register-grid tr.detail-band:not([hidden])').length === 0 &&
            document.getElementById('register-title').textContent.trim() ===
                expected.title &&
            document.querySelectorAll(
                '#register-grid > tbody > tr:not(.detail-band)').length ===
                expected.rows;
    },
    { title: otherRegister.full_name, rows: otherRegister.splits.length },
    { timeout: 30000 });
check('a split account link opens that account\'s register tab',
      new URL(page.url()).pathname.endsWith(
          `/register/${otherSplit.account_guid}`),
      `${otherRegister.full_name}: ${page.url()}`);

/* ------------------------------------- keyboard: Tab in, Escape back out */

// Escape returns focus to the row the band came from, not just to whatever
// row happens to be selected after the link navigation.  The band's links are
// reached with Tab from the transfer control, and Shift+Tab comes back: no
// trap and no stop that does nothing.
await clickAccount(populated.account.guid);
await clickRow(multiIndex);
await openSplitBand(multiIndex);
await page.keyboard.press('Tab');
const firstLink = await focusAt();
check('Tab moves from the transfer control to the first split link',
      firstLink.tag === 'A' &&
      firstLink.guid === multiTransaction.splits[0].account_guid,
      `${firstLink.tag} ${firstLink.guid}`);
await page.keyboard.press('Tab');
const secondLink = await focusAt();
check('Tab reaches the next split link',
      secondLink.tag === 'A' &&
      secondLink.guid === multiTransaction.splits[1].account_guid,
      `${secondLink.tag} ${secondLink.guid}`);
await page.keyboard.press('Shift+Tab');
const backToFirst = await focusAt();
check('Shift+Tab returns to the previous split link',
      backToFirst.tag === 'A' &&
      backToFirst.guid === multiTransaction.splits[0].account_guid,
      `${backToFirst.tag} ${backToFirst.guid}`);
await page.keyboard.press('Shift+Tab');
const backToButton = await focusAt();
check('Shift+Tab from the first link returns to the transfer control',
      backToButton.tag === 'BUTTON' && backToButton.className === 'split-link' &&
      backToButton.transactionGuid ===
          registerRows[multiIndex].transaction_guid,
      `${backToButton.tag}.${backToButton.className}`);
// Into the band once more, then Escape from a split link.
await page.keyboard.press('Tab');
await page.keyboard.press('Escape');
await waitForBand(multiIndex, false);
state = await gridState();
check('Escape from inside the band returns focus to the source row',
      state.selected === multiIndex && state.activeIndex === multiIndex &&
      state.openBands === 0,
      `selected=${state.selected} active=${state.activeIndex} ` +
      `open bands=${state.openBands}`);

/* ------------------------------------------- arrows skip the open band */

await clickRow(multiIndex);
await page.keyboard.press('Enter');
await waitForBand(multiIndex, true);
await page.keyboard.press('ArrowDown');
state = await gridState();
const afterArrowDown = await bandInfo(multiIndex);
check('ArrowDown moves to the register row after the band, not the band',
      state.selected === multiIndex + 1 &&
      state.activeIndex === multiIndex + 1 &&
      afterArrowDown.openBands === 1,
      `selected=${state.selected} active=${state.activeIndex} ` +
      `(band still open: ${afterArrowDown.openBands === 1})`);
await page.keyboard.press('ArrowUp');
state = await gridState();
check('ArrowUp moves back to the source row and leaves the band open',
      state.selected === multiIndex && state.activeIndex === multiIndex &&
      state.openBands === 1,
      `selected=${state.selected} active=${state.activeIndex}`);
await page.keyboard.press('Escape');
await waitForBand(multiIndex, false);

/* ------------------------------------------------- a two-split comparison */

// A two-split row has nothing to disclose in a band: its cell stays a label,
// and a click on it selects the row without opening anything.
await clickRow(singleIndex);
const singleTransfer = await page.evaluate((index) => {
    const cell = document.querySelector(
        `#register-grid > tbody > tr[data-index="${index}"]`).children[4];
    return { buttons: cell.querySelectorAll('button').length,
             text: cell.textContent.trim() };
}, singleIndex);
check('a two-split row keeps the transfer cell as plain text',
      singleTransfer.buttons === 0 &&
      singleTransfer.text === registerRows[singleIndex].transfer_account,
      `${singleTransfer.buttons} controls, "${singleTransfer.text}"`);
await rowLocator(singleIndex).locator('td').nth(4).click();
await page.waitForTimeout(150);
const singleBand = await bandInfo(singleIndex);
check('clicking a two-split row\'s transfer cell opens nothing',
      singleBand.bandFound === false && singleBand.openBands === 0 &&
      singleBand.buttonExpanded === null,
      `band=${singleBand.bandFound} open bands=${singleBand.openBands} ` +
      `button=${singleBand.buttonExpanded}`);

/* ------------------------------------------------------ scrolling the band */

// Opening a band must not jump the grid back to the top and must keep the
// source row on screen.  A row deep in the register forces the grid to be
// scrolled when its band opens.
const deepIndex = registerRows.reduce(
    (found, row, index) =>
        row.split_count > 2 && index > 40 ? index : found, -1);
check('the register has a split transaction far enough down to scroll to',
      deepIndex >= 0, deepIndex >= 0 ? `row ${deepIndex}` : 'none');
if (deepIndex >= 0) {
    await clickRow(deepIndex);
    await openSplitBand(deepIndex);
    const scrolled = await page.evaluate((index) => {
        const wrap = document.getElementById('register-grid-wrap');
        const row = document.querySelector(
            `#register-grid > tbody > tr[data-index="${index}"]`);
        const wrapRect = wrap.getBoundingClientRect();
        const rowRect = row.getBoundingClientRect();
        return {
            scrollTop: wrap.scrollTop,
            rowVisible: rowRect.bottom > wrapRect.top &&
                        rowRect.top < wrapRect.bottom,
        };
    }, deepIndex);
    check('opening a band keeps the source row in view and does not jump ' +
          'the grid to the top',
          scrolled.scrollTop > 0 && scrolled.rowVisible,
          `scrollTop=${scrolled.scrollTop} rowVisible=${scrolled.rowVisible}`);
    await page.keyboard.press('Escape');
    await waitForBand(deepIndex, false);
}

/* ------------------------------------------------------- unbalanced branch */

// The demo book is balanced, so the unbalanced branch is shown one substituted
// response rather than a specially broken book: the band must say it does not
// balance, and by how much, instead of claiming it does.
const unbalancedIndex = registerRows.findIndex((row, index) =>
    row.split_count > 2 && index !== multiIndex &&
    index !== otherMultiIndex && index !== deepIndex);
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
    await openSplitBand(unbalancedIndex);
    const unbalancedText = await page.evaluate(() =>
        document.querySelector('#register-grid tr.detail-band:not([hidden]) ' +
                                '.detail-total')?.textContent || '');
    check('the band says when a transaction does not balance, and by how much',
          unbalancedText.includes('does not balance') &&
          unbalancedText.includes('$0.01'), unbalancedText);
    await page.unroute(unbalancedPattern);
    await page.keyboard.press('Escape');
    await waitForBand(unbalancedIndex, false);
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
        document.querySelectorAll(
            '#register-grid > tbody > tr:not(.detail-band)').length === expected,
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
    (expected) => document.querySelectorAll(
        '#register-grid > tbody > tr:not(.detail-band)').length === expected,
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
          // The button now also carries the figure; the name span is the
          // account's name, and the figure is compared separately below.
          link.name === assets.children[index].name),
      state.childLinks.map((link) => `${link.name} ${link.total}`).join(', '));

// The chooser promises "including sub-accounts", so every child's button must
// carry the same subtree figure the tree shows for that child -- the register
// JSON carries it, and the tree cell is the other rendering of one engine
// call.  A child with no activity is still a choice and must say $0.00.
const childTreeFigures = [];
for (const child of assets.children) childTreeFigures.push(await treeTotal(child.guid));
check('the sub-account chooser shows a figure for every child',
      state.childLinks.length === assets.children.length &&
      state.childLinks.every((link) => link.total !== ''),
      state.childLinks.map((link) => `${link.name}: ${link.total}`).join(', '));
check('a chooser figure is the register JSON\'s subtree figure for that child',
      state.childLinks.every((link, index) =>
          stripIsolates(link.total) ===
              stripIsolates(assets.children[index].subtree_balance)),
      state.childLinks.map((link) => link.total).join(', '));
check('every chooser figure matches the tree\'s cell for that child',
      state.childLinks.every((link, index) =>
          stripIsolates(link.total) === childTreeFigures[index]),
      state.childLinks.map((link, index) =>
          `${link.total} vs ${childTreeFigures[index]}`).join('; '));
check('each chooser button names the account and figure in words',
      state.childLinks.every((link, index) =>
          stripIsolates(link.ariaLabel) ===
              `Open ${assets.children[index].name}, including sub-accounts ` +
              `${stripIsolates(link.total)}`),
      state.childLinks.map((link) => link.ariaLabel).join(' | '));
check('the chooser amount is visually secondary to the name',
      state.childLinks.every((link) =>
          link.totalWeight !== '' && link.totalWeight !== link.nameWeight),
      state.childLinks.map((link) =>
          `${link.nameWeight}/${link.totalWeight}`).join(', '));

const idleChildren = [];
for (const child of assets.children) {
    const register = await getJson(`api/register/${child.guid}`);
    if (register.splits.length === 0) idleChildren.push(child);
}
check('a child with no activity is still a choice, showing zero',
      idleChildren.length > 0 && idleChildren.every((child) => {
          const link = state.childLinks.find((row) => row.guid === child.guid);
          return Boolean(link) && cents(link.total) === 0;
      }),
      idleChildren.map((child) => {
          const link = state.childLinks.find((row) => row.guid === child.guid);
          return `${child.name}: ${link ? link.total : 'missing'}`;
      }).join(', '));

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
            document.querySelectorAll(
                '#register-grid > tbody > tr:not(.detail-band)').length ===
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
