# The eguile decision — invoices and tax reports without Guile

Status: proposal, awaiting a decision
Branch: `webui/engine-server`
Related: `doc/report-engine-migration.md` (stage 6), `doc/report-coverage.md`

## What is at stake

Nine of the 66 Scheme reports are **documents**, not tables:

| report | kind | what it needs |
|---|---|---|
| Easy Invoice, Fancy Invoice, Printable Invoice | customer invoice layouts | invoice + owner + entry model, HTML template |
| Receipt | customer receipt | same, shorter |
| Tax Invoice, Australian Tax Invoice | locale invoice layouts | as above + locale tax fields |
| Balance Sheet (eguile) | statement | **redundant** — the native Balance Sheet already covers it |
| US Tax Schedule / TXF export, German tax report | tax schedules and file export | locale tax tables + TXF/XML writer |

For a personal-finance web UI these are the difference between "my reports" and
"my business documents". For a business user they are a day-one blocker: an
invoice is a thing you send to a customer, not a view you look at.

## Why they are not just another report

Everything else ported so far reads the engine and prints a table. These need
three things the native layer does not have:

1. **an invoice/owner model in the web tier** — `gncInvoice`, `gncOwner`,
   `gncEntry`, with tax tables and payment terms;
2. **a templating layer** — the Scheme reports are *eguile*: HTML files with
   embedded Scheme expressions (`<?scm ... ?>`), which is how users customise
   an invoice without writing code;
3. **locale data** — tax table definitions and the TXF/XML export formats,
   which differ per country and are themselves registered from Scheme.

## The options

### A. Port eguile (a template language in the server)

Implement a small template engine of our own and convert the five eguile
templates. Closest to "the same reports", preserves the idea of editable
templates, but means designing a template language, its sandbox and its error
reporting — and it is still *our* language, not the one users' templates are
written in, so existing customised templates break anyway.

**Cost:** weeks to months. **Risk:** high (a new interpreter in the request path).

### B. Bespoke renderers for the five documents

Write the invoice, receipt and tax-invoice layouts natively, as C++/HTML, with a
shared invoice model and tax tables. No template language. Users cannot change
the layout, but they get a correct document.

**Cost:** weeks, once the invoice model exists (which the tax reports need
anyway). **Risk:** low — it is the same shape as every other native report, and
the parity gate works identically.

### C. Hybrid: invoice model now, templates later

Build the invoice/owner model and the **tax reports** (which need no templates),
ship the five documents as bespoke renderers, and leave a documented seam where
a template engine could be added if the need appears in practice.

**Cost:** B plus the tax tables. **Risk:** low, and it defers the expensive
choice until there is evidence anyone needs template editing.

### D. Declare them out of scope for the Guile-free tier

The web tier keeps 18 reports and no invoices; a Guile build keeps the nine
documents. Honest, cheap, and leaves the business use case unserved.

## Recommendation

**C.** The invoice model and the tax tables are needed by *any* option except D,
they are ordinary engine work with a parity gate, and they unlock five
documents plus two tax reports. The template language is the only genuinely
expensive part of A, and nothing measured so far says users need to edit
templates — it says they need invoices that are correct.

Two things should be decided explicitly before work starts:

1. **Balance Sheet (eguile)** — recommend dropping it rather than porting: the
   native Balance Sheet already renders the same statement, and keeping a
   second implementation invites divergence.
2. **Customised templates** — for anyone who has edited their eguile templates,
   C means their customisation is lost. If that matters, A is the only answer
   that preserves the *idea*, and even A does not preserve the files.

## What the decision costs, in order

| step | work | unlocks |
|---|---|---|
| invoice + owner model in the native layer | weeks | everything below |
| tax tables + TXF/XML writers | weeks | US Tax Schedule, German tax report |
| five document renderers | weeks | invoices, receipts, tax invoices |
| template engine (only if chosen) | months | editable layouts |

Until the first line lands, the other three cannot start, which is why the
recommendation is to commit to the model and defer the language.
