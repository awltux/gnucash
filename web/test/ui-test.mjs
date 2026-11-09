// Headless UI test for gnc-webd.
//
//   node web/test/ui-test.mjs <url> [screenshot.png]
//
// <url> is the address gnc-webd prints (it includes the token). The script
// drives a real browser, asserts on the DOM, opens a report and checks the
// rendered document, and fails on any console error or failed request.
//
// It also exercises the generated report options form: the form must be built
// from /api/report/<name>/options, changing an option must reload the frame,
// a rejected option must surface the server's message in the page, and a
// saved view must survive a reload.  The build without Guile lists only the
// native reports, so the report-count check adapts to the build.
//
// Needs playwright (or playwright-core) available on NODE_PATH; it uses the
// installed Chrome, so no browser download:
//
//   npm install playwright-core
//   NODE_PATH=./node_modules node web/test/ui-test.mjs <url>

import { createRequire } from 'node:module';

// CommonJS resolution, so NODE_PATH works and the harness can live in the repo
// while playwright-core stays a dev dependency somewhere outside it.
const { chromium } = createRequire(import.meta.url)('playwright-core');

const url = process.argv[2];
if (!url) {
    console.error('usage: node ui-test.mjs <url> [screenshot.png]');
    process.exit(2);
}
const screenshotPath = process.argv[3];

const browser = await chromium.launch({
    // PW_CHANNEL=chrome drives an installed browser (no download); without it
    // the bundled Chromium of the full `playwright` package is used.
    ...(process.env.PW_CHANNEL ? { channel: process.env.PW_CHANNEL } : {}),
    headless: true,
    // For containers that disable user namespaces.
    args: process.env.PW_NO_SANDBOX ? ['--no-sandbox'] : [],
});
const page = await browser.newPage({ viewport: { width: 1200, height: 900 } });

// A favicon miss is not a defect; anything else is.  The bad-date check below
// deliberately asks the server for a 400, and Chrome logs that as a console
// error, so that one URL is expected while the check runs.
let expectedBadDate = false;
const isExpectedRejection = (target) =>
    expectedBadDate && target.includes('end=2018-1-1');
const ignorable = (u) => /favicon\.ico$/.test(u);
const problems = [];
page.on('console', (m) => {
    if (m.type() !== 'error') return;
    if (isExpectedRejection(m.location()?.url || '')) return;
    problems.push(`console: ${m.text()}`);
});
page.on('requestfailed', (r) => {
    if (!ignorable(r.url())) problems.push(`request failed: ${r.url()} (${r.failure()?.errorText})`);
});
page.on('response', (r) => {
    if (r.status() >= 400 && !ignorable(r.url()) && !isExpectedRejection(r.url()))
        problems.push(`http ${r.status()}: ${r.url()}`);
});

let failures = 0;
function check(name, ok, detail = '') {
    console.log(`${ok ? 'ok  ' : 'FAIL'} ${name}${detail ? ` — ${detail}` : ''}`);
    if (!ok) failures++;
}

await page.goto(url, { waitUntil: 'domcontentloaded', timeout: 15000 });
await page.waitForFunction(
    () => document.getElementById('summary')?.textContent.includes('accounts'),
    null, { timeout: 15000 });
// The summary is filled after api/book; the tree and the report list arrive
// from two more requests afterwards, so wait for both before reading them.
await page.waitForFunction(
    () => document.querySelectorAll('#tree li').length > 0 &&
          document.querySelectorAll('#report-list a').length > 0,
    null, { timeout: 15000 });

const summary = await page.textContent('#summary');
check('summary shows the book size', /\d+ accounts, \d+ top level/.test(summary), summary);
check('the report frame starts hidden',
      await page.evaluate(() => document.getElementById('view').hidden),
      'no empty frame before a report is opened');

const rows = await page.locator('#tree li').count();
check('account tree rendered', rows > 50, `${rows} rows`);

const firstRow = (await page.locator('#tree li').first().innerText()).replace(/\s+/g, ' ');
check('balances are formatted', /\$[\d,]+\.\d\d/.test(firstRow), firstRow);

// A Guile build lists the Scheme report templates in addition to the native
// ones; the no-Guile build lists the native six.  Assert the strong count only
// where the full list exists, and the native set everywhere.
const reportNames = await page.evaluate(() =>
    [...document.querySelectorAll('#report-list a')].map((a) => a.textContent));
const nativeDescriptors =
    await (await fetch(new URL('api/reports?options=1', url))).json();
const nativeNames = new Set(nativeDescriptors.map((entry) => entry.report));
const hasSchemeReports = reportNames.some((name) => !nativeNames.has(name));
check('reports are listed',
      reportNames.length >= nativeNames.size &&
      (hasSchemeReports ? reportNames.length > 20 : reportNames.length > 0),
      `${reportNames.length} reports (${nativeNames.size} native` +
      `${hasSchemeReports ? ' + Scheme' : ''})`);

// Open a report the way a user does, then look inside the frame.
await page.locator('#report-list a', { hasText: 'Trial Balance' }).first().click();
await page.waitForFunction(() => {
    const frame = document.getElementById('view');
    return frame && !frame.hidden && frame.src;
}, null, { timeout: 15000 });

// frameLocator re-resolves on each use, so it survives the frame's navigation.
// Wait for the report's table: <body> exists before the document is parsed.
const frame = page.frameLocator('#view');
await frame.locator('tr').first().waitFor({ timeout: 15000 });
const reportText = (await frame.locator('body').innerText()).replace(/\s+/g, ' ');
check('report rendered in the frame', reportText.length > 1000, `${reportText.length} chars`);
check('report has its table', /Balance|Debit|Credit/i.test(reportText),
      reportText.slice(0, 120));

const rowCount = await frame.locator('tr').count();
check('report table has rows', rowCount > 3, `${rowCount} rows`);

// Drill-down: the report's account links point at the register page now, so
// clicking one should navigate the frame instead of going nowhere.
const registerLink = frame.locator('a[href*="/register/acct-guid="]').first();
check('report links to the register', (await registerLink.count()) > 0);
await registerLink.click();
await frame.locator('table').first().waitFor({ timeout: 15000 });
const registerTitle = (await frame.locator('h1').first().innerText()).trim();
check('register page opened', registerTitle.length > 0, registerTitle);
const balanceText = (await frame.locator('p.total').first().innerText()).trim();
check('register shows a balance', /-?[$€£]|[\d]/.test(balanceText), balanceText);

/* ------------------------------------------------- report option form */

// The page must build the form from the descriptors the server publishes, not
// from a per-report list in JavaScript.
const descriptor = await (await fetch(
    new URL('api/report/Trial%20Balance/options', url))).json();
const endDescriptor = descriptor.options.find(
    (option) => option.name === 'end' && option.type === 'date');
check('Trial Balance publishes a date descriptor',
      Boolean(endDescriptor) && /^\d{4}-\d{2}-\d{2}$/.test(endDescriptor.default),
      endDescriptor ? endDescriptor.default : 'no end descriptor');

await page.waitForSelector('#options-form input[name="end"]', { timeout: 15000 });
const endType = await page.getAttribute('#options-form input[name="end"]', 'type');
const endValue = await page.inputValue('#options-form input[name="end"]');
check('the date input comes from the descriptor',
      endType === 'date' && endValue === endDescriptor.default,
      `${endType} ${endValue}`);

const controlNames = await page.evaluate(() => {
    const form = document.getElementById('options-form');
    return [...form.querySelectorAll('[name]')].map((node) => node.name);
});
const missingControls = descriptor.options.filter((option) =>
    !['date', 'choice', 'currency', 'account-list'].includes(option.type) ||
    !controlNames.includes(option.name));
check('the form has a control for every descriptor',
      missingControls.length === 0,
      missingControls.map((option) => `${option.name}:${option.type}`).join(', ') ||
      controlNames.join(','));

const periodChoices = await page.evaluate(() => {
    const select = document.querySelector('#options-form select[name="period"]');
    return select ? [...select.options].map((option) => option.value) : [];
});
check('the period choices come from the descriptor',
      periodChoices.includes('') &&
      ['this-year', 'last-year', 'all'].every((choice) => periodChoices.includes(choice)),
      periodChoices.join(','));

// The currency descriptor has a default but no choices, so the form renders a
// validated text field rather than a one-entry select.
const currencyOption = descriptor.options.find((option) => option.name === 'currency');
const currencyValue = await page.inputValue('#options-form [name="currency"]');
check('the currency field starts at the descriptor default',
      currencyValue === currencyOption.default,
      `${currencyValue} (${currencyOption.choices ? 'select' : 'free text'})`);

// Change the end date to one outside the fixture's entries.  The server is
// the oracle: fetch the same URL the form submitted and compare the figures.
const beforeText = reportText;
await page.fill('#options-form input[name="end"]', '2009-12-31');
await page.click('#options-submit');
await page.waitForFunction(() => {
    const frame = document.getElementById('view');
    const doc = frame && frame.contentDocument;
    return frame && /[?&]end=2009-12-31(&|$)/.test(frame.src) &&
        doc && doc.contentType === 'text/html' &&
        doc.body && doc.body.innerText.includes('12/31/09');
}, null, { timeout: 15000 });
const afterText = (await frame.locator('body').innerText()).replace(/\s+/g, ' ');
const submittedUrl = await page.evaluate(() => document.getElementById('view').src);
const submittedHtml = await (await fetch(submittedUrl)).text();
check('changing the end date reloads the report',
      afterText !== beforeText,
      `${beforeText.length} -> ${afterText.length} chars`);
check('the reloaded figures match the server for that URL',
      submittedHtml.includes('12/31/09') &&
      !submittedHtml.includes('10,000.00') &&
      afterText.includes('12/31/09') &&
      !afterText.includes('10,000.00'),
      submittedHtml.includes('10,000.00') ? 'server still shows 10,000.00'
                                          : 'server and frame agree on 12/31/09');

/* --------------------------------------------------------- saved views */

await page.fill('#view-name', 'End of 2009');
await page.click('#save-view');
await page.waitForFunction(() =>
    [...document.querySelectorAll('#saved-views button.apply-view')]
        .some((button) => button.textContent.trim() === 'End of 2009'),
    null, { timeout: 15000 });
const savedViews = await page.locator(
    '#saved-views button.apply-view', { hasText: 'End of 2009' }).count();
check('a named view is saved', savedViews === 1, `${savedViews} matching view`);

await page.reload({ waitUntil: 'domcontentloaded' });
await page.waitForFunction(
    () => document.getElementById('summary')?.textContent.includes('accounts'),
    null, { timeout: 15000 });
await page.locator('#report-list a', { hasText: 'Trial Balance' }).first().click();
await page.waitForSelector('#options-form input[name="end"]', { timeout: 15000 });
await page.waitForFunction(() =>
    [...document.querySelectorAll('#saved-views button.apply-view')]
        .some((button) => button.textContent.trim() === 'End of 2009'),
    null, { timeout: 15000 });
const reloadedViews = await page.locator(
    '#saved-views button.apply-view', { hasText: 'End of 2009' }).count();
check('the saved view survives a page reload', reloadedViews === 1,
      `${reloadedViews} matching view in localStorage`);

await page.locator('#saved-views button.apply-view', { hasText: 'End of 2009' })
    .first().click();
await page.waitForFunction(() => {
    const frame = document.getElementById('view');
    const doc = frame && frame.contentDocument;
    return frame && /[?&]end=2009-12-31(&|$)/.test(frame.src) &&
        doc && doc.contentType === 'text/html' &&
        doc.body && doc.body.innerText.includes('12/31/09');
}, null, { timeout: 15000 });
const appliedEnd = await page.inputValue('#options-form input[name="end"]');
const appliedText = (await frame.locator('body').innerText()).replace(/\s+/g, ' ');
check('the saved view applies its options',
      appliedEnd === '2009-12-31' && appliedText.includes('12/31/09'),
      `end=${appliedEnd}`);

await page.locator('#saved-views li', { hasText: 'End of 2009' })
    .locator('button.delete-view').click();
const remainingViews = await page.locator(
    '#saved-views button.apply-view', { hasText: 'End of 2009' }).count();
const storedViews = await page.evaluate(
    () => localStorage.getItem('gnucash-webui.saved-views.v1'));
check('deleting a saved view removes it',
      remainingViews === 0 && !(storedViews || '').includes('End of 2009'),
      `${remainingViews} left, store=${storedViews}`);

// A period preset replaces the dates; the server rejects the two together, so
// the form must send only the preset (and still send the currency).
await page.selectOption('#options-form select[name="period"]', 'all');
await page.click('#options-submit');
await page.waitForFunction(() => {
    const frame = document.getElementById('view');
    const doc = frame && frame.contentDocument;
    return frame && /[?&]period=all(&|$)/.test(frame.src) &&
        doc && doc.contentType === 'text/html' &&
        doc.body && doc.body.innerText.includes('All Dates');
}, null, { timeout: 15000 });
const periodUrl = await page.evaluate(() => document.getElementById('view').src);
check('a period preset replaces the dates',
      /[?&]period=all(&|$)/.test(periodUrl) && !/[?&]start=/.test(periodUrl),
      periodUrl);

/* ------------------------------------------------- account selection */

// The transaction report and the general ledger take an account list; the
// checkbox value must become the GUID the server understands.
await page.locator('#report-list a', { hasText: 'Transaction Report' }).first().click();
await page.waitForSelector('#options-form input[name="accounts"]', { timeout: 15000 });
const accountLabel = page.locator('#options-form label', { hasText: 'Checking Account' }).first();
const accountGuid = await accountLabel.locator('input[type="checkbox"]').getAttribute('value');
await accountLabel.locator('input[type="checkbox"]').check();
await page.fill('#options-form input[name="start"]', '2018-01-01');
await page.fill('#options-form input[name="end"]', '2018-02-23');
await page.click('#options-submit');
await page.waitForFunction(() => {
    const frame = document.getElementById('view');
    const doc = frame && frame.contentDocument;
    return doc && doc.contentType === 'text/html' &&
        doc.body && /Checking Account/.test(doc.body.innerText) &&
        !/No accounts selected/.test(doc.body.innerText);
}, null, { timeout: 15000 });
const transactionUrl = await page.evaluate(() => document.getElementById('view').src);
const transactionHtml = await (await fetch(transactionUrl)).text();
check('an account checkbox becomes an account GUID',
      transactionUrl.includes(`accounts=${accountGuid}`) &&
      transactionHtml.includes('Checking Account') &&
      !transactionHtml.includes('No accounts selected'),
      accountGuid);

/* ------------------------------------------------- rejected options */

// A native date input cannot hold 2018-1-1 (the browser blanks it), so drop
// the picker to submit the malformed string a hand-edited request would send.
// The point of the check is that the page shows the server's message.
await page.locator('#report-list a', { hasText: 'Trial Balance' }).first().click();
await page.waitForSelector('#options-form input[name="end"]', { timeout: 15000 });
await page.locator('#options-form input[name="end"]').evaluate((input) => {
    input.type = 'text';
    input.value = '2018-1-1';
});
expectedBadDate = true;
await page.click('#options-submit');
await page.waitForSelector('#report-error:not([hidden])', { timeout: 15000 });
const errorText = (await page.textContent('#report-error')).trim();
check('a bad date surfaces the server error',
      /invalid end date '2018-1-1' \(expected YYYY-MM-DD\)/.test(errorText),
      errorText);
check('the rejected request does not leave a blank frame',
      await page.evaluate(() => document.getElementById('view').hidden),
      'frame hidden behind the message');
expectedBadDate = false;

if (screenshotPath) {
    await page.screenshot({ path: screenshotPath });
    console.log(`screenshot: ${screenshotPath}`);
}

for (const p of problems) console.log(`page problem: ${p}`);
check('no console errors or failed requests', problems.length === 0, `${problems.length} problems`);

await browser.close();
process.exit(failures === 0 ? 0 : 1);
