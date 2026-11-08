// Headless UI test for gnc-webd.
//
//   node web/test/ui-test.mjs <url> [screenshot.png]
//
// <url> is the address gnc-webd prints (it includes the token). The script
// drives a real browser, asserts on the DOM, opens a report and checks the
// rendered document, and fails on any console error or failed request.
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

// A favicon miss is not a defect; anything else is.
const ignorable = (u) => /favicon\.ico$/.test(u);
const problems = [];
page.on('console', (m) => {
    if (m.type() === 'error') problems.push(`console: ${m.text()}`);
});
page.on('requestfailed', (r) => {
    if (!ignorable(r.url())) problems.push(`request failed: ${r.url()} (${r.failure()?.errorText})`);
});
page.on('response', (r) => {
    if (r.status() >= 400 && !ignorable(r.url())) problems.push(`http ${r.status()}: ${r.url()}`);
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

const summary = await page.textContent('#summary');
check('summary shows the book size', /\d+ accounts, \d+ top level/.test(summary), summary);

const rows = await page.locator('#tree li').count();
check('account tree rendered', rows > 50, `${rows} rows`);

const firstRow = (await page.locator('#tree li').first().innerText()).replace(/\s+/g, ' ');
check('balances are formatted', /\$[\d,]+\.\d\d/.test(firstRow), firstRow);

const reportCount = await page.locator('#report-list a').count();
check('reports are listed', reportCount > 20, `${reportCount} reports`);

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

if (screenshotPath) {
    await page.screenshot({ path: screenshotPath });
    console.log(`screenshot: ${screenshotPath}`);
}

for (const p of problems) console.log(`page problem: ${p}`);
check('no console errors or failed requests', problems.length === 0, `${problems.length} problems`);

await browser.close();
process.exit(failures === 0 ? 0 : 1);
