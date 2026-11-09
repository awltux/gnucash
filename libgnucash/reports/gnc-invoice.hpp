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
    gnc_numeric discount = gnc_numeric_zero ();
    DiscountType discount_type = DiscountType::Value;
    bool taxable = false;
    std::string tax_table;
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

/* ------------------------------------------------------- printable invoice */

/** Which of the Scheme invoice report's display switches the printable
 *  invoice honours.  The defaults are the Scheme report's defaults:
 *  every column except the per-entry tax amount, the due date, subtotal,
 *  billing id, billing terms and payments on; notes and the owner id off. */
struct InvoicePrintOptions
{
    /** The Scheme "Custom Title"; empty means the document type's own name
     *  ("Invoice", "Bill", ...). */
    std::string custom_title;
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
};

/** Apply one of the boolean display options by its descriptor name
 *  ("show_date", ...).  Returns false for a name the printable invoice
 *  does not have, so the route can reject it rather than ignore it. */
bool set_invoice_print_option (InvoicePrintOptions& options,
                               const std::string& name, bool value);

/** Render the customer/vendor invoice document natively.  This is the
 *  "Printable Invoice" layout, the simplest of the three Scheme invoice
 *  layouts: it is the base variant, whose only stylesheet is the shared
 *  base CSS (the Easy and Fancy variants add layout rules and change the
 *  header rows).  The markup mirrors the Scheme document's class names
 *  (client-table, company-table, invoice-details-table, entries-table,
 *  number-cell, total-number-cell) so one extractor can read both. */
std::string render_printable_invoice (const InvoiceDocument& document,
                                      const InvoicePrintOptions& options);

/** The Scheme invoice report's output when no invoice is selected. */
std::string render_invoice_not_selected ();

} // namespace report
} // namespace gnc

#endif
