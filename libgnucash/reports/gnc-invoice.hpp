/********************************************************************\
 * gnc-invoice.hpp -- the engine's business documents as plain data  *
 *                                                                  *
 * The invoice model the document reports share.  It reads a        *
 * GncInvoice (owner, entries, tax tables, posted lot) and turns it  *
 * into a small tree of ordinary C++ values, with the engine's own   *
 * rounded amounts and the same currency formatting the Scheme       *
 * documents print.  The HTML of a particular document lives in      *
 * gnc-invoice-print.cpp: a receipt or a tax invoice can be rendered *
 * from the same tree without touching this file, and a future       *
 * template engine can consume it as well.                           *
 *                                                                  *
 * This program is free software; you can redistribute it and/or     *
 * modify it under the terms of the GNU General Public License as    *
 * published by the Free Software Foundation; either version 2 of    *
 * the License, or (at your option) any later version.               *
\********************************************************************/

#ifndef GNC_INVOICE_HPP
#define GNC_INVOICE_HPP

#include <optional>
#include <string>
#include <vector>

#include <Account.h>
#include <gnc-date.h>
#include <gnc-numeric.h>

typedef struct _gncInvoice GncInvoice;

namespace gnc
{
namespace report
{

/* --------------------------------------------------------------- owners */

/** The kind of party a document is addressed to.  This is the engine's
 *  GncOwnerType, reduced to the values a document prints. */
enum class OwnerKind
{
    None,
    Customer,
    Job,
    Vendor,
    Employee,
};

/** A postal address.  Every line is optional: the Scheme documents print
 *  only the non-empty ones, joined by newlines (gnc:owner-get-address-dep). */
struct PostalAddress
{
    std::string name;
    std::string line1;
    std::string line2;
    std::string line3;
    std::string line4;
    std::string phone;
    std::string fax;
    std::string email;

    bool empty () const;
    /** The name and lines 1-4, one per line, skipping empty ones. */
    std::string multiline () const;
};

/** The party a document is addressed to, resolved through the owner's job
 *  chain exactly as gnc:owner-get-name-dep / -address-dep / -owner-id do. */
struct DocumentOwner
{
    OwnerKind kind = OwnerKind::None;
    std::string id;
    std::string name;
    PostalAddress address;
    /** The Scheme "contact" row: gnc:*company-contact*.  Only the book's
     *  company identity sets it; a customer or vendor has no such field. */
    std::string contact;
    /** The Scheme company URL, gnc:*company-url*.  Company only. */
    std::string url;
    /** The book's tax number (the Scheme "Tax" / "Tax Number" option, which
     *  invoice.scm prints as the company table's company-tax-nr row).  The
     *  company only; empty when the book does not set one. */
    std::string tax_number;
};

/* -------------------------------------------------------------- invoices */

/** The document type, matching GncInvoiceType's printable distinctions. */
enum class DocumentKind
{
    CustomerInvoice,
    VendorBill,
    EmployeeVoucher,
    CustomerCreditNote,
    VendorCreditNote,
    EmployeeCreditNote,
};

const char* document_kind_name (DocumentKind kind);

/** How an entry's discount is expressed (GncAmountType). */
enum class DiscountType
{
    Value,
    Percent,
};

/** One entry of an entry's tax table, as the Tax Invoice's tax-rate column
 *  prints it: a percentage ("10.00%") or a fixed amount in the document
 *  currency. */
struct TaxRateEntry
{
    bool percent = true;
    gnc_numeric amount = gnc_numeric_zero ();
};

/** One invoice line.  The amounts are the engine's rounded document
 *  amounts, i.e. what gncEntryGetDocValue(entry, TRUE, ...) returns; the
 *  quantity is the document quantity, so a credit note shows the positive
 *  quantity printed on the paper rather than the negative stored one. */
struct DocumentLine
{
    time64 date = 0;
    std::string description;
    std::string action;
    std::string notes;
    gnc_numeric quantity = gnc_numeric_zero ();
    gnc_numeric unit_price = gnc_numeric_zero ();
    /** The unit price net of included tax, which the Tax Invoice's "Show
     *  net price" switch selects (gncEntryGetPrice(entry, cust, TRUE)). */
    gnc_numeric net_unit_price = gnc_numeric_zero ();
    gnc_numeric discount = gnc_numeric_zero ();
    DiscountType discount_type = DiscountType::Value;
    bool taxable = false;
    std::string tax_table;
    /** The tax table's own entries, for the Tax Invoice's tax-rate column.
     *  Empty when the entry has no tax table. */
    std::vector<TaxRateEntry> tax_rates;
    gnc_numeric amount = gnc_numeric_zero ();
    gnc_numeric tax = gnc_numeric_zero ();
    gnc_numeric discount_value = gnc_numeric_zero ();
};

/** One tax total, grouped by the account the tax posts to. */
struct DocumentTax
{
    std::string account;
    gnc_numeric amount = gnc_numeric_zero ();
};

/** A payment applied to the invoice, as the Scheme document prints it: the
 *  amount is the A/R or A/P split's own amount, so a customer receipt is a
 *  negative number and subtracts from the amount due. */
struct DocumentPayment
{
    time64 date = 0;
    gnc_numeric amount = gnc_numeric_zero ();
};

/** A whole invoice, ready to print.  This is the reusable part: it carries
 *  no HTML and no report option state. */
struct InvoiceDocument
{
    std::string id;
    std::string billing_id;
    std::string notes;
    DocumentOwner owner;
    /** The job the invoice's owner chain points at, when it is a job owner:
     *  the Tax Invoice's optional Job name and Job number rows. */
    std::string job_name;
    std::string job_number;
    /** The Scheme client table's "REF <reference>" rows: the references of
     *  the orders the invoice's entries belong to, unique, in the order
     *  invoice.scm's fold builds them (the entries reversed, first
     *  reference wins).  Empty when no entry has an order. */
    std::vector<std::string> references;
    /** The books's own business identity (the Scheme "company table"):
     *  name, address, phone, fax, email, URL and company id, read from the
     *  book's Business options. */
    DocumentOwner company;
    DocumentKind kind = DocumentKind::CustomerInvoice;
    bool posted = false;
    bool paid = false;
    bool credit_note = false;
    /** Whether the document increases the owner's balance (a customer
     *  invoice does, a vendor bill does not). */
    bool amount_positive = true;
    time64 date_opened = 0;
    time64 date_posted = 0;
    time64 date_due = 0;
    /** The billing term's description, e.g. "Net 30". */
    std::string terms;
    gnc_commodity* currency = nullptr;
    std::string currency_code;
    std::vector<DocumentLine> entries;
    std::vector<DocumentTax> taxes;
    std::vector<DocumentPayment> payments;
    gnc_numeric subtotal = gnc_numeric_zero ();
    gnc_numeric tax_total = gnc_numeric_zero ();
    gnc_numeric total = gnc_numeric_zero ();
    /** total plus the (signed) payments; what the customer still owes. */
    gnc_numeric amount_due = gnc_numeric_zero ();
};

/** Read one invoice's model from the engine.  The invoice is taken from the
 *  book that owns `root`; `id` is matched against gncInvoiceGetID() first,
 *  and, when it is a GUID, against the invoice's GUID.  Returns false with
 *  an explanation when no such invoice exists. */
bool invoice_document (GncInvoice* invoice, InvoiceDocument& document);

/** Look an invoice up in the book below `root` by its document id (or by
 *  its GUID) and read its model.  An empty id is the Scheme report's "no
 *  invoice selected" case and is reported as an error. */
bool find_invoice_document (Account* root, const std::string& id,
                            InvoiceDocument& document,
                            std::string* error = nullptr);

/** Every invoice in the book, sorted by document id, for the invoice
 *  picker.  A book with no invoices yields an empty list. */
std::vector<InvoiceDocument> invoice_documents (Account* root);

/* ------------------------------------------------------------ formatting */

/** A currency amount, rounded to the commodity's smallest unit and printed
 *  with the commodity's symbol, exactly as the Scheme documents' monetary
 *  renderer does. */
std::string format_money (gnc_numeric amount, gnc_commodity* currency);

/** A unit price: the commodity's price precision, which is what
 *  gnc:default-price-renderer uses (for USD two decimal places). */
std::string format_price (gnc_numeric price, gnc_commodity* currency);

/** A bare quantity, printed with the user's locale precision
 *  (gnc:default-html-gnc-numeric-renderer): "2.00" for a quantity of two. */
std::string format_quantity (gnc_numeric quantity);

/** A discount stored as a percentage: "10.00 %". */
std::string format_percent (gnc_numeric percent);

/** A date in the book's date format, as qof-print-date prints it. */
std::string format_date (time64 when);

/* ---------------------------------------------------------------- receipt */

/** Render the Receipt document (receipt.scm) from the same model.  The
 *  Scheme receipt has no display switches: it always prints the four
 *  product columns, the item count, the tax subtotals when the invoice has
 *  a tax table, the payment rows and the amount due.  It is a customer
 *  document only; for a vendor bill the Scheme report prints an explanation
 *  instead of a document, which render_receipt_not_customer() reproduces. */
std::string render_receipt (const InvoiceDocument& document);

/** The Receipt report's output when no invoice is selected. */
std::string render_receipt_not_selected ();

/** The Receipt report's output for a bill or expense voucher. */
std::string render_receipt_not_customer ();

/* ------------------------------------------------------------ tax invoices */

/** Which of the Tax Invoice report's display switches the renderer honours.
 *  The defaults are the Scheme report's: every Element page switch on
 *  except "Invoice number next to title", "Job number" and "Show net
 *  price".  The heading texts are not options here; the Australian variant
 *  changes the ones that differ. */
struct TaxInvoiceOptions
{
    /** The Scheme "Report Title"; empty means the report's own default
     *  ("Invoice", or "Tax Invoice" for the Australian variant). */
    std::string custom_title;
    bool show_date = true;                 /* column: Date */
    bool show_tax_rate = true;             /* column: Tax Rate */
    bool show_units = true;                /* column: Units */
    bool show_contact = true;              /* row: Contact */
    bool show_address = true;              /* row: Address */
    bool show_invoice_number = true;       /* row: Invoice Number */
    bool show_company_name = true;         /* row: Company Name */
    bool invoice_number_next_to_title = false;
    bool show_job_name = true;             /* Display Job name? */
    bool show_job_number = false;          /* Invoice Job number? */
    bool show_net_price = false;           /* Show net price? */
    bool border_collapse = false;          /* table-border-collapse */
};

/** Apply one of the Tax Invoice boolean display options by its descriptor
 *  name.  Returns false for a name the report does not have. */
bool set_tax_invoice_option (TaxInvoiceOptions& options,
                             const std::string& name, bool value);

/** Render the Tax Invoice document (taxinvoice.scm).  The Australian Tax
 *  Invoice is the same Scheme renderer with a different default option set
 *  (title, headings and extra CSS), so it is the same function with
 *  `australian` true. */
std::string render_tax_invoice (const InvoiceDocument& document,
                                const TaxInvoiceOptions& options,
                                bool australian);

/** The Tax Invoice report's output when no invoice is selected.  Both
 *  variants print "Tax Invoice" here, as the Scheme template does. */
std::string render_tax_invoice_not_selected ();

/* ------------------------------------------------------- printable invoice */

/** Which of the Scheme invoice report's three registrations a document is
 *  rendered as.  invoice.scm registers "Printable Invoice", "Easy Invoice"
 *  and "Fancy Invoice" from one renderer and one options-generator; the
 *  three differ only in two defaults, the "CSS" text and the "Row 3 Right"
 *  slot (the print date).  Everything else -- the option pages, the layout
 *  rows, the entry table -- is the same document, so it is one renderer
 *  here too, with the variant choosing those two defaults. */
enum class InvoiceLayout
{
    Printable,
    Easy,
    Fancy,
};

/** One of the six header slots of the Scheme layout.  The Scheme option value
 *  is a symbol: none, client, company, invoice, today or picture. */
enum class InvoiceSlot
{
    /** Not set: the layout's own default for that slot. */
    Default,
    Empty,
    Client,
    Company,
    Details,
    Today,
    Picture,
};

/** Resolve a report name to its layout.  False for a name that is not one of
 *  the three invoice documents. */
bool invoice_layout_from_name (const std::string& name, InvoiceLayout& layout);

/** The Scheme variant's default for one header slot.  `row` is 1..3 and
 *  `right` says which half of the row; false for a row outside 1..3. */
bool invoice_default_slot (InvoiceLayout layout, int row, bool right,
                           InvoiceSlot& slot);

/** The text of one slot as the API writes it: none, client, company, invoice,
 *  today or picture.  "default" is InvoiceSlot::Default. */
const char* invoice_slot_name (InvoiceSlot slot);

/** Parse an API slot value.  False for a name that is not a slot. */
bool invoice_slot_from_name (const std::string& name, InvoiceSlot& slot);

/** The Scheme "CSS" option's default for a layout: the shared base
 *  stylesheet, plus the rules the Easy and Fancy variants add. */
std::string invoice_default_css (InvoiceLayout layout);

/** Which of the Scheme invoice report's display switches and layout choices
 *  the native invoice documents honour.  The defaults are the Scheme
 *  report's defaults; the layout the document is rendered as supplies the two
 *  defaults that differ between the three registrations (the CSS text and
 *  "Row 3 Right"), so a default-constructed option set renders the Printable
 *  Invoice. */
struct InvoicePrintOptions
{
    /** Which registration the document is rendered as.  The renderer sets it
     *  from the report name; it is not an option. */
    InvoiceLayout layout = InvoiceLayout::Printable;
    /** The Scheme "Custom Title"; empty means the document type's own name
     *  ("Invoice", "Bill", ...). */
    std::string custom_title;
    /** The Scheme "CSS" text.  Empty means the layout's own stylesheet
     *  (invoice_default_css), which is the Scheme default. */
    std::string css;
    /** The Scheme "Picture Location".  Empty means no picture; the value is
     *  turned into a file URL the way make-file-url does. */
    std::string picture;
    /* The six header slots, in the Scheme's "Layout" page order.  A Default
     * slot takes the layout's own value (invoice_default_slot). */
    InvoiceSlot row1_left = InvoiceSlot::Default;
    InvoiceSlot row1_right = InvoiceSlot::Default;
    InvoiceSlot row2_left = InvoiceSlot::Default;
    InvoiceSlot row2_right = InvoiceSlot::Default;
    InvoiceSlot row3_left = InvoiceSlot::Default;
    InvoiceSlot row3_right = InvoiceSlot::Default;
    /** The Scheme "Minimum # of entries": the entry table is padded with
     *  blank rows up to this many. */
    int minimum_entries = 1;
    bool show_date = true;
    bool show_description = true;
    bool show_action = true;
    bool show_quantity = true;
    bool show_price = true;
    bool show_discount = true;
    bool show_taxable = true;
    bool show_tax_amount = false;
    bool show_total = true;
    bool show_due_date = true;
    bool show_subtotal = true;
    bool show_billing_id = true;
    bool show_owner_id = false;
    bool show_notes = false;
    bool show_terms = true;
    bool show_payments = true;
    /** The Scheme "References": the client table's REF rows. */
    bool show_references = true;
    /** The Scheme "Use Detailed Tax Summary": one tax row per tax account
     *  instead of one "Tax" row. */
    bool show_detailed_tax = false;
    /** The Scheme "Job Details": the invoice details' Job number/name rows. */
    bool show_job_details = false;
    /** The Scheme "Payable to" switch and its phrase. */
    bool show_payable_to = false;
    std::string payable_to_string = "Please make all checks payable to";
    /** The Scheme "Company contact" switch and its phrase. */
    bool show_company_contact = false;
    std::string company_contact_string = "Please direct all enquiries to";
    /** The Scheme "Extra Notes" text, printed at the foot of every layout. */
    std::string extra_notes = "Thank you for your patronage!";
};

/** Apply one of the boolean display options by its descriptor name
 *  ("show_date", ...).  Returns false for a name the printable invoice
 *  does not have, so the route can reject it rather than ignore it. */
bool set_invoice_print_option (InvoicePrintOptions& options,
                               const std::string& name, bool value);

/** Apply a text, choice or number option by its descriptor name: the CSS, the
 *  picture location, the six layout slots, the minimum entry count, the two
 *  phrase texts and the extra notes.  Returns false for a name this document
 *  does not have; the route has already validated a choice against the
 *  descriptor and a number against its range. */
bool set_invoice_print_value (InvoicePrintOptions& options,
                              const std::string& name,
                              const std::string& value);

/** Render the invoice document natively: the scheme invoice.scm layout,
 *  which is the Printable Invoice for the default option set and the Easy or
 *  Fancy Invoice when `options.layout` says so.  The three variants share
 *  this renderer, exactly as they share one in Scheme; the variant supplies
 *  the default stylesheet and the third row's right-hand slot.
 *
 *  The markup mirrors the Scheme document's class names (main-table,
 *  client-table, company-table, invoice-details-table, entries-table,
 *  number-cell, total-number-cell) so one extractor can read both. */
std::string render_invoice (const InvoiceDocument& document,
                            const InvoicePrintOptions& options);

/** The Easy Invoice layout: the same document with the Easy variant's default
 *  stylesheet, for callers that name the variant rather than set it. */
std::string render_easy_invoice (const InvoiceDocument& document,
                                 const InvoicePrintOptions& options);

/** The Fancy Invoice layout: the same document with the Fancy variant's
 *  default stylesheet and no print-date row. */
std::string render_fancy_invoice (const InvoiceDocument& document,
                                  const InvoicePrintOptions& options);

/** The Printable Invoice layout.  Kept as the name the first document's
 *  callers and tests use; it is render_invoice with the Printable variant. */
std::string render_printable_invoice (const InvoiceDocument& document,
                                      const InvoicePrintOptions& options);

/** The Scheme invoice report's output when no invoice is selected. */
std::string render_invoice_not_selected ();

} // namespace report
} // namespace gnc

#endif
