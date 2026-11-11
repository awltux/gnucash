// The book a browser phase is allowed to run against, checked before any
// check.  A phase states what it needs with an exported FIXTURE object and
// calls requireFixture() before it launches a browser:
//
//   export const FIXTURE = { book: "demo", writable: false };
//   await requireFixture("register-ui.mjs", FIXTURE, url);
//
// Three times a phase was pointed at the wrong book, and every failure looked
// like a regression in the page.  A mismatch now refuses in one line, naming
// what was expected and what the server answered, and exits before a single
// check is printed.
//
// The account count is the fixture's own fact, never a guess: sample1 is 104
// accounts, and the demo book records its count in `<book>.expect.json` (the
// generator writes it).  web/run-ui-test.sh reads that file and passes the
// number in GNC_UI_DEMO_ACCOUNTS; when node can see the server's filesystem
// the file is read directly, so a phase run by hand is checked the same way.
//
// No dependency: node's fetch and fs are enough.

import { readFileSync } from "node:fs";

export const SAMPLE1_ACCOUNTS = 104;

function refuse(message) {
    console.error(`REFUSED: ${message}`);
    process.exit(3);
}

// WSL forwards the Linux server's loopback to Windows, and a Windows client
// can be refused for a moment after the server binds.  That is a harness
// fact, not a fixture mismatch, so only a connection refusal is retried; any
// other failure refuses at once.  (The browser suites wait the same way before
// their first navigation.)
const refused = (err) =>
    /ECONNREFUSED|CONNECTION_REFUSED|ERR_CONNECTION_REFUSED|fetch failed/i
        .test(String(err)) ||
    /ECONNREFUSED/.test(String(err && err.cause));

async function api(url, route) {
    for (let attempt = 0; ; attempt++) {
        try {
            const response = await fetch(new URL(route, url));
            if (!response.ok)
                throw new Error(`${route} answered ${response.status}`);
            return await response.json();
        } catch (err) {
            if (attempt >= 50 || !refused(err)) throw err;
            await new Promise((resolve) => setTimeout(resolve, 200));
        }
    }
}

// The demo generator's account count.  The environment is preferred because
// the runner has already read the expect file next to the book it generated;
// the file itself is the fallback for a phase run on the same machine.
function demoAccounts(session) {
    const fromRunner = process.env.GNC_UI_DEMO_ACCOUNTS;
    if (fromRunner !== undefined && fromRunner !== "") {
        const count = Number(fromRunner);
        if (Number.isInteger(count) && count > 0) return count;
        refuse(`GNC_UI_DEMO_ACCOUNTS is not an account count: ${fromRunner}`);
    }
    if (session.book) {
        try {
            const expect = JSON.parse(
                readFileSync(`${session.book}.expect.json`, "utf8"));
            if (Number.isInteger(expect.accounts) && expect.accounts > 0)
                return expect.accounts;
        } catch (err) {
            // Reported below with the rest of the refusal.
        }
    }
    refuse("cannot verify the demo fixture: GNC_UI_DEMO_ACCOUNTS is unset " +
           "and its <book>.expect.json could not be read; run the phase " +
           "through web/run-ui-test.sh");
}

// On a mismatch this does not return: it exits 3 so the caller cannot go on
// to run a check.
export async function requireFixture(phase, fixture, url) {
    let session;
    let book;
    try {
        [session, book] = await Promise.all([
            api(url, "api/session"),
            api(url, "api/book"),
        ]);
    } catch (err) {
        refuse(`${phase} needs the ${fixture.book} fixture, but the server ` +
               `did not answer /api/session and /api/book: ${err.message}`);
    }

    let expected;
    if (fixture.book === "sample1")
        expected = SAMPLE1_ACCOUNTS;
    else if (fixture.book === "demo")
        expected = demoAccounts(session);
    else
        refuse(`${phase} declares an unknown fixture book: ${fixture.book}`);

    if (book.accounts !== expected)
        refuse(`${phase} needs the ${fixture.book} fixture (${expected} ` +
               `accounts), but the server has ${book.accounts} accounts`);

    if (fixture.writable && session.writable !== true)
        refuse(`${phase} needs the ${fixture.book} fixture on a writable ` +
               `server, but /api/session answered writable:${session.writable}` +
               (session.edit_reason ? ` (${session.edit_reason})` : ""));
}
