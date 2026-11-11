// Headless browser test for the page split (stage 1).
//
//   node web/test/pages-ui.mjs <url>
//
// <url> is a gnc-webd address with its token, or the tokenless
// --debug-port address; the same checks run in both modes because the page
// URLs are built from the same base the server printed.  web/run-ui-test.sh
// runs this as its fifth and sixth phases.
//
// It checks the pages as URLs, not only as clicks:
//
//   * direct navigation to /accounts, /register/<guid>, /reports,
//     /report/<name> and /report/spec/<id> puts the right page on screen;
//   * a reload lands on the same page, tab and report;
//   * Back and Forward move between the pages the URLs name;
//   * the Book page's tab bar: the pinned Accounts tab, one closable register
//     tab per account, open/switch/close, the tablist's ARIA contract, and
//     Ctrl+Tab / Ctrl+Shift+Tab / arrow keys;
//   * opening a report from the list is reflected in the URL;
//   * the menu's page items go to their pages, and a page-scoped item whose
//     page now exists says "no web equivalent" instead of naming a page the
//     web tier lacks;
//   * /report/<name> and /report/spec/<id> still answer the report document
//     to a subresource request (the frame and every harness), while a
//     navigation to the same path gets the shell;
//   * the Reports page's rail / document / inspector layout: each pane is
//     present and collapsible, widths survive a reload, applying options keeps
//     the page and frame scroll, the status bar and breadcrumb describe the
//     report, Ctrl+K / Enter / [ / ] work, the designer is a full-window mode
//     with a live preview and a guarded exit, and the 800px layout stays
//     usable;
//   * no console errors or unexpected failed requests.
//
// Needs playwright-core on NODE_PATH, exactly like ui-test.mjs.

import { createRequire } from 'node:module';

const { chromium } = createRequire(import.meta.url)('playwright-core');

const url = process.argv[2];
if (!url) {
    console.error('usage: node pages-ui.mjs <url>');
    process.exit(2);
}

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
page.on('pageerror', (e) => problems.push(`page error: ${e.message}`));
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

const pageUrl = (path) => new URL(path, url).href;
const pathOf = (href) => new URL(href).pathname;

// WSL forwards the Linux server's 127.0.0.1 to Windows; for a moment after a
// fresh server binds, a Windows client can still be refused.  That is a
// harness fact, not a page or server failure, so connection refusals are
// retried and every other error still fails the run.
const isRefused = (err) =>
    /ECONNREFUSED|CONNECTION_REFUSED|fetch failed/i.test(String(err)) ||
    /ECONNREFUSED/.test(String(err && err.cause));

async function gotoWithRetry(target, options) {
    for (let attempt = 0; ; attempt++) {
        try {
            return await page.goto(target, options);
        } catch (err) {
            if (attempt >= 9 || !isRefused(err)) throw err;
            await page.waitForTimeout(300);
        }
    }
}

async function getJson(path) {
    for (let attempt = 0; ; attempt++) {
        try {
            const response = await fetch(new URL(path, url));
            if (!response.ok)
                throw new Error(
                    `${path} answered ${response.status}: ${await response.text()}`);
            return await response.json();
        } catch (err) {
            if (attempt >= 9 || !isRefused(err)) throw err;
            await new Promise((resolve) => setTimeout(resolve, 300));
        }
    }
}

// The book's leaves, so the register checks follow whatever book the runner
// used instead of hard-coding an account.
const accounts = await getJson('api/accounts');
const leaves = [];
(function walk(node) {
    if (node.children.length === 0) leaves.push(node);
    for (const child of node.children) walk(child);
})(accounts);
const withRows = [];
for (const leaf of leaves) {
    const register = await getJson(`api/register/${leaf.guid}`);
    if (register.splits.length > 0)
        withRows.push({ account: leaf, register });
}
check('the book has at least two accounts with register rows',
      withRows.length >= 2, `${withRows.length} found`);
const first = withRows[0];
const second = withRows[1] || withRows[0];

const specs = await getJson('api/specs');
const builtin = (specs.builtin || [])[0];
check('the book has a built-in report spec to open by URL', Boolean(builtin),
      builtin ? builtin.id : 'none');

async function appState() {
    return page.evaluate(() => {
        const tabs = [...document.querySelectorAll('#book-tabs [role="tab"]')];
        return {
            path: location.pathname,
            book: !document.getElementById('page-book').hidden,
            reports: !document.getElementById('page-reports').hidden,
            registerHidden: document.getElementById('register').hidden,
            rows: document.querySelectorAll('#register-grid tbody tr').length,
            registerTitle: document.getElementById('register-title').textContent,
            tablistRole: document.getElementById('book-tabs').getAttribute('role'),
            tabs: tabs.map((tab) => ({
                id: tab.id,
                text: tab.textContent.trim(),
                selected: tab.getAttribute('aria-selected'),
                controls: tab.getAttribute('aria-controls'),
                controlsRole: tab.getAttribute('aria-controls')
                    ? (document.getElementById(tab.getAttribute('aria-controls')) || {})
                        .getAttribute?.('role') : null,
                labelledBy: tab.getAttribute('aria-controls')
                    ? (document.getElementById(tab.getAttribute('aria-controls')) || {})
                        .getAttribute?.('aria-labelledby') : null,
                tabindex: tab.getAttribute('tabindex'),
                hasClose: Boolean(tab.closest('.tab-slot')
                    ?.querySelector('.tab-close')),
            })),
            closers: document.querySelectorAll('#book-tabs .tab-close').length,
            frameHidden: document.getElementById('view').hidden,
            frameSrc: document.getElementById('view').getAttribute('src') || '',
            specName: document.getElementById('spec-name').value,
            designerHidden: document.getElementById('designer').hidden,
        };
    });
}

async function showAccountsTab() {
    if (await page.evaluate(() => document.getElementById('register').hidden))
        return;
    await page.locator('#tab-accounts').click();
    await page.waitForFunction(() => document.getElementById('register').hidden,
                               null, { timeout: 15000 });
}

// The page marks the document when the URL has been applied.  That is later
// than "the tree is there" (the book is read first) and, on a report path,
// later than the frame's first paint: it includes the report selection's own
// fetches, so the next navigation does not abort one of them.
const waitForRoute = () => page.waitForFunction(
    () => document.documentElement.dataset.routeReady === '1',
    null, { timeout: 30000 });

// Wait for the report frame's document (and its subresources, such as a
// chart's script) to finish, so navigating away does not abort one of them.
const waitForFrameSettled = () => page.waitForFunction(() => {
    const frame = document.getElementById('view');
    if (frame.hidden) return true;
    const doc = frame.contentDocument;
    return doc && doc.readyState === 'complete';
}, null, { timeout: 30000 });

async function openFromTree(entry) {
    await showAccountsTab();
    await page.locator(`#tree button.account-name[data-guid="${entry.account.guid}"]`)
        .click();
    await page.waitForFunction((title) =>
        !document.getElementById('register').hidden &&
        document.getElementById('register-title').textContent === title,
        entry.register.full_name, { timeout: 30000 });
}

// The frame's own document, once it has parsed.
async function waitForReportFrame(match) {
    await page.waitForFunction((wanted) => {
        const frame = document.getElementById('view');
        const doc = frame && frame.contentDocument;
        return frame && !frame.hidden && doc && doc.readyState === 'complete' &&
            doc.body && doc.body.innerText.trim().length > 100 &&
            (!wanted || wanted.test(frame.getAttribute('src') || ''));
    }, match, { timeout: 30000 });
}

/* ------------------------------------------------ direct navigation: /accounts */

await gotoWithRetry(pageUrl('accounts'), { waitUntil: 'domcontentloaded' });
await waitForRoute();
await page.waitForFunction(
    () => document.querySelectorAll('#tree li').length > 0 &&
          !document.getElementById('page-book').hidden,
    null, { timeout: 30000 });
let state = await appState();
check('a direct navigation to /accounts shows the Book page, Accounts tab',
      state.path === pathOf(pageUrl('accounts')) && state.book && !state.reports &&
      state.registerHidden && state.tabs.length === 1 &&
      state.tabs[0].selected === 'true',
      `${state.path}: book=${state.book} reports=${state.reports} ` +
      `tabs=${state.tabs.length}`);

check('the Accounts tab is a pinned ARIA tab',
      state.tablistRole === 'tablist' && state.tabs[0].text === 'Accounts' &&
      state.tabs[0].controls === 'panel-accounts' &&
      state.tabs[0].controlsRole === 'tabpanel' &&
      state.tabs[0].labelledBy === 'tab-accounts' &&
      state.tabs[0].tabindex === '0' && !state.tabs[0].hasClose &&
      state.closers === 0,
      `role=${state.tablistRole} controls=${state.tabs[0].controls}/` +
      `${state.tabs[0].controlsRole} labelledby=${state.tabs[0].labelledBy} ` +
      `close=${state.tabs[0].hasClose}`);

/* -------------------------------------- direct navigation: /register/<guid> */

await gotoWithRetry(pageUrl(`register/${first.account.guid}`),
                { waitUntil: 'domcontentloaded' });
await page.waitForFunction((title) =>
    !document.getElementById('register').hidden &&
    document.getElementById('register-title').textContent === title,
    first.register.full_name, { timeout: 30000 });
state = await appState();
check('a direct navigation to /register/<guid> opens that register tab',
      state.path === pathOf(pageUrl(`register/${first.account.guid}`)) &&
      state.book && !state.registerHidden &&
      state.rows === first.register.splits.length &&
      state.tabs.length === 2 && state.tabs[1].selected === 'true' &&
      state.tabs[1].text === first.account.name &&
      state.tabs[1].controls === `panel-register-${first.account.guid}` &&
      state.closers === 1,
      `${state.path}: rows=${state.rows} tab=${state.tabs[1]?.text} ` +
      `selected=${state.tabs[1]?.selected}`);

await page.reload({ waitUntil: 'domcontentloaded' });
await waitForRoute();
await page.waitForFunction((title) =>
    !document.getElementById('register').hidden &&
    document.getElementById('register-title').textContent === title,
    first.register.full_name, { timeout: 30000 });
state = await appState();
check('a reload of a register URL lands on the same register tab',
      state.path === pathOf(pageUrl(`register/${first.account.guid}`)) &&
      !state.registerHidden && state.rows === first.register.splits.length &&
      state.tabs.length === 2 && state.tabs[1].selected === 'true',
      `${state.path}: tabs=${state.tabs.map((tab) => tab.text).join('|')}`);

/* ------------------------------------------------ direct navigation: /reports */

await gotoWithRetry(pageUrl('reports'), { waitUntil: 'domcontentloaded' });
await waitForRoute();
await page.waitForFunction(
    () => document.querySelectorAll('#report-list a').length > 0 &&
          !document.getElementById('page-reports').hidden,
    null, { timeout: 30000 });
state = await appState();
check('a direct navigation to /reports shows the Reports page and no report',
      state.path === pathOf(pageUrl('reports')) && state.reports && !state.book &&
      state.frameHidden && state.frameSrc === '',
      `${state.path}: reports=${state.reports} frameHidden=${state.frameHidden}`);

// Opening a report from the list is reflected in the URL.
await page.locator('#report-list a', { hasText: 'Trial Balance' })
    .first().click();
await waitForReportFrame(/\/report\/Trial%20Balance/);
state = await appState();
check('opening a report from the list puts it in the URL',
      state.path === pathOf(pageUrl('report/Trial%20Balance')) &&
      state.reports && !state.frameHidden &&
      /\/report\/Trial%20Balance$/.test(state.frameSrc),
      `${state.path} frame=${state.frameSrc}`);

// The same page URL, reached directly rather than by a click.
await gotoWithRetry(pageUrl('report/Trial%20Balance'), { waitUntil: 'domcontentloaded' });
await waitForRoute();
await waitForReportFrame(/\/report\/Trial%20Balance$/);
state = await appState();
check('a direct navigation to /report/<name> opens that report',
      state.path === pathOf(pageUrl('report/Trial%20Balance')) &&
      state.reports && !state.frameHidden &&
      state.frameSrc.endsWith('/report/Trial%20Balance'),
      `${state.path} frame=${state.frameSrc}`);
await waitForFrameSettled();

// The frame receives the report document on the same path a navigation uses
// for the page: the report, not a nested copy of the shell.
const frameIsDocument = await page.evaluate(() => {
    const doc = document.getElementById('view').contentDocument;
    return {
        isShell: Boolean(doc && doc.getElementById('page-book')),
        hasTable: Boolean(doc && doc.querySelector('table')),
        title: doc ? doc.title : '',
    };
});
check('the report frame gets the report document, not the shell',
      !frameIsDocument.isShell && frameIsDocument.hasTable,
      `title=${JSON.stringify(frameIsDocument.title)} ` +
      `shell=${frameIsDocument.isShell}`);

// The harness path: a subresource read of the same URL is the document.
const fetchedReport = await page.evaluate(async (target) => {
    const response = await fetch(target);
    return { status: response.status, text: await response.text() };
}, pageUrl('report/Trial%20Balance'));
check('/report/<name> still answers the report document to a direct read',
      fetchedReport.status === 200 &&
      /<table/i.test(fetchedReport.text) &&
      !fetchedReport.text.includes('id="page-book"'),
      `${fetchedReport.status}, ${fetchedReport.text.length} bytes`);

/* ------------------------------------- direct navigation: /report/spec/<id> */

await gotoWithRetry(pageUrl(`report/spec/${builtin.id}`), { waitUntil: 'domcontentloaded' });
await waitForRoute();
await page.waitForFunction((id) => {
    const frame = document.getElementById('view');
    const doc = frame && frame.contentDocument;
    return !document.getElementById('page-reports').hidden &&
        !frame.hidden && frame.getAttribute('src') &&
        frame.getAttribute('src').endsWith('/report/spec/' + id) &&
        doc && doc.readyState === 'complete' && doc.body &&
        doc.body.innerText.trim().length > 0;
}, builtin.id, { timeout: 30000 });
state = await appState();
check('a direct navigation to /report/spec/<id> opens that saved report',
      state.path === pathOf(pageUrl(`report/spec/${builtin.id}`)) &&
      state.reports && !state.frameHidden && state.specName === builtin.name,
      `${state.path}: spec=${JSON.stringify(state.specName)}`);

await page.reload({ waitUntil: 'domcontentloaded' });
await waitForRoute();
await page.waitForFunction((id) => {
    const frame = document.getElementById('view');
    return !frame.hidden && frame.getAttribute('src') &&
        frame.getAttribute('src').endsWith('/report/spec/' + id);
}, builtin.id, { timeout: 30000 });
await waitForFrameSettled();
await page.waitForFunction((name) =>
    document.getElementById('spec-name').value === name, builtin.name,
    { timeout: 30000 });
state = await appState();
check('a reload of a saved-report URL lands on the same report',
      state.path === pathOf(pageUrl(`report/spec/${builtin.id}`)) &&
      state.specName === builtin.name && !state.frameHidden,
      `${state.path}: spec=${JSON.stringify(state.specName)}`);

const fetchedSpec = await page.evaluate(async (target) => {
    const response = await fetch(target);
    return { status: response.status, text: await response.text() };
}, pageUrl(`report/spec/${builtin.id}`));
check('/report/spec/<id> still answers the document to a direct read',
      fetchedSpec.status === 200 && /<table/i.test(fetchedSpec.text) &&
      !fetchedSpec.text.includes('id="page-book"'),
      `${fetchedSpec.status}, ${fetchedSpec.text.length} bytes`);

/* ------------------------------------------------------------- back/forward */

await gotoWithRetry(pageUrl('accounts'), { waitUntil: 'domcontentloaded' });
await waitForRoute();
await page.waitForFunction(() => document.querySelectorAll('#tree li').length > 0);
await openFromTree(first);
await showAccountsTab();
state = await appState();
check('the Accounts tab and a register tab are two URLs in history',
      state.path === pathOf(pageUrl('accounts')) && state.tabs.length === 2,
      `${state.path}: tabs=${state.tabs.length}`);

await page.goBack({ waitUntil: 'domcontentloaded' });
await page.waitForFunction((title) =>
    !document.getElementById('register').hidden &&
    document.getElementById('register-title').textContent === title,
    first.register.full_name, { timeout: 30000 });
state = await appState();
check('Back returns to the register URL',
      state.path === pathOf(pageUrl(`register/${first.account.guid}`)) &&
      !state.registerHidden && state.tabs[1].selected === 'true',
      `${state.path}: selected=${state.tabs[1]?.selected}`);

await page.goForward({ waitUntil: 'domcontentloaded' });
await page.waitForFunction(() => document.getElementById('register').hidden,
                           null, { timeout: 30000 });
state = await appState();
check('Forward returns to the Accounts URL',
      state.path === pathOf(pageUrl('accounts')) && state.registerHidden &&
      state.tabs[0].selected === 'true',
      `${state.path}: selected=${state.tabs[0]?.selected}`);

/* ------------------------------------------------------------------- tabs */

// Two register tabs, opened the way a reader opens them.
await openFromTree(second);
state = await appState();
check('opening a second account adds a second register tab',
      state.tabs.length === 3 && state.closers === 2 &&
      state.tabs[2].selected === 'true' && state.tabs[2].text === second.account.name,
      state.tabs.map((tab) => `${tab.text}:${tab.selected}`).join(' | '));

// Every tab is a proper ARIA tab with its panel.
const aria = await page.evaluate(() => {
    const tabs = [...document.querySelectorAll('#book-tabs [role="tab"]')];
    return tabs.map((tab) => {
        const panel = document.getElementById(tab.getAttribute('aria-controls'));
        return {
            selected: tab.getAttribute('aria-selected'),
            tabindex: tab.getAttribute('tabindex'),
            panel: Boolean(panel),
            panelRole: panel && panel.getAttribute('role'),
            labelled: panel && panel.getAttribute('aria-labelledby') === tab.id,
        };
    });
});
check('every tab is selected or not, one tab stop, and controls its panel',
      aria.every((tab) => (tab.selected === 'true') === (tab.tabindex === '0')) &&
      aria.filter((tab) => tab.selected === 'true').length === 1 &&
      aria.every((tab) => tab.panel && tab.panelRole === 'tabpanel' && tab.labelled),
      JSON.stringify(aria));

// Switching to the first register tab shows its register again.
await page.locator(`#tab-register-${first.account.guid}`).click();
await page.waitForFunction((title) =>
    document.getElementById('register-title').textContent === title,
    first.register.full_name, { timeout: 30000 });
state = await appState();
check('clicking a register tab switches to that account and URL',
      state.path === pathOf(pageUrl(`register/${first.account.guid}`)) &&
      state.tabs[1].selected === 'true' && state.tabs[2].selected === 'false',
      `${state.path}: ${state.tabs.map((tab) => tab.selected).join(',')}`);

// Ctrl+Tab cycles from the first register tab to the second (the order is
// Accounts, first, second).  Focus is on the tab button, as after a click.
await page.keyboard.press('Control+Tab');
await page.waitForFunction((title) =>
    document.getElementById('register-title').textContent === title,
    second.register.full_name, { timeout: 15000 });
state = await appState();
check('Ctrl+Tab cycles to the next tab and puts it in the URL',
      state.path === pathOf(pageUrl(`register/${second.account.guid}`)) &&
      state.tabs[2].selected === 'true' && state.tabs[1].selected === 'false',
      `${state.path}: ${state.tabs.map((tab) => tab.selected).join(',')}`);

await page.keyboard.press('Control+Shift+Tab');
await page.waitForFunction((title) =>
    document.getElementById('register-title').textContent === title,
    first.register.full_name, { timeout: 15000 });
state = await appState();
check('Ctrl+Shift+Tab cycles back in the other direction',
      state.path === pathOf(pageUrl(`register/${first.account.guid}`)) &&
      state.tabs[1].selected === 'true',
      `${state.path}: ${state.tabs.map((tab) => tab.text + ':' + tab.selected).join('|')}`);

// Arrow keys move between tabs inside the tablist, wrapping at the end.
const focusedBefore = await page.evaluate(() => document.activeElement.id);
await page.locator(`#tab-register-${second.account.guid}`).focus();
await page.keyboard.press('ArrowRight');
await page.waitForFunction(() => document.getElementById('register').hidden,
                           null, { timeout: 15000 });
state = await appState();
check('ArrowRight in the tablist selects the next tab and moves focus to it',
      state.tabs[0].selected === 'true' &&
      await page.evaluate(() => document.activeElement.id) === 'tab-accounts',
      `from ${focusedBefore} to ${await page.evaluate(() => document.activeElement.id)}`);

// Closing a register tab.
await page.locator(`#tab-register-${second.account.guid}`).click();
await page.waitForFunction((title) =>
    document.getElementById('register-title').textContent === title,
    second.register.full_name, { timeout: 30000 });
const closersBefore = (await appState()).closers;
await page.locator(`#tab-register-${second.account.guid}`)
    .locator('xpath=following-sibling::button[contains(@class,"tab-close")]')
    .click();
await page.waitForFunction((guid) =>
    !document.getElementById(`tab-register-${guid}`), second.account.guid,
    { timeout: 15000 });
state = await appState();
check('closing a register tab removes it from the tab list',
      state.closers === closersBefore - 1 &&
      !state.tabs.some((tab) => tab.text === second.account.name),
      `${state.closers} closers, tabs=${state.tabs.map((tab) => tab.text).join('|')}`);

// Closing the active tab falls back to a neighbour, not a blank page.
await page.locator(`#tab-register-${first.account.guid}`).click();
await page.waitForFunction((title) =>
    document.getElementById('register-title').textContent === title,
    first.register.full_name, { timeout: 30000 });
await page.locator(`#tab-register-${first.account.guid}`)
    .locator('xpath=following-sibling::button[contains(@class,"tab-close")]')
    .click();
await page.waitForFunction(() =>
    document.getElementById('register').hidden, null, { timeout: 15000 });
state = await appState();
check('closing the active register tab falls back to the Accounts tab',
      state.tabs.length === 1 && state.tabs[0].selected === 'true' &&
      state.registerHidden && state.path === pathOf(pageUrl('accounts')),
      `${state.path}: tabs=${state.tabs.length}`);

/* ------------------------------------------- the tab strip across a reload */

await openFromTree(first);
await page.reload({ waitUntil: 'domcontentloaded' });
await waitForRoute();
await page.waitForFunction((title) =>
    !document.getElementById('register').hidden &&
    document.getElementById('register-title').textContent === title,
    first.register.full_name, { timeout: 30000 });
state = await appState();
check('the open tab strip and the active tab survive a reload',
      state.tabs.length === 2 && state.tabs[1].selected === 'true' &&
      state.tabs[1].text === first.account.name,
      state.tabs.map((tab) => `${tab.text}:${tab.selected}`).join(' | '));

/* ------------------------------------------------------- menu navigation */

async function openMenu(id) {
    await page.locator(`#menubar .menu[data-menu="${id}"] > .menu-title`).click();
    await page.waitForFunction((menu) => {
        const popup = document.querySelector(
            `#menubar .menu[data-menu="${menu}"] > .menu-popup`);
        return popup && !popup.hidden;
    }, id, { timeout: 15000 });
}

// View > New Accounts Page shows the pinned Accounts tab.
await openMenu('view');
await page.locator('#menubar .menu[data-menu="view"] [data-item="new-accounts-page"]')
    .click();
await page.waitForFunction(() => document.getElementById('register').hidden,
                           null, { timeout: 15000 });
state = await appState();
check('View > New Accounts Page goes to the Accounts tab and its URL',
      state.path === pathOf(pageUrl('accounts')) && state.book &&
      state.tabs[0].selected === 'true', state.path);

// View > Reports goes to the Reports page.
await openMenu('view');
await page.locator('#menubar .menu[data-menu="view"] [data-item="view-reports"]')
    .click();
await page.waitForFunction(() => !document.getElementById('page-reports').hidden,
                           null, { timeout: 15000 });
state = await appState();
check('View > Reports goes to the Reports page and its URL',
      state.path === pathOf(pageUrl('reports')) && state.reports &&
      !state.book, state.path);

// A page-scoped item whose page is absent names the page; the same item with
// its page on screen says the action is not implemented, which is the truth.
await openMenu('view');
const ledgerOffPage = await page.evaluate(() => {
    const item = document.querySelector(
        '#menubar .menu[data-menu="view"] [data-item="basic-ledger"]');
    return item
        ? { disabled: item.getAttribute('aria-disabled'),
            reason: item.getAttribute('aria-description') }
        : null;
});
await page.keyboard.press('Escape');
const ledgerOnPage = await (async () => {
    await gotoWithRetry(pageUrl(`register/${first.account.guid}`),
                    { waitUntil: 'domcontentloaded' });
    await page.waitForFunction(() => !document.getElementById('register').hidden,
                               null, { timeout: 30000 });
    await openMenu('view');
    const item = await page.evaluate(() => {
        const node = document.querySelector(
            '#menubar .menu[data-menu="view"] [data-item="basic-ledger"]');
        return node
            ? { disabled: node.getAttribute('aria-disabled'),
                reason: node.getAttribute('aria-description') }
            : null;
    });
    await page.keyboard.press('Escape');
    return item;
})();
check('a register-scoped item says "no web equivalent" once a register tab is open',
      ledgerOffPage && ledgerOnPage &&
      /Register tab only/.test(ledgerOffPage.reason || '') &&
      /No web equivalent/.test(ledgerOnPage.reason || ''),
      `off=${ledgerOffPage?.reason?.slice(0, 40)} on=${ledgerOnPage?.reason?.slice(0, 40)}`);

// File > Close closes the active register tab and is disabled on the pinned
// Accounts tab.
await openMenu('file');
const closeState = await page.evaluate(() => {
    const item = document.querySelector(
        '#menubar .menu[data-menu="file"] [data-item="close-page"]');
    return { disabled: item.getAttribute('aria-disabled'),
             reason: item.getAttribute('aria-description') };
});
await page.locator('#menubar .menu[data-menu="file"] [data-item="close-page"]')
    .click();
await page.waitForFunction(() => document.getElementById('register').hidden,
                           null, { timeout: 15000 });
state = await appState();
check('File > Close closes the active register tab',
      closeState.disabled === 'false' && state.tabs.length === 1 &&
      state.path === pathOf(pageUrl('accounts')),
      `disabled=${closeState.disabled} tabs=${state.tabs.length}`);

await openMenu('file');
const closePinned = await page.evaluate(() => {
    const item = document.querySelector(
        '#menubar .menu[data-menu="file"] [data-item="close-page"]');
    return { disabled: item.getAttribute('aria-disabled'),
             reason: item.getAttribute('aria-description') };
});
await page.keyboard.press('Escape');
check('File > Close is disabled on the pinned Accounts tab with a reason',
      closePinned.disabled === 'true' && /pinned/.test(closePinned.reason || ''),
      `${closePinned.disabled}: ${closePinned.reason}`);

/* ------------------------------------- the Reports page layout (stage 2) */

// The pane widths are view furniture in sessionStorage; the page is a grid,
// so the rail and inspector scroll independently and the document fills the
// middle.  These checks run at the suite's 1280x900 viewport and drive the
// same controls a reader does.
const paneGeometry = () => page.evaluate(() => {
    const measure = (id) => {
        const node = document.getElementById(id);
        const rect = node.getBoundingClientRect();
        return {
            display: getComputedStyle(node).display,
            width: Math.round(rect.width),
            right: Math.round(rect.right),
        };
    };
    return {
        rail: measure('reports-rail'),
        inspector: measure('report-inspector'),
        document: measure('report-document'),
        designer: measure('designer'),
        shellClass: document.getElementById('reports-shell').className,
        stored: sessionStorage.getItem('gnucash-webui.reports-panes.v1'),
        overflow: document.documentElement.scrollWidth - innerWidth,
    };
});

await gotoWithRetry(pageUrl('reports'), { waitUntil: 'domcontentloaded' });
await waitForRoute();
await page.waitForFunction(
    () => document.querySelectorAll('#report-list a').length > 0,
    null, { timeout: 30000 });

let panes = await paneGeometry();
check('the Reports page has a rail, a document and an inspector',
      panes.rail.display !== 'none' && panes.rail.width >= 200 &&
      panes.inspector.display !== 'none' && panes.inspector.width >= 200 &&
      panes.document.width > 400 && panes.overflow <= 0,
      JSON.stringify(panes));

// Resizing the rail by keyboard is the same path as a drag, and writes the
// width to sessionStorage.  A reload must restore it.
await page.focus('#rail-resizer');
await page.keyboard.press('ArrowLeft');
const resizedWidth = (await paneGeometry()).rail.width;
await page.reload({ waitUntil: 'domcontentloaded' });
await waitForRoute();
await page.waitForFunction(
    () => document.querySelectorAll('#report-list a').length > 0,
    null, { timeout: 30000 });
panes = await paneGeometry();
const storedPanes = JSON.parse(panes.stored || '{}');
check('the rail width is remembered across a reload',
      resizedWidth < 272 && panes.rail.width === resizedWidth &&
      storedPanes.rail && storedPanes.rail.width === resizedWidth,
      `resized=${resizedWidth} reloaded=${panes.rail.width} ` +
      `stored=${panes.stored}`);

// `[` and `]` collapse and restore the panes; the document takes the space.
await page.evaluate(() => document.activeElement.blur());
await page.keyboard.press('[');
panes = await paneGeometry();
const railCollapsedWidth = panes.document.width;
check('`[` collapses the rail and gives its width to the document',
      panes.rail.display === 'none' && railCollapsedWidth > 600,
      `rail=${panes.rail.display} document=${railCollapsedWidth}`);
await page.keyboard.press('[');
panes = await paneGeometry();
check('`[` again restores the rail at its remembered width',
      panes.rail.display !== 'none' && panes.rail.width === resizedWidth,
      `rail=${panes.rail.width}`);

await page.evaluate(() => document.activeElement.blur());
await page.keyboard.press(']');
panes = await paneGeometry();
check('`]` collapses the inspector and gives its width to the document',
      panes.inspector.display === 'none' &&
      panes.document.width > railCollapsedWidth - 100,
      `inspector=${panes.inspector.display} document=${panes.document.width}`);
await page.keyboard.press(']');
panes = await paneGeometry();
check('`]` again restores the inspector',
      panes.inspector.display !== 'none' && panes.inspector.width >= 200,
      `inspector=${panes.inspector.width}`);

/* ---------------------------------- applying options keeps the scroll */

// The common case: change the dates with the options beside the document.
// Neither the page nor the report frame may jump back to the top when the
// reader had scrolled; when they were at the top they must stay there.
await gotoWithRetry(pageUrl('report/Trial%20Balance'),
                { waitUntil: 'domcontentloaded' });
await waitForRoute();
await waitForReportFrame(/\/report\/Trial%20Balance$/);
await page.waitForSelector('#options-form input[name="end"]',
                           { timeout: 30000 });
const tbDescriptor = await (await fetch(
    new URL('api/report/Trial%20Balance/options', url))).json();
const dateOptions = Object.fromEntries(
    tbDescriptor.options.map((option) => [option.name, option.default]));
const reportRows = () => page.evaluate(() => {
    const doc = document.getElementById('view').contentDocument;
    return doc ? doc.querySelectorAll('table tbody tr').length : -1;
});
const statusText = () => page.evaluate(
    () => document.getElementById('report-statusbar').innerText);
const rowsAtTop = await reportRows();
let status = await statusText();
check('the status bar counts the report rows and names the dates used',
      rowsAtTop > 3 && new RegExp(`Rows: ${rowsAtTop}\\b`).test(status) &&
      status.includes(String(dateOptions.start)) &&
      status.includes(String(dateOptions.end)),
      `${rowsAtTop} rows: ${status.replace(/\n/g, ' | ')}`);

await page.fill('#options-form input[name="end"]', '2009-12-31');
await page.click('#options-submit');
// Wait for the frame's load handler as well as readyState: it is what
// restores the scroll position and updates the status bar.
await page.waitForFunction(() => {
    const frame = document.getElementById('view');
    const doc = frame && frame.contentDocument;
    return frame && /[?&]end=2009-12-31(&|$)/.test(frame.src) &&
        doc && doc.readyState === 'complete' &&
        document.getElementById('report-status-dates').textContent
            .includes('2009-12-31');
}, null, { timeout: 30000 });
const atTop = await page.evaluate(() => ({
    page: window.scrollY,
    frame: document.getElementById('view').contentWindow.scrollY,
}));
check('applying options at the top does not scroll the page or the document',
      atTop.page === 0 && atTop.frame === 0, JSON.stringify(atTop));

const scrolled = await page.evaluate(() => {
    const frame = document.getElementById('view');
    window.scrollTo(0, document.documentElement.scrollHeight);
    frame.contentWindow.scrollTo(0, 160);
    return { page: window.scrollY, frame: frame.contentWindow.scrollY };
});
check('the page and the report document can be scrolled away from the top',
      scrolled.frame > 100, JSON.stringify(scrolled));

await page.fill('#options-form input[name="end"]', '2008-12-31');
await page.click('#options-submit');
await page.waitForFunction(() => {
    const frame = document.getElementById('view');
    const doc = frame && frame.contentDocument;
    return frame && /[?&]end=2008-12-31(&|$)/.test(frame.src) &&
        doc && doc.readyState === 'complete' &&
        document.getElementById('report-status-dates').textContent
            .includes('2008-12-31');
}, null, { timeout: 30000 });
await page.waitForTimeout(80);
const afterApply = await page.evaluate(() => ({
    page: window.scrollY,
    frame: document.getElementById('view').contentWindow.scrollY,
}));
status = await statusText();
const rowsAfter = await reportRows();
check('applying options keeps the page and document scroll positions',
      afterApply.page === scrolled.page && afterApply.frame === scrolled.frame,
      `before=${JSON.stringify(scrolled)} after=${JSON.stringify(afterApply)}`);
check('the status bar follows the dates actually applied',
      status.includes('2008-12-31') &&
      status.includes(String(dateOptions.start)) &&
      new RegExp(`Rows: ${rowsAfter}\\b`).test(status),
      status.replace(/\n/g, ' | '));

/* ------------------------------- breadcrumb and search-list keyboard */

// The breadcrumb names the report and its first part is the way back.
const crumbText = () => page.evaluate(
    () => document.getElementById('report-breadcrumb').innerText
        .replace(/\s+/g, ' ').trim());
check('the breadcrumb is Reports › the report name',
      /^Reports\s*›\s*Trial Balance$/.test(await crumbText()),
      await crumbText());
await page.click('#crumb-reports');
await page.waitForFunction(() => {
    return location.pathname.endsWith('/reports') &&
        document.getElementById('view').hidden;
}, null, { timeout: 15000 });
check('the breadcrumb returns to the report list',
      pathOf(await page.evaluate(() => location.href)) === pathOf(pageUrl('reports')) &&
      (await crumbText()) === 'Reports' &&
      await page.evaluate(() => !document.getElementById('document-empty').hidden),
      await crumbText());

await page.keyboard.press('Control+k');
check('Ctrl+K focuses the report search box',
      await page.evaluate(() => document.activeElement.id) === 'report-search',
      await page.evaluate(() => document.activeElement.id));

await page.locator('#report-list a', { hasText: 'Trial Balance' }).first()
    .focus();
await page.keyboard.press('Enter');
await page.waitForFunction(() =>
    location.pathname.includes('/report/Trial%20Balance'), null,
    { timeout: 30000 });
check('Enter in the list opens the focused report',
      pathOf(await page.evaluate(() => location.href)) ===
          pathOf(pageUrl('report/Trial%20Balance')),
      await page.evaluate(() => location.pathname));
await waitForReportFrame(/\/report\/Trial%20Balance$/);

/* -------------------------- the designer as a full-window mode */

await page.waitForFunction(() => {
    const name = document.getElementById('spec-name');
    return name && name.value === 'Trial Balance (copy)';
}, null, { timeout: 30000 });
await page.click('#edit-in-designer');
await page.waitForFunction(() => {
    return document.getElementById('reports-shell').classList
        .contains('designer-mode') &&
        !document.getElementById('designer').hidden;
}, null, { timeout: 15000 });
panes = await paneGeometry();
check('the designer opens as a full-window mode of the Reports page',
      new URLSearchParams(await page.evaluate(() => location.search))
          .get('designer') === '1' &&
      panes.rail.display === 'none' && panes.inspector.display === 'none' &&
      panes.designer.width > 400 && panes.document.width > 500 &&
      await page.evaluate(() => !document.getElementById('preview-label').hidden),
      JSON.stringify(panes));

// The mode is URL state: a reload lands in the designer with the report's
// spec still in the form.
const designerName = await page.inputValue('#spec-name');
await page.reload({ waitUntil: 'domcontentloaded' });
await waitForRoute();
await page.waitForFunction((wanted) => {
    return !document.getElementById('designer').hidden &&
        document.getElementById('spec-name').value === wanted;
}, designerName, { timeout: 30000 });
check('a reload keeps the designer mode and its report selected',
      await page.evaluate(() => document.getElementById('reports-shell')
          .classList.contains('designer-mode')),
      designerName);

// Editing updates the preview without pressing Preview, and the status bar
// says the frame is a preview rather than a saved report.
const previewName = 'Layout gate preview';
await page.fill('#spec-name', previewName);
await page.fill('#spec-title', previewName);
await page.waitForFunction((wanted) => {
    const frame = document.getElementById('view');
    const doc = frame && frame.contentDocument;
    return frame && frame.hasAttribute('srcdoc') && doc &&
        doc.title === wanted;
}, previewName, { timeout: 30000 });
const previewState = await page.evaluate(() => ({
    dirty: !document.getElementById('designer-dirty').hidden,
    saved: document.getElementById('report-status-saved').textContent,
    srcdoc: document.getElementById('view').hasAttribute('srcdoc'),
}));
check('editing the designer form updates a live preview beside it',
      previewState.srcdoc && previewState.dirty &&
      /Preview — not saved/.test(previewState.saved),
      JSON.stringify(previewState));

// Leaving is guarded: Cancel keeps the form (work is not lost), OK leaves to
// the list.
page.once('dialog', (dialog) => dialog.dismiss());
await page.click('#designer-close');
await page.waitForTimeout(150);
check('cancelling the unsaved-work guard stays in the designer with the work',
      await page.evaluate(() => !document.getElementById('designer').hidden) &&
      await page.inputValue('#spec-name') === previewName,
      await page.inputValue('#spec-name'));
page.once('dialog', (dialog) => dialog.accept());
await page.click('#designer-close');
await page.waitForFunction(() => {
    return document.getElementById('designer').hidden &&
        location.pathname.endsWith('/reports');
}, null, { timeout: 15000 });
check('leaving the designer with OK returns to the report list',
      (await crumbText()) === 'Reports' &&
      await page.evaluate(() => !document.getElementById('document-empty').hidden),
      await page.evaluate(() => location.pathname + location.search));

/* ------------------------------- the layout at ~800px */

await page.setViewportSize({ width: 800, height: 900 });
await gotoWithRetry(pageUrl('report/Trial%20Balance'),
                { waitUntil: 'domcontentloaded' });
await waitForRoute();
await waitForReportFrame(/\/report\/Trial%20Balance$/);
await page.waitForTimeout(120);
const narrow = await page.evaluate(() => {
    const frame = document.getElementById('view').getBoundingClientRect();
    const status = document.getElementById('report-statusbar')
        .getBoundingClientRect();
    const doc = document.getElementById('report-document')
        .getBoundingClientRect();
    return {
        rail: getComputedStyle(document.getElementById('reports-rail')).display,
        inspector:
            getComputedStyle(document.getElementById('report-inspector')).display,
        documentWidth: Math.round(doc.width),
        frameWidth: Math.round(frame.width),
        frameHeight: Math.round(frame.height),
        statusBottom: Math.round(status.bottom),
        overflow: document.documentElement.scrollWidth - innerWidth,
        rows: document.getElementById('view').contentDocument
            .querySelectorAll('table tbody tr').length,
    };
});
check('at 800px the rail and inspector collapse and the document stays usable',
      narrow.rail === 'none' && narrow.inspector === 'none' &&
      narrow.documentWidth >= 700 && narrow.frameWidth >= 700 &&
      narrow.frameHeight >= 400 && narrow.statusBottom <= 900 &&
      narrow.overflow <= 0 && narrow.rows > 3,
      JSON.stringify(narrow));
await page.setViewportSize({ width: 1280, height: 900 });

/* --------------------------------------------------------- console gate */

for (const problem of problems) console.log(`page problem: ${problem}`);
check('no console errors or unexpected failed requests', problems.length === 0,
      `${problems.length} problems`);

if (process.env.UI_SCREENSHOT)
    await page.screenshot({ path: process.env.UI_SCREENSHOT });

await browser.close();
process.exit(failures === 0 ? 0 : 1);
