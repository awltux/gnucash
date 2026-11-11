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

// The designer's controls as the form would send them.  This is the browser
// half of "the field values equal the spec": every field, not just the name.
const readDesignerFields = () => page.evaluate(() => ({
    source: document.getElementById('spec-source').value,
    name: document.getElementById('spec-name').value,
    title: document.getElementById('spec-title').value,
    tags: document.getElementById('spec-tags').value
        .split(/[\s,]+/).filter(Boolean),
    columns: [...document.querySelectorAll('#spec-columns input:checked')]
        .map((box) => box.value),
    split_columns: [...document.querySelectorAll(
        '#spec-split-columns input:checked')].map((box) => box.value),
    split_grouping: document.getElementById('spec-split-grouping').value,
    invoice_owner: document.getElementById('spec-invoice-owner').value,
    invoice_grouping: document.getElementById('spec-invoice-grouping').value,
    bucket_basis: document.getElementById('spec-bucket-basis').value,
    lot_account: document.getElementById('spec-lot-account').value,
    description_filter: document.getElementById('spec-description-filter').value,
    document: document.getElementById('spec-document').value,
    invoice: document.getElementById('spec-invoice').value,
    grouping: document.getElementById('spec-grouping').value,
    period_unit: document.getElementById('spec-period-unit').value,
    period_mode: document.getElementById('spec-period-mode').value,
    accounts: [...document.querySelectorAll('#spec-accounts input:checked')]
        .map((box) => box.value),
    account_types: [...document.querySelectorAll(
        '#spec-account-types input:checked')].map((box) => box.value),
    chart: document.getElementById('spec-chart').value,
    chart_series: [...document.querySelectorAll(
        '#spec-chart-series input:checked')].map((box) => box.value),
    start: document.getElementById('spec-opt-start').value,
    end: document.getElementById('spec-opt-end').value,
    date_preset: document.getElementById('spec-opt-date_preset').value,
    currency: document.getElementById('spec-opt-currency').value,
}));

function expectedDesignerFields(spec) {
    return {
        source: spec.source || 'accounts',
        name: spec.name,
        title: spec.title || spec.name,
        tags: Array.isArray(spec.tags) ? spec.tags : [],
        columns: spec.source === 'splits' ? [] : (Array.isArray(spec.columns)
            ? spec.columns : []),
        split_columns: spec.source === 'splits' && Array.isArray(spec.columns)
            ? spec.columns : [],
        split_grouping: spec.split_grouping || 'none',
        invoice_owner: spec.invoice_owner || 'receivable',
        invoice_grouping: spec.invoice_grouping || 'owner',
        bucket_basis: spec.bucket_basis || 'due',
        lot_account: spec.source === 'lots' && Array.isArray(spec.accounts) &&
            spec.accounts.length === 1 ? spec.accounts[0] : '',
        description_filter: spec.description_filter || '',
        document: spec.document || '',
        invoice: spec.invoice || '',
        grouping: spec.grouping || 'none',
        period_unit: spec.period_unit || 'year',
        period_mode: spec.period_mode || 'activity',
        accounts: Array.isArray(spec.accounts) ? spec.accounts : [],
        account_types: Array.isArray(spec.account_types)
            ? spec.account_types : [],
        chart: spec.chart || '',
        chart_series: Array.isArray(spec.chart_series) ? spec.chart_series : [],
        start: spec.date_preset ? '' : (spec.start || ''),
        end: spec.date_preset ? '' : (spec.end || ''),
        date_preset: spec.date_preset || '',
        currency: spec.currency || '',
    };
}

// Compare the populated form with a served spec.  Accounts are a set (their
// order does not reach the renderer), everything else must match exactly:
// column and chart-series order changes the document.
function designerEquals(expected, actual) {
    const want = expectedDesignerFields(expected);
    const got = { ...actual };
    const setEqual = (left, right) =>
        JSON.stringify([...left].sort()) === JSON.stringify([...right].sort());
    const accountsMatch = setEqual(got.accounts, want.accounts);
    delete got.accounts;
    delete want.accounts;
    return accountsMatch && JSON.stringify(got) === JSON.stringify(want);
}

const tableRowsOfHtml = (html) => page.evaluate((text) => {
    const doc = new DOMParser().parseFromString(text, 'text/html');
    return [...doc.querySelectorAll('table tr')].map((row) =>
        [...row.cells].map((cell) =>
            cell.textContent.replace(/\s+/g, ' ').trim()).join(' | '));
}, html);

// Wait for the report frame to finish loading a report.  Switching reports
// immediately after a click aborts the previous request; that is a browser
// fact, not a server error, so the tests let each frame settle first.
const waitForReportFrame = (name) => page.waitForFunction((reportName) => {
    const frame = document.getElementById('view');
    if (!frame || frame.hidden) return false;
    let source = '';
    try {
        source = decodeURIComponent(frame.src || '');
    } catch (err) {
        return false;
    }
    const doc = frame.contentDocument;
    return source.includes(`/report/${reportName}`) && doc && doc.body &&
        (!doc.contentType || doc.contentType === 'text/html');
}, name, { timeout: 15000 });

const waitForSpecFrame = () => page.waitForFunction(() => {
    const frame = document.getElementById('view');
    const doc = frame && frame.contentDocument;
    return frame && !frame.hidden && /\/report\/spec\//.test(frame.src) &&
        doc && doc.body && (!doc.contentType || doc.contentType === 'text/html');
}, null, { timeout: 15000 });

const servedSpec = (id) => page.evaluate(async (specId) => {
    const response = await fetch(
        new URL('api/specs/' + encodeURIComponent(specId), location.href));
    return response.ok ? response.json() : null;
}, id);

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
// ones; the no-Guile build lists the native fourteen plus the built-in specs.
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

/* ------------------------------------------------- report list filtering */

// The list carries tags from the server, the search box narrows it as you
// type, and a tag chip applies that tag.  These are the visible-list half of
// the API's ?tag= and ?q= filters that report-spec.sh checks.
const visibleEntries = () => page.evaluate(() =>
    [...document.querySelectorAll('#report-list li')]
        .filter((li) => !li.hidden)
        .map((li) => li.textContent.replace(/\s+/g, ' ').trim()));
const allEntries = await visibleEntries();
const tagChips = await page.locator('#report-list button.tag-chip').count();
check('the report entries carry tag chips', tagChips > 0, `${tagChips} chips`);

await page.fill('#report-search', 'net worth');
const narrowed = await visibleEntries();
check('typing in the search box narrows the list',
      narrowed.length > 0 && narrowed.length < allEntries.length &&
      narrowed.every((text) => /net worth/i.test(text)),
      `${allEntries.length} -> ${narrowed.length}: ${narrowed.join(' | ')}`);

await page.fill('#report-search', '');
const restored = await visibleEntries();
check('clearing the search restores the list',
      restored.length === allEntries.length,
      `${restored.length} of ${allEntries.length}`);

await page.locator('#report-list button.tag-chip[data-tag="breakdown"]')
    .first().click();
const taggedEntries = await visibleEntries();
check('clicking a tag chip filters to that tag',
      taggedEntries.length === 2 &&
      taggedEntries.every((text) =>
          /Expenses by Account|Income by Account/.test(text)),
      taggedEntries.join(' | '));
await page.locator(
    '#report-list button.tag-chip.selected[data-tag="breakdown"]')
    .first().click();
const clearedEntries = await visibleEntries();
check('clicking the selected chip clears the tag filter',
      clearedEntries.length === allEntries.length,
      `${clearedEntries.length} of ${allEntries.length}`);

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

/* ------------------------------------- built-in definition in the designer */

// Selecting a built-in must load its definition into the designer: every
// field, compared with the document the server serves for that id.
const incomeBuiltin = specList.builtin.find(
    (spec) => spec.name === 'Income vs Expenses by Month');
await page.waitForFunction(() => {
    const name = document.getElementById('spec-name');
    return name && name.value === 'Income vs Expenses by Month' &&
        !document.getElementById('designer-banner').hidden;
}, null, { timeout: 15000 });
const servedIncome = await servedSpec(incomeBuiltin.id);
const builtinFields = await readDesignerFields();
check('selecting a built-in populates every designer field from its spec',
      Boolean(servedIncome) && designerEquals(servedIncome, builtinFields),
      JSON.stringify({ got: builtinFields, want: servedIncome }));

const definitionBanner = await page.textContent('#designer-provenance');
check('the built-in designer says it is the definition',
      /definition/i.test(definitionBanner) &&
      /built-in report/.test(definitionBanner) &&
      /Save as/.test(definitionBanner),
      definitionBanner);
check('the built-in designer offers Save as…, not Save',
      (await page.textContent('#spec-save')).trim() === 'Save as…');

// Saving under the built-in's own name must be refused in the UI, not left
// to the server's duplicate-name error.
await page.click('#spec-save');
await page.waitForSelector('#spec-error:not([hidden])', { timeout: 15000 });
const sameNameError = (await page.textContent('#spec-error')).trim();
check('Save as… refuses to reuse the built-in’s name',
      /needs a new name for the copy/.test(sameNameError), sameNameError);

// Fork it under a new name.  The title stays the built-in's, so the saved
// copy must render exactly the document the built-in renders.
await page.fill('#spec-name', 'Income vs Expenses (copy)');
await page.click('#spec-save');
await page.waitForFunction(() =>
    [...document.querySelectorAll('#report-list a.saved-report-link')]
        .some((node) => node.textContent === 'Income vs Expenses (copy)'),
    null, { timeout: 15000 });
await waitForSpecFrame();
const copySpec = await page.evaluate(async (base) => {
    const response = await fetch(new URL('api/specs', base));
    const data = await response.json();
    return data.specs.find((spec) => spec.name === 'Income vs Expenses (copy)');
}, url);
check('the fork is a new saved report with its own id',
      Boolean(copySpec) && copySpec.id !== incomeBuiltin.id,
      copySpec ? copySpec.id : 'not saved');

const builtinHtml = await (await fetch(
    new URL('report/spec/' + encodeURIComponent(incomeBuiltin.id), url))).text();
const copyHtml = await (await fetch(
    new URL('report/spec/' + encodeURIComponent(copySpec.id), url))).text();
const builtinRows = await tableRowsOfHtml(builtinHtml);
const copyRows = await tableRowsOfHtml(copyHtml);
check('the fork’s preview is identical to the built-in’s render',
      builtinHtml === copyHtml ||
      JSON.stringify(builtinRows) === JSON.stringify(copyRows),
      builtinHtml === copyHtml ? 'byte for byte'
          : JSON.stringify(builtinRows) === JSON.stringify(copyRows)
              ? 'cell for cell' : 'the documents differ');

// The copy is a saved report, so it can be deleted again; the built-in must
// still be there and untouched.
await page.locator('#report-list li.saved-report',
                  { hasText: 'Income vs Expenses (copy)' })
    .locator('button.delete-spec').click();
await page.waitForFunction(() =>
    ![...document.querySelectorAll('#report-list a.saved-report-link')]
        .some((node) => node.textContent === 'Income vs Expenses (copy)'),
    null, { timeout: 15000 });
const afterFork = await (await fetch(new URL('api/specs', url))).json();
check('the built-in survives deleting the fork',
      afterFork.specs.length === 0 &&
      afterFork.builtin.some((spec) => spec.id === incomeBuiltin.id),
      `${afterFork.specs.length} saved, ${afterFork.builtin.length} built-in`);

// Switching from one built-in to another repopulates: different spec,
// different fields, no leftovers.
await page.locator('#report-list a.builtin-report-link',
                  { hasText: 'Expenses by Account' }).first().click();
await page.waitForFunction(() => {
    const name = document.getElementById('spec-name');
    return name && name.value === 'Expenses by Account';
}, null, { timeout: 15000 });
const expensesBuiltin = specList.builtin.find(
    (spec) => spec.name === 'Expenses by Account');
const switchedFields = await readDesignerFields();
check('switching built-ins repopulates the designer rather than leaving stale values',
      designerEquals(await servedSpec(expensesBuiltin.id), switchedFields) &&
      switchedFields.chart === 'bar' &&
      switchedFields.chart_series.join(',') === 'debit' &&
      JSON.stringify(switchedFields.tags) ===
          JSON.stringify(['spending', 'breakdown']),
      JSON.stringify(switchedFields));
await waitForSpecFrame();

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
                doc.defaultView &&
                (doc.defaultView.__reportCharts || []).length > 0)
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
// A <tr> appears while the document is still being parsed, so reading the
// body now can return a fraction of it. Wait for the frame's document to
// finish, then for two consecutive reads to agree before asserting a length.
await page.waitForFunction(() => {
    const view = document.getElementById('view');
    return view && view.contentDocument &&
           view.contentDocument.readyState === 'complete';
}, null, { timeout: 15000 });
const frameText = async () => (await frame.locator('body').innerText()).replace(/\s+/g, ' ');
let reportText = await frameText();
for (let attempt = 0; attempt < 100; attempt++) {
    await page.waitForTimeout(20);
    const again = await frameText();
    if (again === reportText) break;
    reportText = again;
}
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

// The last report opened was a native one (Transaction Report), whose
// designer has no honest equivalent and is therefore hidden.  Start a blank
// report before building the explicit Trial Balance spec below.
await page.click('#spec-new');
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

const tagChoices = await page.evaluate(() =>
    [...document.querySelectorAll('#spec-tag-suggestions option')]
        .map((option) => option.value));
check('the designer offers the existing tags as suggestions',
      tagChoices.includes('net-worth') && tagChoices.includes('breakdown'),
      tagChoices.join(','));

await page.fill('#spec-name', 'Designer Trial Balance');
await page.fill('#spec-title', 'Designer Trial Balance');
await page.fill('#spec-tags', 'spending, monthly');
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
const readTableRows = () => page.evaluate(async () => {
    const rowsOf = () => {
        const doc = document.getElementById('view').contentDocument;
        return doc ? [...doc.querySelectorAll('table tr')].map((row) =>
            [...row.cells].map((cell) =>
                cell.textContent.replace(/\s+/g, ' ').trim()).join(' | ')) : [];
    };
    // The frame's document is parsed asynchronously, so the wait conditions
    // above can be satisfied by the title and the first rows while the rest of
    // the table is still being parsed; reading then compares a partial table
    // (and the numbers, not the document, look wrong).  Wait for the document
    // to finish parsing and for two reads to agree before returning.
    const pause = () => new Promise((resolve) => setTimeout(resolve, 20));
    let previous = rowsOf();
    for (let attempt = 0; attempt < 250; ++attempt) {
        const doc = document.getElementById('view').contentDocument;
        const current = rowsOf();
        if (doc && doc.readyState === 'complete' &&
            current.length === previous.length &&
            JSON.stringify(current) === JSON.stringify(previous))
            return current;
        previous = current;
        await pause();
    }
    return previous;
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
check('the saved spec keeps its tags on the server',
      Boolean(savedSpec) &&
      JSON.stringify(savedSpec.tags) === JSON.stringify(['spending', 'monthly']),
      savedSpec ? JSON.stringify(savedSpec.tags) : 'not in /api/specs');

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
const savedChips = await page.evaluate(() => {
    const link = [...document.querySelectorAll(
        '#report-list a.saved-report-link')]
        .find((node) => node.textContent === 'Designer Trial Balance');
    const row = link && link.closest('li');
    return row
        ? [...row.querySelectorAll('button.tag-chip')]
            .map((chip) => chip.dataset.tag)
        : [];
});
check('saving a spec with tags then reloading the page shows them',
      JSON.stringify(savedChips) === JSON.stringify(['spending', 'monthly']),
      savedChips.join(','));

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

/* --------------------------------------- native reports in the designer */

// A native report is C++: the implementation panel must name the file and
// function, and the designer may only offer a starting point it labels.  The
// Trial Balance starting point claims to be equivalent, so its preview is
// compared with the native report cell for cell; the Income Statement's is
// approximate and must say how; the Transaction Report has no equivalent and
// must leave the designer empty.
await page.locator('#report-list a', { hasText: /^Trial Balance$/ })
    .first().click();
await page.waitForFunction(() => {
    const name = document.getElementById('spec-name');
    return name && name.value === 'Trial Balance (copy)' &&
        !document.getElementById('designer-banner').hidden;
}, null, { timeout: 15000 });
await waitForReportFrame('Trial Balance');
const tbBanner = (await page.textContent('#designer-provenance')).replace(/\s+/g, ' ');
check('the Trial Balance starting point is labelled, not called a definition',
      /This report is implemented in C\+\+; this is an equivalent starting point, not its definition/.test(tbBanner),
      tbBanner);
const tbSource = (await page.textContent('#native-source-text')).replace(/\s+/g, ' ');
check('the native implementation panel names the C++ file and function',
      /gnc-report\.cpp/.test(tbSource) && /trial_balance\(\)/.test(tbSource),
      tbSource);

const tbFields = await readDesignerFields();
check('the Trial Balance starting point is a flat debit/credit account list',
      tbFields.grouping === 'none' &&
      tbFields.columns.join(',') === 'account-name,debit,credit' &&
      tbFields.account_types.length === 0 &&
      tbFields.end !== '',
      JSON.stringify({ columns: tbFields.columns, grouping: tbFields.grouping }));

// Post the form's fields as a spec and compare the result with the native
// report's own URL.  This is the proof of "equivalent": the label alone
// would not be enough.
const tbPayload = {
    name: tbFields.name,
    title: tbFields.title,
    columns: tbFields.columns,
    grouping: tbFields.grouping,
    period_unit: tbFields.period_unit,
    period_mode: tbFields.period_mode,
    accounts: tbFields.accounts,
    account_types: tbFields.account_types,
    chart: tbFields.chart,
    chart_series: tbFields.chart_series,
};
if (tbFields.date_preset !== '') tbPayload.date_preset = tbFields.date_preset;
else {
    if (tbFields.start !== '') tbPayload.start = tbFields.start;
    if (tbFields.end !== '') tbPayload.end = tbFields.end;
}
if (tbFields.currency !== '') tbPayload.currency = tbFields.currency;
const tbPreview = await page.evaluate(async (spec) => {
    const response = await fetch(new URL('api/spec/preview', location.href), {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(spec),
    });
    return { status: response.status, text: await response.text() };
}, tbPayload);
const tbNativeHtml = await (await fetch(
    new URL('report/Trial%20Balance', url))).text();
const tbPreviewRows = await tableRowsOfHtml(tbPreview.text);
const tbNativeRows = await tableRowsOfHtml(tbNativeHtml);
check('the Trial Balance starting point renders the native report’s cells',
      tbPreview.status === 200 && tbPreviewRows.length > 3 &&
      JSON.stringify(tbPreviewRows) === JSON.stringify(tbNativeRows),
      `HTTP ${tbPreview.status}, ${tbPreviewRows.length} vs ${tbNativeRows.length} rows`);

await page.locator('#report-list a', { hasText: /^Income Statement$/ })
    .first().click();
await page.waitForFunction(() => {
    const name = document.getElementById('spec-name');
    return name && name.value === 'Income Statement (starting point)' &&
        !document.getElementById('designer-banner').hidden;
}, null, { timeout: 15000 });
await waitForReportFrame('Income Statement');
const isBanner = (await page.textContent('#designer-provenance')).replace(/\s+/g, ' ');
check('the Income Statement starting point says it is approximate and how',
      /this is an approximate starting point, not its definition/.test(isBanner) &&
      /sign-reversed Balance column/.test(isBanner),
      isBanner);
const isFields = await readDesignerFields();
check('the Income Statement starting point is income and expense activity',
      isFields.grouping === 'none' &&
      isFields.account_types.join(',') === 'INCOME,EXPENSE' &&
      isFields.columns.join(',') === 'account-name,debit,credit',
      JSON.stringify(isFields.account_types));

await page.locator('#report-list a', { hasText: /^Transaction Report$/ })
    .first().click();
await page.waitForFunction(() => {
    const banner = document.getElementById('designer-provenance');
    return banner && /no designer equivalent/.test(banner.textContent) &&
        !document.getElementById('designer-banner').hidden;
}, null, { timeout: 15000 });
await waitForReportFrame('Transaction Report');
const noneBanner = (await page.textContent('#designer-provenance')).replace(/\s+/g, ' ');
const noneSource = (await page.textContent('#native-source-text')).replace(/\s+/g, ' ');
check('a report with no equivalent explains why and names its implementation',
      /lists transactions and splits/.test(noneBanner) &&
      /transaction_report\(\)/.test(noneSource),
      noneBanner);
check('a report with no equivalent leaves no designer fields invented',
      await page.evaluate(() =>
          document.getElementById('spec-form').hidden &&
          document.getElementById('spec-name').value === ''),
      'the form is hidden and empty');

// Switching back to a spec-backed report fills the fields again.
await page.locator('#report-list a.builtin-report-link',
                  { hasText: 'Income by Account' }).first().click();
await page.waitForFunction(() => {
    const name = document.getElementById('spec-name');
    return name && name.value === 'Income by Account';
}, null, { timeout: 15000 });
await waitForSpecFrame();
const backFields = await readDesignerFields();
const incomeByAccount = specList.builtin.find(
    (spec) => spec.name === 'Income by Account');
check('switching from a native report back to a built-in repopulates',
      designerEquals(await servedSpec(incomeByAccount.id), backFields),
      JSON.stringify(backFields));

// New report is the way out of a hidden designer: blank fields and Save.
await page.click('#spec-new');
const newFields = await readDesignerFields();
check('New report clears the designer and returns to Save',
      newFields.name === '' && newFields.grouping === 'none' &&
      newFields.columns.join(',') === 'account-name' &&
      newFields.account_types.length === 0 &&
      newFields.chart === '' &&
      (await page.textContent('#spec-save')).trim() === 'Save report' &&
      !(await page.evaluate(() => document.getElementById('spec-form').hidden)),
      JSON.stringify({ name: newFields.name, columns: newFields.columns }));

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

/* --------------------------------- source-aware designer: every source */

// The gate for the source-aware form: each source /api/spec/options publishes
// must be selectable, must show only its own fields, must preview a document
// (not a 400), must save, and must keep its tags and its source across a page
// reload.  The list comes from the server, so a new source cannot be added
// without this suite noticing.

const optionsVocabulary =
    await (await fetch(new URL('api/spec/options', url))).json();
const declaredSources = optionsVocabulary.sources.map((entry) => entry.name);
const offeredSources = await page.evaluate(() =>
    [...document.querySelectorAll('#spec-source option')]
        .map((option) =>
            ({ name: option.value, label: option.textContent.trim() })));
check('the designer lists every source /api/spec/options publishes',
      JSON.stringify(offeredSources.map((entry) => entry.name)) ===
          JSON.stringify(declaredSources) &&
      declaredSources.length === 9 &&
      offeredSources.every((entry) => entry.label !== ''),
      offeredSources.map((entry) => `${entry.name}=${entry.label}`).join(' | '));

// A preview replaces the frame with srcdoc.  Removing the old srcdoc starts a
// navigation, so the frame's *document* is replaced; remembering the old
// document (the WindowProxy itself is stable across navigations) is how the
// wait below knows it is looking at the preview and not at the previous
// source's still-complete document.  The predicate and the read of the text
// happen in one page task for the same reason.
async function previewSource(name) {
    await page.evaluate(() => {
        const frame = document.getElementById('view');
        window.__previewDocument = frame.contentDocument;
        frame.removeAttribute('srcdoc');
    });
    await page.click('#spec-preview');
    const preview = await page.waitForFunction(() => {
        const frame = document.getElementById('view');
        const doc = frame && frame.contentDocument;
        if (!frame || frame.hidden || !frame.hasAttribute('srcdoc') || !doc ||
            doc === window.__previewDocument ||
            doc.readyState !== 'complete' || !doc.body) return false;
        const text = doc.body.innerText.replace(/\s+/g, ' ').trim();
        if (text.length === 0) return false;
        const error = document.getElementById('spec-error');
        return {
            rejected: !error.hidden,
            error: error.textContent.trim(),
            text,
        };
    }, null, { timeout: 15000 });
    return preview.jsonValue();
}

// A render through /report/spec/<id> is done when its body has parsed and the
// frame is visible.  Reloading or deleting before that aborts the request,
// which the request log would honestly report as a failed request.
async function waitForSpecFrameComplete() {
    await page.waitForFunction(() => {
        const frame = document.getElementById('view');
        const doc = frame && frame.contentDocument;
        return frame && !frame.hidden && /\/report\/spec\//.test(frame.src) &&
            doc && doc.readyState === 'complete' && doc.body &&
            doc.body.innerText.trim().length > 0;
    }, null, { timeout: 15000 });
}

const savedSpecNamed = (name) => page.evaluate(async (wanted) => {
    const response = await fetch(new URL('api/specs', location.href));
    const data = await response.json();
    return data.specs.find((spec) => spec.name === wanted) || null;
}, name);

// The vocabulary, not just the report list: selectSpec() and the page's own
// loadDesignerOptions() would otherwise race to build the form.
const waitForSourcePicker = () => page.waitForFunction(
    () => document.querySelectorAll('#spec-source option').length >= 9,
    null, { timeout: 15000 });

// A populated designer: the named spec is in the form, its source is in the
// picker, and (when asked) its provenance banner is showing.
const waitForDesigner = (name, source, banner = true) =>
    page.waitForFunction(({ wanted, wantedSource, wantedBanner }) => {
        const form = document.getElementById('spec-form');
        if (!form || form.hidden) return false;
        if (document.getElementById('spec-name').value !== wanted ||
            document.getElementById('spec-source').value !== wantedSource)
            return false;
        return !wantedBanner ||
            !document.getElementById('designer-banner').hidden;
    }, { wanted: name, wantedSource: source, wantedBanner: banner },
    { timeout: 15000 });

async function saveSource(name, source) {
    await page.click('#spec-save');
    await page.waitForFunction((wanted) =>
        [...document.querySelectorAll('#report-list a.saved-report-link')]
            .some((link) => link.textContent === wanted), name,
        { timeout: 15000 });
    // Save selects the new report; wait for its population so the frame and
    // the form are settled before the reload.
    await waitForDesigner(name, source, false);
    await waitForSpecFrameComplete();
    return savedSpecNamed(name);
}

async function reopenAfterReload(name, source) {
    await page.reload({ waitUntil: 'domcontentloaded' });
    await page.waitForFunction(
        () => document.getElementById('summary')
            ?.textContent.includes('accounts'), null, { timeout: 15000 });
    await waitForSourcePicker();
    await page.waitForFunction((wanted) =>
        [...document.querySelectorAll('#report-list a.saved-report-link')]
            .some((link) => link.textContent === wanted), name,
        { timeout: 15000 });
    await page.locator('#report-list a.saved-report-link', { hasText: name })
        .first().click();
    await waitForDesigner(name, source);
    await waitForSpecFrameComplete();
    return page.evaluate(() => ({
        name: document.getElementById('spec-name').value,
        source: document.getElementById('spec-source').value,
        tags: document.getElementById('spec-tags').value
            .split(/[\s,]+/).filter(Boolean),
    }));
}

async function deleteSaved(name) {
    await page.locator('#report-list li.saved-report', { hasText: name })
        .locator('button.delete-spec').click();
    await page.waitForFunction((wanted) =>
        ![...document.querySelectorAll('#report-list a.saved-report-link')]
            .some((link) => link.textContent === wanted), name,
        { timeout: 15000 });
}

// Switching the picker must clear the fields the new source refuses.  Fill
// account-only fields, switch to the invoice source (which accepts none of
// them), and require them gone from the DOM and hidden — then preview the new
// source to prove the stale values cannot cause a 400.
await page.click('#spec-new');
await page.selectOption('#spec-source', 'accounts');
await page.fill('#spec-opt-currency', 'EUR');
await page.fill('#spec-opt-start', '2018-01-01');
await page.selectOption('#spec-grouping', 'period');
await page.selectOption('#spec-source', 'invoices');
const sourceSwitch = await page.evaluate(() => ({
    source: document.getElementById('spec-source').value,
    currency: document.getElementById('spec-opt-currency').value,
    currencyHidden: document.getElementById('spec-opt-row-currency').hidden,
    start: document.getElementById('spec-opt-start').value,
    startHidden: document.getElementById('spec-opt-row-start').hidden,
    datePresetHidden:
        document.getElementById('spec-opt-row-date_preset').hidden,
    grouping: document.getElementById('spec-grouping').value,
    groupingHidden: document.getElementById('spec-grouping-row').hidden,
    columnsHidden: document.getElementById('spec-columns-row').hidden,
}));
check('switching source clears and hides the fields the new source refuses',
      sourceSwitch.source === 'invoices' && sourceSwitch.currency === '' &&
      sourceSwitch.currencyHidden && sourceSwitch.start === '' &&
      sourceSwitch.startHidden && sourceSwitch.datePresetHidden &&
      sourceSwitch.grouping === 'none' && sourceSwitch.groupingHidden &&
      sourceSwitch.columnsHidden,
      JSON.stringify(sourceSwitch));
await page.fill('#spec-name', 'Gate switched invoices');
await page.fill('#spec-opt-end', '2018-12-31');
await page.selectOption('#spec-invoice-owner', 'payable');
const switchedPreview = await previewSource('Gate switched invoices');
check('a source switched over a stale value previews without a rejection',
      !switchedPreview.rejected && switchedPreview.text.length > 20 &&
      switchedPreview.text.includes('Gate switched invoices'),
      switchedPreview.rejected ? switchedPreview.error
          : `${switchedPreview.text.length} characters`);

const gateSources = [
    {
        source: 'accounts', name: 'Gate accounts',
        fill: async () => {
            await page.check('#spec-columns input[value="debit"]');
            await page.check('#spec-columns input[value="credit"]');
            await page.fill('#spec-opt-start', '2018-01-01');
            await page.fill('#spec-opt-end', '2018-12-31');
        },
        shows: { columns: true, grouping: true, accounts: true },
    },
    {
        source: 'splits', name: 'Gate splits',
        fill: async () => {
            await page.fill('#spec-opt-start', '2018-01-01');
            await page.fill('#spec-opt-end', '2018-12-31');
        },
        shows: { splitColumns: true, accounts: true },
    },
    {
        source: 'balance-sheet', name: 'Gate balance sheet',
        fill: async () => {
            await page.fill('#spec-opt-end', '2018-12-31');
        },
        shows: {},
    },
    {
        source: 'invoices', name: 'Gate invoices',
        fill: async () => {
            await page.selectOption('#spec-invoice-owner', 'payable');
            await page.fill('#spec-opt-end', '2018-12-31');
        },
        shows: { invoice: true },
    },
    {
        source: 'lots', name: 'Gate lots',
        fill: async () => {
            await page.fill('#spec-opt-start', '2018-01-01');
            await page.fill('#spec-opt-end', '2018-12-31');
        },
        shows: { lots: true },
    },
    {
        source: 'budgets', name: 'Gate budgets',
        fill: async () => {},
        shows: { accounts: true },
    },
    {
        source: 'tax-codes', name: 'Gate tax codes',
        fill: async () => {
            await page.fill('#spec-opt-start', '2018-01-01');
            await page.fill('#spec-opt-end', '2018-12-31');
        },
        shows: { accounts: true },
    },
    {
        source: 'cash-flow', name: 'Gate cash flow',
        fill: async () => {
            await page.fill('#spec-opt-start', '2018-01-01');
            await page.fill('#spec-opt-end', '2018-12-31');
        },
        shows: { accounts: true },
    },
    {
        source: 'document', name: 'Gate document',
        fill: async () => {
            await page.selectOption('#spec-document', 'Printable Invoice');
            await page.fill('#spec-invoice', 'yoyo1');
        },
        marker: 'Bill #yoyo1',
        shows: { document: true },
    },
];

for (const gate of gateSources) {
    const { source, name } = gate;
    const tags = ['gate', source];
    await page.click('#spec-new');
    await page.selectOption('#spec-source', source);
    await page.fill('#spec-name', name);
    await page.fill('#spec-title', name);
    await page.fill('#spec-tags', tags.join(', '));

    // A cold form is the source with its own defaults and nothing else.  Every
    // source must preview from there; a fixed-column source used to fail
    // because the account column group had nothing ticked.
    const cold = await previewSource(name);
    check(`${source}: previews from a cold form with only the source's defaults`,
          !cold.rejected && cold.text.length > 20,
          cold.rejected ? cold.error : `${cold.text.length} characters`);

    await gate.fill();

    const shown = await page.evaluate(() => ({
        columns: !document.getElementById('spec-columns-row').hidden,
        splitColumns:
            !document.getElementById('spec-split-columns-row').hidden,
        grouping: !document.getElementById('spec-grouping-row').hidden,
        accounts: !document.getElementById('spec-accounts-row').hidden,
        invoice: !document.getElementById('spec-invoice-owner-row').hidden,
        lots: !document.getElementById('spec-lot-account-row').hidden,
        document: !document.getElementById('spec-document-row').hidden,
    }));
    const expectedShown = {
        columns: false, splitColumns: false, grouping: false,
        accounts: false, invoice: false, lots: false, document: false,
        ...gate.shows,
    };
    check(`${source}: the form shows only the fields the source accepts`,
          JSON.stringify(shown) === JSON.stringify(expectedShown),
          JSON.stringify(shown));

    const preview = await previewSource(name);
    check(`${source}: preview renders a document, not a rejection`,
          !preview.rejected && preview.text.length > 20 &&
          preview.text.includes(gate.marker || name),
          preview.rejected ? preview.error
              : `${preview.text.length} characters`);

    const saved = await saveSource(name, source);
    check(`${source}: save keeps the source and the tags`,
          Boolean(saved) && saved.source === source &&
          JSON.stringify(saved.tags) === JSON.stringify(tags),
          saved ? `source=${saved.source} tags=${JSON.stringify(saved.tags)}`
              : 'not in /api/specs');

    const reopened = await reopenAfterReload(name, source);
    check(`${source}: reopening after a reload keeps the source and the tags`,
          reopened.source === source &&
          JSON.stringify(reopened.tags) === JSON.stringify(tags),
          JSON.stringify(reopened));

    await deleteSaved(name);
}

/* ---------------------- provenance: exact, approximate and no spec */

// The banner is the difference between a definition and a reconstruction.
// An exact report says its starting spec renders the report's own cells; an
// approximate one says how it differs; a report the vocabulary cannot express
// says it is C++ and names the source it would need.
await page.locator('#report-list a', { hasText: /^Trial Balance$/ })
    .first().click();
await waitForDesigner('Trial Balance (copy)', 'accounts');
await waitForReportFrame('Trial Balance');
const exactBanner =
    (await page.textContent('#designer-provenance')).replace(/\s+/g, ' ');
check('an exact report says exact and that the starting spec is its cells',
      /Exact equivalence: the starting spec renders the report's own cells/
          .test(exactBanner) &&
      /this is an equivalent starting point, not its definition/
          .test(exactBanner),
      exactBanner);
check('the exact starting point keeps the accounts source in the picker',
      await page.inputValue('#spec-source') === 'accounts',
      await page.inputValue('#spec-source'));

await page.locator('#report-list a', { hasText: /^Income Statement$/ })
    .first().click();
await waitForDesigner('Income Statement (starting point)', 'accounts');
await waitForReportFrame('Income Statement');
const approximateBanner =
    (await page.textContent('#designer-provenance')).replace(/\s+/g, ' ');
check('an approximate report says approximate and how it differs',
      /Approximate equivalence:/.test(approximateBanner) &&
      /this is an approximate starting point, not its definition/
          .test(approximateBanner) &&
      /sign-reversed Balance column/.test(approximateBanner) &&
      /groups the rows under Revenues and Expenses/.test(approximateBanner),
      approximateBanner);

// Transaction Breakdown is the report whose table has a date's account
// columns: a spec's fixed column list cannot describe it.  The notice must say
// so in plain words and name the source it would need.
await page.locator('#report-list a', { hasText: /^Transaction Breakdown Report$/ })
    .first().click();
await page.waitForFunction(() => {
    const banner = document.getElementById('designer-provenance');
    return banner && /No spec/.test(banner.textContent) &&
        !document.getElementById('designer-banner').hidden;
}, null, { timeout: 15000 });
await waitForReportFrame('Transaction Breakdown Report');
const noSpecBanner =
    (await page.textContent('#designer-provenance')).replace(/\s+/g, ' ');
check('a report with no spec says it is C++ and names the source it would need',
      /No spec/.test(noSpecBanner) &&
      /implemented in C\+\+/.test(noSpecBanner) &&
      /no designer equivalent/.test(noSpecBanner) &&
      /`transaction-breakdown` source would be needed/.test(noSpecBanner),
      noSpecBanner);
check('a report with no spec invents no starting point',
      await page.evaluate(() =>
          document.getElementById('spec-form').hidden &&
          document.getElementById('spec-name').value === ''),
      'the form is hidden and empty');

// A starting point carries the source it was published with, so a fork keeps
// it: the Balance Sheet's source is balance-sheet and its fork is served with
// the same source.
await page.locator('#report-list a', { hasText: /^Balance Sheet$/ })
    .first().click();
await waitForDesigner('Balance Sheet (copy)', 'balance-sheet');
await waitForReportFrame('Balance Sheet');
check('a balance-sheet starting point carries its source in the picker',
      await page.inputValue('#spec-source') === 'balance-sheet',
      await page.inputValue('#spec-source'));
await page.fill('#spec-name', 'Gate forked balance sheet');
await page.click('#spec-save');
await page.waitForFunction(() =>
    [...document.querySelectorAll('#report-list a.saved-report-link')]
        .some((link) => link.textContent === 'Gate forked balance sheet'),
    null, { timeout: 15000 });
await waitForSpecFrameComplete();
const fork = await savedSpecNamed('Gate forked balance sheet');
check('a fork of a starting point keeps its source',
      Boolean(fork) && fork.source === 'balance-sheet' &&
      fork.columns.join(',') === 'account-name,balance',
      fork ? `source=${fork.source} columns=${fork.columns.join(',')}`
          : 'not saved');
await deleteSaved('Gate forked balance sheet');

// The document starting point carries its layout too.
await page.locator('#report-list a', { hasText: /^Printable Invoice$/ })
    .first().click();
await waitForDesigner('Printable Invoice (copy)', 'document');
await waitForReportFrame('Printable Invoice');
check('a document starting point carries its source and its layout',
      await page.inputValue('#spec-source') === 'document' &&
      await page.inputValue('#spec-document') === 'Printable Invoice',
      `${await page.inputValue('#spec-source')} / ` +
      `${await page.inputValue('#spec-document')}`);

// ---------------------------------------------------------------- the menu
// The menu bar is the desktop application's, rendered from one table: same
// twelve top-level menus in the same order, the desktop's labels and
// accelerator hints, page-scoped items present but disabled with the page
// they belong to, and every item that cannot work disabled with a reason.
// Checks below: the shape, opening/closing, keyboard navigation, two working
// items, a disabled item that reports why and does not act, a submenu, About,
// and a menu action that really writes to the server.

const menuRoot = (id) => `#menubar .menu[data-menu="${id}"]`;
const menuPopup = (id) => `${menuRoot(id)} > .menu-popup`;

async function openMenu(id) {
    await page.click(`${menuRoot(id)} > .menu-title`);
    await page.waitForSelector(`${menuPopup(id)}:not([hidden])`);
}

async function menuItemLabels(id) {
    return page.evaluate((selector) =>
        [...document.querySelectorAll(`${selector} > .menu-item .menu-label,
            ${selector} > .menu-sub > .menu-item .menu-label`)]
            .map((node) => node.textContent.trim()), menuPopup(id));
}

// What the browser actually shows.  A submenu counts only when it is both not
// hidden and laid out (an "open" submenu inside a closed popup has no layout),
// and the label comes from its trigger.  The menu checks below use this rather
// than the page's own state, so a flag that claims a submenu is open while it
// is not visible cannot satisfy them.
async function visibleMenuContainers() {
    return page.evaluate(() => {
        const menus = [...document.querySelectorAll('#menubar [role="menu"]')]
            .filter((popup) => !popup.hidden && popup.getClientRects().length > 0);
        return menus.map((popup) => {
            const submenu = popup.classList.contains('menu-submenu');
            const owner = popup.parentElement.querySelector(
                submenu ? ':scope > .menu-item' : ':scope > .menu-title');
            return {
                top: !submenu,
                label: (owner.querySelector('.menu-label') || owner)
                    .textContent.trim(),
            };
        });
    });
}

// The invariant every menu step must leave behind: at most one top-level menu
// visible, at most one submenu visible, and aria-expanded on every submenu
// trigger agreeing with whether that submenu has layout.
async function checkMenuInvariant(step) {
    const containers = await visibleMenuContainers();
    const subs = containers.filter((menu) => !menu.top);
    const tops = containers.filter((menu) => menu.top);
    const ariaMatches = await page.evaluate(() =>
        [...document.querySelectorAll('#menubar .menu-sub > .menu-item')]
            .every((trigger) => {
                const sub = trigger.parentElement.querySelector(
                    ':scope > .menu-popup');
                const shown = !sub.hidden && sub.getClientRects().length > 0;
                return (trigger.getAttribute('aria-expanded') === 'true') === shown;
            }));
    check(`at most one menu path is visible after ${step}`,
          tops.length <= 1 && subs.length <= 1 && ariaMatches,
          `${tops.length} top-level menu(s), ${subs.length} submenu(s), ` +
          `aria matches: ${ariaMatches}` +
          (subs.length ? ` [${subs.map((sub) => sub.label).join(', ')}]` : ''));
    return { containers, tops, subs };
}

const topLevelLabels = await page.evaluate(() =>
    [...document.querySelectorAll('#menubar > .menu > .menu-title')]
        .map((button) => button.textContent.trim()));
check('the menu bar has the desktop application\'s twelve menus in order',
      JSON.stringify(topLevelLabels) === JSON.stringify(
          ['File', 'Edit', 'View', 'Transaction', 'Actions', 'Business',
           'Scheduled', 'Reports', 'Tools', 'Extensions', 'Windows', 'Help']),
      topLevelLabels.join(', '));

const menuRoles = await page.evaluate(() => {
    const bar = document.getElementById('menubar');
    const titles = [...document.querySelectorAll('#menubar > .menu > .menu-title')];
    return {
        bar: bar.getAttribute('role'),
        titlesAreItems: titles.every((button) =>
            button.getAttribute('role') === 'menuitem' &&
            button.getAttribute('aria-haspopup') === 'true' &&
            button.getAttribute('aria-expanded') === 'false'),
        popupsAreMenus: [...document.querySelectorAll('#menubar .menu-popup')]
            .every((popup) => popup.getAttribute('role') === 'menu' && popup.hidden),
        oneTabStop: titles.filter((button) => button.tabIndex === 0).length,
        total: titles.length,
    };
});
check('the menu bar is a menubar of closed menus with one tab stop',
      menuRoles.bar === 'menubar' && menuRoles.titlesAreItems &&
      menuRoles.popupsAreMenus && menuRoles.oneTabStop === 1 &&
      menuRoles.total === 12,
      `role=${menuRoles.bar} menus=${menuRoles.total} ` +
      `tabstops=${menuRoles.oneTabStop}`);

await openMenu('file');
const fileLabels = await menuItemLabels('file');
check('opening File shows the desktop\'s items and its two submenus',
      ['New File', 'Open…', 'Save', 'Save As…', 'Revert', 'Print',
       'Page Setup', 'Properties', 'Close', 'Quit', 'Import', 'Export']
          .every((label) => fileLabels.includes(label)),
      fileLabels.join(' | '));

await page.keyboard.press('Escape');
const afterEscape = await page.evaluate(() => ({
    hidden: document.querySelector('#menubar .menu[data-menu="file"] > .menu-popup').hidden,
    expanded: document.getElementById('menubar-file').getAttribute('aria-expanded'),
    focus: document.activeElement.id,
}));
check('Escape closes the open menu and returns focus to its title',
      afterEscape.hidden && afterEscape.expanded === 'false' &&
      afterEscape.focus === 'menubar-file',
      `hidden=${afterEscape.hidden} focus=${afterEscape.focus}`);

// The keyboard model: ArrowDown opens the menu on its first item, arrows move
// between items, ArrowRight/Left move between menus.
await page.focus('#menubar-file');
await page.keyboard.press('ArrowDown');
const firstItemFocus = await page.evaluate(() => document.activeElement.dataset.item);
await page.keyboard.press('ArrowDown');
const secondItemFocus = await page.evaluate(() => document.activeElement.dataset.item);
await page.keyboard.press('ArrowUp');
const backToFirst = await page.evaluate(() => document.activeElement.dataset.item);
check('ArrowDown opens a menu and the arrows move between its items',
      firstItemFocus === 'new-file' && secondItemFocus === 'open' &&
      backToFirst === 'new-file',
      `${firstItemFocus} -> ${secondItemFocus} -> ${backToFirst}`);

await page.keyboard.press('ArrowRight');
const movedMenu = await page.evaluate(() => ({
    menu: document.activeElement.closest('.menu')?.dataset.menu,
    expanded: document.getElementById('menubar-edit').getAttribute('aria-expanded'),
}));
await page.keyboard.press('ArrowLeft');
const movedBack = await page.evaluate(() =>
    document.activeElement.closest('.menu')?.dataset.menu);
check('ArrowRight and ArrowLeft move between top-level menus',
      movedMenu.menu === 'edit' && movedMenu.expanded === 'true' &&
      movedBack === 'file',
      `${movedMenu.menu} -> ${movedBack}`);
await page.keyboard.press('Escape');

// A submenu opens with ArrowRight into its first item and closes with Escape
// back onto its trigger, leaving the parent menu open.
await openMenu('actions');
await page.keyboard.press('ArrowRight');
const submenuState = await page.evaluate(() => {
    const trigger = document.querySelector(
        '#menubar .menu[data-menu="actions"] .menu-sub > .menu-item');
    return {
        open: trigger.getAttribute('aria-expanded'),
        focus: document.activeElement.textContent.trim(),
    };
});
await page.keyboard.press('Escape');
const submenuClosed = await page.evaluate(() => {
    const trigger = document.querySelector(
        '#menubar .menu[data-menu="actions"] .menu-sub > .menu-item');
    return {
        closed: trigger.getAttribute('aria-expanded'),
        focus: document.activeElement.textContent.trim(),
        parentOpen: !document.querySelector(
            '#menubar .menu[data-menu="actions"] > .menu-popup').hidden,
    };
});
check('a submenu opens into its first item and Escape returns to its trigger',
      submenuState.open === 'true' && /Get Balance/.test(submenuState.focus) &&
      submenuClosed.closed === 'false' &&
      /Online Actions/.test(submenuClosed.focus) && submenuClosed.parentOpen,
      `${submenuState.focus} -> ${submenuClosed.focus}`);

// The desktop's Stock Split item is a register item.  It must keep its name,
// report the page it belongs to, and do nothing when activated.
const stockSplit = await page.evaluate(() => {
    const button = document.querySelector(
        '#menubar .menu[data-menu="actions"] [data-item="stock-split"]');
    return {
        label: button.querySelector('.menu-label').textContent.trim(),
        disabled: button.getAttribute('aria-disabled'),
        description: button.getAttribute('aria-description') || '',
        title: button.title,
        focusable: button.tabIndex === -1,
    };
});
// A disabled menuitem is not "enabled" to Playwright either, so dispatch the
// activation directly: the page's own handler must ignore it.
await page.locator('#menubar .menu[data-menu="actions"] [data-item="stock-split"]')
    .dispatchEvent('click');
const afterDisabledClick = await page.evaluate(() => ({
    menuOpen: !document.querySelector(
        '#menubar .menu[data-menu="actions"] > .menu-popup').hidden,
    dialogOpen: document.getElementById('about-dialog').open,
    designerHidden: document.getElementById('designer').hidden,
}));
check('a desktop item the web cannot do keeps its label and states the reason',
      stockSplit.label === 'Stock Split…' && stockSplit.disabled === 'true' &&
      /writes to the book/.test(stockSplit.description) &&
      stockSplit.title === stockSplit.description && stockSplit.focusable,
      `${stockSplit.label}: ${stockSplit.description}`);
check('activating a disabled item performs no action',
      afterDisabledClick.menuOpen && !afterDisabledClick.dialogOpen &&
      !afterDisabledClick.designerHidden,
      'the menu stayed open and nothing changed');
await page.keyboard.press('Escape');

// One menu path at a time.  Opening a submenu used to leave a sibling one
// open, and clicking another top-level title while a menu was open only
// closed the menu the pointer had already opened.  Every step below also
// asserts the global invariant from what the DOM shows: one top-level menu,
// at most one submenu, and aria-expanded that agrees with the layout.
await openMenu('file');
const importTrigger = page.locator(`${menuRoot('file')} .menu-sub > .menu-item`,
                                    { hasText: 'Import' }).first();
await importTrigger.click();
const importStep = await checkMenuInvariant('opening File > Import');
check('File > Import opens one submenu',
      importStep.subs.length === 1 && importStep.subs[0].label === 'Import',
      importStep.subs.map((sub) => sub.label).join(', '));

await page.click(`${menuRoot('edit')} > .menu-title`);
const switchedStep = await checkMenuInvariant(
    'clicking Edit while File > Import is open');
const switchedState = await page.evaluate(() => ({
    openTitles: [...document.querySelectorAll('#menubar > .menu > .menu-title')]
        .filter((title) => title.getAttribute('aria-expanded') === 'true')
        .map((title) => title.textContent.trim()),
    fileSubmenus: [...document.querySelectorAll(
        '#menubar .menu[data-menu="file"] .menu-submenu')]
        .filter((sub) => !sub.hidden && sub.getClientRects().length > 0).length,
    fileExpanded: document.getElementById('menubar-file')
        .getAttribute('aria-expanded'),
    editExpanded: document.getElementById('menubar-edit')
        .getAttribute('aria-expanded'),
}));
check('clicking another top-level menu closes the first menu and its submenu',
      switchedStep.subs.length === 0 &&
      switchedState.openTitles.length === 1 &&
      switchedState.openTitles[0] === 'Edit' &&
      switchedState.fileSubmenus === 0 &&
      switchedState.fileExpanded === 'false' &&
      switchedState.editExpanded === 'true',
      `open: ${switchedState.openTitles.join(', ') || 'none'}; ` +
      `File submenus: ${switchedState.fileSubmenus}`);
await page.keyboard.press('Escape');
await checkMenuInvariant('Escape closing the Edit menu');

await openMenu('actions');
const onlineTrigger = page.locator(
    `${menuRoot('actions')} .menu-sub > .menu-item`,
    { hasText: 'Online Actions' }).first();
await onlineTrigger.click();
const onlineStep = await checkMenuInvariant(
    'opening Actions > Online Actions');
check('Actions > Online Actions opens its submenu',
      onlineStep.subs.length === 1 &&
      onlineStep.subs[0].label === 'Online Actions',
      onlineStep.subs.map((sub) => sub.label).join(', '));

const budgetTrigger = page.locator(
    `${menuRoot('actions')} .menu-sub > .menu-item`,
    { hasText: 'Budget' }).first();
await budgetTrigger.click();
const siblingStep = await checkMenuInvariant(
    'opening the sibling Actions > Budget');
const labelsOf = (step) => step.subs.map((sub) => sub.label).join(', ') || 'none';
check('opening a sibling submenu closes the one that was open',
      siblingStep.subs.length === 1 && siblingStep.subs[0].label === 'Budget',
      `${labelsOf(onlineStep)} -> ${labelsOf(siblingStep)}`);

await budgetTrigger.click();
const closedStep = await checkMenuInvariant(
    'clicking the open Budget parent');
const closedState = await page.evaluate(() => ({
    focus: document.activeElement.textContent.trim(),
    expanded: [...document.querySelectorAll(
        '#menubar .menu-sub > .menu-item[aria-expanded="true"]')].length,
    actionsOpen: !document.querySelector(
        '#menubar .menu[data-menu="actions"] > .menu-popup').hidden,
}));
check('clicking the parent of an open submenu closes it and focuses the parent',
      closedStep.subs.length === 0 && closedState.expanded === 0 &&
      /Budget/.test(closedState.focus) && closedState.actionsOpen,
      `visible submenus: ${closedStep.subs.length}; focus: ${closedState.focus}`);

// Move the pointer away and back: a submenu closed by clicking its parent
// must not reopen on the next hover.
await page.mouse.move(10, 700);
await budgetTrigger.hover();
const hoverStep = await checkMenuInvariant(
    'hovering the closed Budget parent');
check('a submenu closed by clicking its parent does not reopen on hover',
      hoverStep.subs.length === 0, `${hoverStep.subs.length} submenu(s)`);

await budgetTrigger.click();
const reopenedStep = await checkMenuInvariant('clicking Budget again');
check('the parent of a closed submenu reopens it on the next click',
      reopenedStep.subs.length === 1 &&
      reopenedStep.subs[0].label === 'Budget',
      reopenedStep.subs.map((sub) => sub.label).join(', '));

await page.keyboard.press('Escape');
const escapeStep = await checkMenuInvariant('Escape from an open submenu');
const escapeState = await page.evaluate(() => ({
    focus: document.activeElement.textContent.trim(),
    expanded: [...document.querySelectorAll(
        '#menubar .menu-sub > .menu-item[aria-expanded="true"]')].length,
    actionsOpen: !document.querySelector(
        '#menubar .menu[data-menu="actions"] > .menu-popup').hidden,
}));
check('Escape closes every open submenu and returns focus to the trigger',
      escapeStep.subs.length === 0 && escapeState.expanded === 0 &&
      /Budget/.test(escapeState.focus) && escapeState.actionsOpen,
      `visible submenus: ${escapeStep.subs.length}; focus: ${escapeState.focus}`);

await page.keyboard.press('Escape');
const barClosed = await checkMenuInvariant('Escape closing the menu bar');
const barClosedState = await page.evaluate(() =>
    [...document.querySelectorAll('#menubar .menu-popup')]
        .every((popup) => popup.hidden) &&
    [...document.querySelectorAll('#menubar .menu-title')]
        .every((title) => title.getAttribute('aria-expanded') === 'false'));
check('a second Escape closes the top-level menu with no stale expansion',
      barClosed.tops.length === 0 && barClosed.subs.length === 0 &&
      barClosedState,
      `${barClosed.tops.length} menus left open`);

// A whole page-scoped menu: the desktop hides Transaction without a register
// page; the web keeps it visible and says why, with every item disabled.
await openMenu('transaction');
const transactionMenu = await page.evaluate(() => {
    const popup = document.querySelector(
        '#menubar .menu[data-menu="transaction"] > .menu-popup');
    const items = [...popup.querySelectorAll('[data-item]')];
    return {
        note: popup.querySelector('.menu-note')?.textContent || '',
        items: items.length,
        allDisabled: items.every((item) =>
            item.getAttribute('aria-disabled') === 'true'),
    };
});
check('a page-scoped menu shows its items disabled and names its page',
      transactionMenu.items > 10 && transactionMenu.allDisabled &&
      /Register page only/.test(transactionMenu.note),
      `${transactionMenu.items} items: ${transactionMenu.note}`);
await page.keyboard.press('Escape');

// Working items: Find focuses the report search, Summary Bar toggles the
// summary, Report Designer toggles the designer.
await openMenu('edit');
await page.click('#menubar .menu[data-menu="edit"] [data-item="find"]');
check('Edit > Find… focuses the report search box',
      await page.evaluate(() => document.activeElement.id) === 'report-search',
      await page.evaluate(() => document.activeElement.id));

await openMenu('view');
await page.click('#menubar .menu[data-menu="view"] [data-item="toggle-summary"]');
const summaryHidden = await page.evaluate(() =>
    document.getElementById('summary').hidden);
await openMenu('view');
const summaryChecked = await page.evaluate(() => document.querySelector(
    '#menubar .menu[data-menu="view"] [data-item="toggle-summary"]')
    .getAttribute('aria-checked'));
await page.click('#menubar .menu[data-menu="view"] [data-item="toggle-summary"]');
const summaryShown = await page.evaluate(() =>
    !document.getElementById('summary').hidden);
check('View > Summary Bar hides and shows the summary, and reports its state',
      summaryHidden && summaryChecked === 'false' && summaryShown,
      `hidden=${summaryHidden} checked=${summaryChecked} shown=${summaryShown}`);

await openMenu('view');
await page.click('#menubar .menu[data-menu="view"] [data-item="toggle-designer"]');
const designerHidden = await page.evaluate(() =>
    document.getElementById('designer').hidden);
await openMenu('view');
await page.click('#menubar .menu[data-menu="view"] [data-item="toggle-designer"]');
const designerShown = await page.evaluate(() =>
    !document.getElementById('designer').hidden);
check('View > Report Designer hides and shows the designer',
      designerHidden && designerShown, `hidden=${designerHidden} shown=${designerShown}`);

// Reports > Report List goes to the list the desktop generates its Reports
// menu from, and focuses it.
await openMenu('reports');
await page.click('#menubar .menu[data-menu="reports"] [data-item="report-list"]');
check('Reports > Report List focuses the report list',
      await page.evaluate(() =>
          Boolean(document.activeElement.closest('#report-list'))),
      await page.evaluate(() => document.activeElement.textContent.trim()));

// Help > About must not invent a version: it shows the report store version
// the server publishes and says the session is read-only.
await openMenu('help');
await page.click('#menubar .menu[data-menu="help"] [data-item="about"]');
const about = await page.evaluate(() => ({
    open: document.getElementById('about-dialog').open,
    version: document.getElementById('about-version').textContent,
    session: document.getElementById('about-session').textContent,
    hint: document.getElementById('about-hint').textContent,
}));
await page.keyboard.press('Escape');
const aboutClosed = await page.evaluate(() =>
    !document.getElementById('about-dialog').open);
check('Help > About opens a dialog that says which version it can show',
      about.open &&
      (/Report store version: [0-9]+/.test(about.version) ||
       /not published by this server/.test(about.version)) &&
      /read-only/i.test(about.session) && /build ID/.test(about.hint) &&
      aboutClosed,
      `${about.version.trim()}; closed=${aboutClosed}`);

// A menu action that really writes: Save Report Configuration As… submits the
// designer's form, the report is saved server-side and appears in the list.
const menuReportName = 'Menu gate report';
await page.fill('#spec-name', menuReportName);
await openMenu('reports');
await page.click('#menubar .menu[data-menu="reports"] [data-item="save-config-as"]');
await page.waitForFunction((wanted) =>
    [...document.querySelectorAll('#report-list a.saved-report-link')]
        .some((link) => link.textContent === wanted), menuReportName,
    { timeout: 15000 });
const menuSaved = await savedSpecNamed(menuReportName);
check('Reports > Save Report Configuration As… saves the designer report',
      Boolean(menuSaved) && menuSaved.name === menuReportName,
      menuSaved ? `saved ${menuSaved.name}` : 'not saved');
await deleteSaved(menuReportName);

// Nothing is silently inert: every disabled item in the rendered bar carries
// a reason in its title and its ARIA description.
const menuStats = await page.evaluate(() => {
    const items = [...document.querySelectorAll('#menubar [data-item]')];
    const disabled = items.filter((item) =>
        item.getAttribute('aria-disabled') === 'true');
    return {
        total: items.length,
        available: items.length - disabled.length,
        disabled: disabled.length,
        withoutReason: disabled.filter((item) =>
            !(item.getAttribute('aria-description') || '').trim() ||
            !(item.title || '').trim()).map((item) => item.textContent.trim()),
    };
});
check('every disabled menu item carries a reason',
      menuStats.withoutReason.length === 0 && menuStats.disabled > 40 &&
      menuStats.available > 0,
      `${menuStats.total} items: ${menuStats.available} available, ` +
      `${menuStats.disabled} disabled`);
if (menuStats.withoutReason.length)
    console.log(`items without a reason: ${menuStats.withoutReason.join(', ')}`);

// Extensibility: a page module adds its own item to an existing menu, before
// a named base item, without the base table changing.  This is the hook a
// register or business page will use.
const pageModuleWorked = await page.evaluate(() => {
    MENU_PAGES.test_page = {
        present: () => true,
        menus: {
            view: {
                before: 'refresh',
                items: [{ label: 'Page Added Item', run: () => {} }],
            },
        },
    };
    renderMenus();
    const labels = [...document.querySelectorAll(
        '#menubar .menu[data-menu="view"] .menu-label')]
        .map((node) => node.textContent);
    const addedAt = labels.indexOf('Page Added Item');
    const refreshAt = labels.indexOf('Refresh');
    const correctPlace = addedAt >= 0 && refreshAt > addedAt;
    delete MENU_PAGES.test_page;
    renderMenus();
    const gone = ![...document.querySelectorAll('#menubar .menu-label')]
        .some((node) => node.textContent === 'Page Added Item');
    return { correctPlace, gone, labels: labels.slice(0, 4) };
});
check('a page module adds an item without editing the base table',
      pageModuleWorked.correctPlace && pageModuleWorked.gone,
      `inserted before Refresh: ${pageModuleWorked.correctPlace}; ` +
      `removed again: ${pageModuleWorked.gone}`);

if (screenshotPath) {
    await page.screenshot({ path: screenshotPath });
    console.log(`screenshot: ${screenshotPath}`);
}

for (const p of problems) console.log(`page problem: ${p}`);
check('no console errors or failed requests', problems.length === 0, `${problems.length} problems`);

await browser.close();
process.exit(failures === 0 ? 0 : 1);
