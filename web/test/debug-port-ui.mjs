// Headless browser test for --debug-port (ADR 0012).
//
//   node web/test/debug-port-ui.mjs <url>
//
// <url> is a gnc-webd started with --debug-port on a fixed loopback port, so
// the page is served at "/" with no token segment.  The FIXTURE below
// declares sample1.gnucash (104 accounts); a server with another book is
// refused before a check runs.  web/run-ui-test.sh runs this as its fourth
// phase.  It checks:
//
//   * the page loads at "/" and /api/book + /api/accounts answer without a
//     token (the summary shows the book);
//   * the debug banner is visible and says the URL has no token, that any
//     local process can drive the UI, and what --writable would add;
//   * a report renders at the natural /report/<name> path;
//   * no console errors or unexpected failed requests.
//
// Needs playwright-core on NODE_PATH, exactly like ui-test.mjs.

import { createRequire } from 'node:module';
import { requireFixture } from './fixture.mjs';

const { chromium } = createRequire(import.meta.url)('playwright-core');

export const FIXTURE = { book: 'sample1', writable: false };

const url = process.argv[2];
if (!url) {
    console.error('usage: node debug-port-ui.mjs <url>');
    process.exit(2);
}

// Refuse a server that is not the declared fixture before a single check.
await requireFixture('debug-port-ui.mjs', FIXTURE, url);

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

let failures = 0;
function check(name, ok, detail = '') {
    console.log(`${ok ? 'ok  ' : 'FAIL'} ${name}${detail ? ` — ${detail}` : ''}`);
    if (!ok) failures++;
}

// WSL forwards 127.0.0.1 from Windows; a just-bound server can still refuse
// for a moment.  Retry only that condition, so a real page failure still fails.
for (let attempt = 0; ; attempt++) {
    try {
        await page.goto(url);
        break;
    } catch (err) {
        if (attempt >= 9 ||
            !/ECONNREFUSED|CONNECTION_REFUSED|ERR_CONNECTION_REFUSED/.test(
                String(err))) throw err;
        await page.waitForTimeout(300);
    }
}
await page.waitForFunction(
    () => (document.getElementById('summary')?.textContent || '').includes('accounts'),
    null, { timeout: 30000 });

const state = await page.evaluate(() => {
    const banner = document.getElementById('debug-port-banner');
    const summary = document.getElementById('summary');
    return {
        path: location.pathname,
        summary: summary ? summary.textContent : '',
        banner: banner
            ? {
                visible: banner.offsetParent !== null && !banner.hidden,
                text: banner.textContent,
            }
            : null,
    };
});

check('the page is served at "/" with no token segment',
      state.path === '/', `path=${state.path}`);
check('the page loads the book without a token',
      /\d+ accounts/.test(state.summary), `summary=${JSON.stringify(state.summary)}`);
check('the debug banner is visible', Boolean(state.banner?.visible),
      state.banner ? 'banner present' : 'no banner element');
const bannerText = state.banner?.text || '';
check('the banner says the URL has no token',
      bannerText.includes('no token in the URL'), bannerText);
check('the banner names the local-process risk',
      bannerText.includes('Any local process can drive this UI'), bannerText);
check('the banner names the --writable risk',
      bannerText.includes('--writable') &&
      bannerText.includes('edit and save the book'), bannerText);

// The banner is the first body child and the menu follows it.  Its height is
// whatever its text needs, so the property to assert is geometric: the banner
// is fully inside the viewport and it ends at or before the menu starts, at
// both the normal width and the width where the menu's margins change.  A
// screenshot would not say either of those; the boxes do.
async function bannerGeometry(width) {
    await page.setViewportSize({ width, height: 900 });
    await page.evaluate(() => new Promise((resolve) =>
        requestAnimationFrame(() => requestAnimationFrame(resolve))));
    return page.evaluate(() => {
        const banner = document.getElementById('debug-port-banner');
        const menu = document.getElementById('menubar');
        // A missing box is a failed check, not an exception with no check.
        if (!banner || !menu)
            return { banner: null, menu: null, viewport:
                { width: innerWidth, height: innerHeight }, inlineStyle: null };
        const b = banner.getBoundingClientRect();
        const m = menu.getBoundingClientRect();
        return {
            banner: { top: b.top, bottom: b.bottom, left: b.left, right: b.right },
            menu: { top: m.top, bottom: m.bottom, left: m.left, right: m.right },
            viewport: { width: innerWidth, height: innerHeight },
            inlineStyle: banner.getAttribute('style'),
        };
    });
}

for (const width of [1280, 800]) {
    const layout = await bannerGeometry(width);
    const boxes = layout.banner !== null && layout.menu !== null;
    check(`the debug banner and the menu do not overlap at ${width}px`,
          boxes && layout.banner.bottom <= layout.menu.top,
          boxes ? `banner ${layout.banner.top}-${layout.banner.bottom}, ` +
                  `menu ${layout.menu.top}-${layout.menu.bottom}`
                : 'banner or menu missing');
    check(`the debug banner is fully visible at ${width}px`,
          boxes && layout.banner.top >= 0 && layout.banner.left >= 0 &&
          layout.banner.bottom <= layout.viewport.height &&
          layout.banner.right <= layout.viewport.width,
          boxes ? JSON.stringify(layout.banner) : 'banner missing');
    if (width === 1280)
        check('the banner carries no inline layout (the stylesheet owns it)',
              layout.inlineStyle === null, String(layout.inlineStyle));
}
await page.setViewportSize({ width: 1280, height: 900 });

const book = await page.evaluate(async () => {
    const response = await fetch('/api/book');
    return { status: response.status, body: await response.json() };
});
check('/api/book answers 200 at the natural path',
      book.status === 200 && typeof book.body.accounts === 'number',
      `status=${book.status}, accounts=${book.body?.accounts}`);

const report = await page.evaluate(async () => {
    const response = await fetch('/report/Trial%20Balance');
    return { status: response.status, text: await response.text() };
});
check('a report renders at the natural /report/<name> path',
      report.status === 200 && /<table/i.test(report.text),
      `status=${report.status}, ${report.text.length} bytes`);

for (const problem of problems) console.log(`page problem: ${problem}`);
check('no console errors or unexpected failed requests', problems.length === 0,
      `${problems.length} problems`);

if (process.env.UI_SCREENSHOT)
    await page.screenshot({ path: process.env.UI_SCREENSHOT });

await browser.close();
process.exit(failures === 0 ? 0 : 1);
