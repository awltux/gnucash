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
// ones; the no-Guile build lists the native ten plus the built-in specs.
// The built-ins are named in /api/reports too, so exclude them before asking
// whether any Scheme-only report is present, and assert the strong count only
// where the full Scheme list exists.
const reportNames = await page.evaluate(() =>
    [...document.querySelectorAll('#report-list a')].map((a) => a.textContent));
const nativeDescriptors =
    await (await fetch(new URL('api/reports?options=1', url))).json();
const nativeNames = new Set(nativeDescriptors.map((entry) => entry.report));
const specList = await (await fetch(new URL('api/specs', url))).json();
const builtinNames = new Set((specList.builtin || []).map((spec) => spec.name));
const hasSchemeReports =
    reportNames.some((name) => !nativeNames.has(name) && !builtinNames.has(name));
check('reports are listed',
      reportNames.length >= nativeNames.size + builtinNames.size &&
      (hasSchemeReports ? reportNames.length > 20 : reportNames.length > 0),
      `${reportNames.length} reports (${nativeNames.size} native, ` +
      `${builtinNames.size} built-in` +
      `${hasSchemeReports ? ' + Scheme' : ''})`);

/* ------------------------------------------------- built-in chart specs */

// The built-ins are designer specs, not hard-coded reports: the list entry
// must link to the spec route, say what it is scoped to, and offer no delete
// control.  The income/expenses one carries two chart series.
const builtinLinks = await page.locator('#report-list a.builtin-report-link').count();
check('the built-in specs are listed', builtinLinks === builtinNames.size,
      `${builtinLinks} built-in links for ${builtinNames.size} specs`);
const builtinDeletes = await page.locator(
    '#report-list li.builtin-report button.delete-spec').count();
check('built-in reports have no delete control', builtinDeletes === 0,
      `${builtinDeletes} delete buttons`);

await page.locator('#report-list a.builtin-report-link',
                  { hasText: 'Income vs Expenses by Month' }).first().click();
await page.waitForFunction(() => {
    const frame = document.getElementById('view');
    const doc = frame && frame.contentDocument;
    return doc && doc.getElementById('report-chart-canvas') &&
        doc.defaultView && (doc.defaultView.__reportCharts || []).length > 0;
}, null, { timeout: 15000 });
const builtinChart = await page.evaluate(() => {
    const doc = document.getElementById('view').contentDocument;
    const canvas = doc.getElementById('report-chart-canvas');
    const dataNode = doc.getElementById('report-chart-data');
    let data = null;
    try { data = JSON.parse(dataNode.textContent); } catch (err) { data = null; }
    const pixels = canvas.getContext('2d')
        .getImageData(0, 0, canvas.width, canvas.height).data;
    let drawn = 0;
    for (let index = 3; index < pixels.length; index += 4)
        if (pixels[index] !== 0) drawn++;
    return {
        data,
        drawn,
        image: canvas.toDataURL(),
        note: document.getElementById('options-note').textContent,
        charts: (doc.defaultView.__reportCharts || []).length,
    };
});
check('a built-in spec renders its own chart',
      builtinChart.data && builtinChart.data.type === 'line' &&
      builtinChart.data.series.length === 2 &&
      builtinChart.data.series.map((series) => series.label).join(',') ===
          'Credit,Debit' &&
      builtinChart.data.labels.length > 0,
      builtinChart.data
        ? `${builtinChart.data.type}, ${builtinChart.data.labels.length} labels, ` +
          builtinChart.data.series.map((series) =>
            `${series.label}=${series.values.join('/')}`).join(' ')
        : 'no chart data');
check('the built-in chart canvas drew content',
      builtinChart.charts === 1 && builtinChart.drawn > 1000,
      `${builtinChart.drawn} non-blank pixels`);
check('the UI says the built-in is scoped',
      /scoped to Income and Expense accounts/.test(builtinChart.note),
      builtinChart.note);

// The chart has to come from the spec's own rows, not a stored image: build
// the same spec with an explicit period that moves the numbers, preview it
// through the server, and require both the data and the pixels to change.
const widerSpec = {
    name: 'Wider income and expenses',
    columns: ['account-name', 'debit', 'credit'],
    grouping: 'period',
    period_unit: 'month',
    start: '2026-01-01',
    end: '2026-12-31',
    account_types: ['INCOME', 'EXPENSE'],
    chart: 'line',
    chart_series: ['credit', 'debit'],
};
const widerHtml = await page.evaluate(async (spec) => {
    const response = await fetch(new URL('api/spec/preview', location.href), {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(spec),
    });
    return { status: response.status, text: await response.text() };
}, widerSpec);
check('a hand-built period chart previews', widerHtml.status === 200,
      `HTTP ${widerHtml.status}`);

const widerChart = await page.evaluate(async (arg) => {
    const frame = document.getElementById('view');
    // The frame's previous document still has a chart; drop it so the wait
    // below cannot mistake the old report for the new one.
    if (frame.contentDocument && frame.contentDocument.defaultView)
        frame.contentDocument.defaultView.__reportCharts = [];
    frame.removeAttribute('src');
    frame.srcdoc = arg.html;
    const ready = async () => {
        for (let attempt = 0; attempt < 150; attempt++) {
            const doc = frame.contentDocument;
            if (doc && doc.title === arg.title &&
                doc.getElementById('report-chart-canvas') &&
                doc.defaultView && doc.defaultView.__reportCharts.length > 0)
                return doc;
            await new Promise((resolve) => setTimeout(resolve, 100));
        }
        return null;
    };
    const doc = await ready();
    if (!doc) return null;
    const canvas = doc.getElementById('report-chart-canvas');
    const data = JSON.parse(
        doc.getElementById('report-chart-data').textContent);
    const pixels = canvas.getContext('2d')
        .getImageData(0, 0, canvas.width, canvas.height).data;
    let drawn = 0;
    for (let index = 3; index < pixels.length; index += 4)
        if (pixels[index] !== 0) drawn++;
    return { data, drawn, image: canvas.toDataURL() };
}, { html: widerHtml.text, title: widerSpec.name });
check('the chart changes when the period and numbers change',
      widerChart && widerChart.data.labels.length === 12 &&
      widerChart.data.series.every((series) =>
          series.values.every((value) => value === 0)) &&
      widerChart.drawn > 1000 &&
      widerChart.image !== builtinChart.image,
      widerChart
        ? `${widerChart.data.labels.length} labels, ` +
          `drawn=${widerChart.drawn}, ` +
          `image ${widerChart.image === builtinChart.image ? 'same' : 'different'}`
        : 'no chart');

/* --------------------------------------------------------- stage 4 chart */

// The balance sheet is the native report with a chart section.  A canvas
// element alone would prove nothing, so wait for Chart.js to register the
// chart and then read the pixels back out of the canvas.
await page.locator('#report-list a', { hasText: 'Balance Sheet' }).first().click();
await page.waitForFunction(() => {
    const frame = document.getElementById('view');
    const doc = frame && frame.contentDocument;
    return doc && doc.title.indexOf('Balance Sheet') === 0 &&
        doc.getElementById('report-chart-canvas') &&
        doc.defaultView && (doc.defaultView.__reportCharts || []).length > 0;
}, null, { timeout: 15000 });

const chart = await page.evaluate(() => {
    const doc = document.getElementById('view').contentDocument;
    const canvas = doc.getElementById('report-chart-canvas');
    const dataNode = doc.getElementById('report-chart-data');
    let data = null;
    try { data = JSON.parse(dataNode.textContent); } catch (err) { data = null; }
    const pixels = canvas.getContext('2d')
        .getImageData(0, 0, canvas.width, canvas.height).data;
    let drawn = 0;
    for (let index = 3; index < pixels.length; index += 4)
        if (pixels[index] !== 0) drawn++;
    return {
        width: canvas.width,
        height: canvas.height,
        drawn,
        data,
        charts: (doc.defaultView.__reportCharts || []).length,
        chartjs: typeof doc.defaultView.Chart,
    };
});
check('the report HTML has a canvas and its data',
      chart.width === 640 && chart.height === 320 &&
      chart.data && Array.isArray(chart.data.labels) &&
      chart.data.labels.length === 3 &&
      chart.data.series.length === 1 &&
      chart.data.series[0].values.length === 3,
      `${chart.width}x${chart.height}, ` +
      `${chart.data ? chart.data.labels.join('/') : 'no data'}`);
check('the chart canvas actually drew content',
      chart.chartjs === 'function' && chart.charts === 1 &&
      chart.drawn > 5000,
      `${chart.drawn} non-blank pixels, Chart.js=${chart.chartjs}`);

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

/* ------------------------------------------------------ stage 5 designer */

// Build a spec equivalent to an explicit Trial Balance request, inside the
// UI, and require the rendered table to be the same table: a designer that
// silently produced different numbers would pass every weaker check.
await page.waitForSelector('#spec-columns input[value="debit"]',
                           { timeout: 15000 });
const specVocabulary = await page.evaluate(() => ({
    columns: [...document.querySelectorAll('#spec-columns input')]
        .map((node) => node.value),
    groupings: [...document.querySelectorAll('#spec-grouping option')]
        .map((node) => node.value),
    options: [...document.querySelectorAll('#spec-options [name]')]
        .map((node) => node.name),
}));
check('the designer is built from the server vocabulary',
      specVocabulary.columns.includes('account-name') &&
      specVocabulary.columns.includes('debit') &&
      ['none', 'account-tree', 'period']
          .every((name) => specVocabulary.groupings.includes(name)) &&
      ['start', 'end', 'date_preset', 'currency']
          .every((name) => specVocabulary.options.includes(name)),
      `columns=${specVocabulary.columns.join(',')} ` +
      `groupings=${specVocabulary.groupings.join(',')} ` +
      `options=${specVocabulary.options.join(',')}`);

await page.fill('#spec-name', 'Designer Trial Balance');
await page.fill('#spec-title', 'Designer Trial Balance');
await page.check('#spec-columns input[value="debit"]');
await page.check('#spec-columns input[value="credit"]');
await page.selectOption('#spec-grouping', 'none');
await page.fill('#spec-opt-start', '');
await page.fill('#spec-opt-end', '2018-12-31');

const explicitUrl = new URL('report/Trial%20Balance?end=2018-12-31', url).href;
// Compare table *text*, cell by cell: the DOM normalises attribute quoting
// when it serialises the preview, and the point of the gate is the numbers,
// not the markup's quote style.
const explicitRows = await page.evaluate(async (target) => {
    const text = await (await fetch(target)).text();
    const doc = new DOMParser().parseFromString(text, 'text/html');
    return [...doc.querySelectorAll('table tr')].map((row) =>
        [...row.cells].map((cell) =>
            cell.textContent.replace(/\s+/g, ' ').trim()).join(' | '));
}, explicitUrl);
const readTableRows = () => page.evaluate(() => {
    const doc = document.getElementById('view').contentDocument;
    return [...doc.querySelectorAll('table tr')].map((row) =>
        [...row.cells].map((cell) =>
            cell.textContent.replace(/\s+/g, ' ').trim()).join(' | '));
});

await page.click('#spec-preview');
await page.waitForFunction(() => {
    const frame = document.getElementById('view');
    const doc = frame && frame.contentDocument;
    // The preview replaces the frame with srcdoc; the old report may already
    // contain the same figures, so wait for the preview's own title.
    return doc && doc.title === 'Designer Trial Balance' &&
        doc.body && doc.body.innerText.includes('10,000.00');
}, null, { timeout: 15000 });
const previewRows = await readTableRows();
check('the designer preview matches the explicit request',
      JSON.stringify(previewRows) === JSON.stringify(explicitRows),
      `${previewRows.length} rows vs ${explicitRows.length}`);

await page.click('#spec-save');
await page.waitForFunction(() => {
    const link = [...document.querySelectorAll('#report-list a.saved-report-link')]
        .find((node) => node.textContent === 'Designer Trial Balance');
    return Boolean(link);
}, null, { timeout: 15000 });
const savedSpec = await page.evaluate(async (base) => {
    const response = await fetch(new URL('api/specs', base));
    const data = await response.json();
    return data.specs.find((spec) => spec.name === 'Designer Trial Balance');
}, url);
check('the spec is saved server-side and listed',
      Boolean(savedSpec) && typeof savedSpec.id === 'string' &&
      savedSpec.grouping === 'none',
      savedSpec ? savedSpec.id : 'not in /api/specs');

await page.waitForFunction(() => {
    const frame = document.getElementById('view');
    const doc = frame && frame.contentDocument;
    return frame && /\/report\/spec\//.test(frame.src) && doc &&
        doc.body && doc.body.innerText.includes('10,000.00');
}, null, { timeout: 15000 });
const savedRows = await readTableRows();
check('the saved report renders the same table as the explicit request',
      JSON.stringify(savedRows) === JSON.stringify(explicitRows),
      `${savedRows.length} rows vs ${explicitRows.length}`);

await page.reload({ waitUntil: 'domcontentloaded' });
await page.waitForFunction(
    () => document.getElementById('summary')?.textContent.includes('accounts'),
    null, { timeout: 15000 });
await page.waitForFunction(() =>
    [...document.querySelectorAll('#report-list a.saved-report-link')]
        .some((node) => node.textContent === 'Designer Trial Balance'),
    null, { timeout: 15000 });
check('the saved report survives a page reload', true,
      'it lives on the server, not in localStorage');

await page.locator('#report-list a.saved-report-link',
                  { hasText: 'Designer Trial Balance' }).first().click();
await page.waitForFunction(() => {
    const frame = document.getElementById('view');
    const doc = frame && frame.contentDocument;
    return frame && /\/report\/spec\//.test(frame.src) && doc &&
        doc.body && doc.body.innerText.includes('10,000.00');
}, null, { timeout: 15000 });
const reloadedRows = await readTableRows();
check('a saved report opened after a reload still matches',
      JSON.stringify(reloadedRows) === JSON.stringify(explicitRows),
      `${reloadedRows.length} rows vs ${explicitRows.length}`);

await page.locator('#report-list li.saved-report',
                  { hasText: 'Designer Trial Balance' })
    .locator('button.delete-spec').click();
await page.waitForFunction(() =>
    ![...document.querySelectorAll('#report-list a.saved-report-link')]
        .some((node) => node.textContent === 'Designer Trial Balance'),
    null, { timeout: 15000 });
const specsAfterDelete = await (await fetch(new URL('api/specs', url))).json();
check('deleting a saved report removes it from the server',
      specsAfterDelete.specs.length === 0,
      `${specsAfterDelete.specs.length} left`);

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
