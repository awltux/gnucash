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

async function getJson(path) {
    const response = await fetch(new URL(path, url));
    if (!response.ok)
        throw new Error(`${path} answered ${response.status}: ${await response.text()}`);
    return response.json();
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

await page.goto(pageUrl('accounts'), { waitUntil: 'domcontentloaded' });
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

await page.goto(pageUrl(`register/${first.account.guid}`),
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

await page.goto(pageUrl('reports'), { waitUntil: 'domcontentloaded' });
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
await page.goto(pageUrl('report/Trial%20Balance'), { waitUntil: 'domcontentloaded' });
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

await page.goto(pageUrl(`report/spec/${builtin.id}`), { waitUntil: 'domcontentloaded' });
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

await page.goto(pageUrl('accounts'), { waitUntil: 'domcontentloaded' });
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
    await page.goto(pageUrl(`register/${first.account.guid}`),
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

/* --------------------------------------------------------- console gate */

for (const problem of problems) console.log(`page problem: ${problem}`);
check('no console errors or unexpected failed requests', problems.length === 0,
      `${problems.length} problems`);

if (process.env.UI_SCREENSHOT)
    await page.screenshot({ path: process.env.UI_SCREENSHOT });

await browser.close();
process.exit(failures === 0 ? 0 : 1);
