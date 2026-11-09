/********************************************************************\
 * gnc-invoice.cpp -- read a GncInvoice into the shared document     *
 *                    model, and format its numbers                  *
 *                                                                  *
 * This program is free software; you can redistribute it and/or     *
 * modify it under the terms of the GNU General Public License as    *
 * published by the Free Software Foundation; either version 2 of    *
 * the License, or (at your option) any later version.               *
\********************************************************************/

#include <config.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include <glib.h>

#include "gnc-invoice.hpp"

extern "C"
{
#include <Account.h>
#include <Split.h>
#include <Transaction.h>
#include <gnc-lot.h>
#include <gnc-commodity.h>
#include <gnc-date.h>
#include <gnc-numeric.h>
#include <gnc-ui-util.h>
#include <gncAddress.h>
#include <gncBillTerm.h>
#include <gncCustomer.h>
#include <gncEmployee.h>
#include <gncEntry.h>
#include <gncInvoice.h>
#include <gncJob.h>
#include <gncOwner.h>
#include <gncTaxTable.h>
#include <gncVendor.h>
#include <guid.h>
#include <qof.h>
#include <qofbook.h>
#include <qofinstance.h>
}

#include <kvp-value.hpp>

namespace gnc
{
namespace report
{
namespace
{

std::string
text_or_empty (const char* text)
{
    return text != nullptr ? std::string (text) : std::string ();
}

/* Follow a job owner chain to the party a document is really addressed to,
 * which is what gnc:owner-get-name-dep and friends do. */
const GncOwner*
end_owner (const GncOwner* owner)
{
    while (owner != nullptr && gncOwnerGetType (owner) == GNC_OWNER_JOB)
    {
        GncJob* job = gncOwnerGetJob (owner);
        owner = job != nullptr ? gncJobGetOwner (job) : nullptr;
    }
    return owner;
}

OwnerKind
owner_kind (const GncOwner* owner)
{
    switch (gncOwnerGetType (owner))
    {
    case GNC_OWNER_CUSTOMER: return OwnerKind::Customer;
    case GNC_OWNER_JOB:      return OwnerKind::Job;
    case GNC_OWNER_VENDOR:   return OwnerKind::Vendor;
    case GNC_OWNER_EMPLOYEE: return OwnerKind::Employee;
    default:                 return OwnerKind::None;
    }
}

PostalAddress
owner_address (const GncOwner* owner)
{
    PostalAddress address;
    GncAddress* source = gncOwnerGetAddr (owner);
    if (source == nullptr)
        return address;
    address.name = text_or_empty (gncAddressGetName (source));
    address.line1 = text_or_empty (gncAddressGetAddr1 (source));
    address.line2 = text_or_empty (gncAddressGetAddr2 (source));
    address.line3 = text_or_empty (gncAddressGetAddr3 (source));
    address.line4 = text_or_empty (gncAddressGetAddr4 (source));
    address.phone = text_or_empty (gncAddressGetPhone (source));
    address.fax = text_or_empty (gncAddressGetFax (source));
    address.email = text_or_empty (gncAddressGetEmail (source));
    return address;
}

DocumentOwner
document_owner (const GncOwner* owner)
{
    DocumentOwner result;
    const GncOwner* party = end_owner (owner);
    if (party == nullptr)
        return result;
    result.kind = owner_kind (party);
    result.id = text_or_empty (gncOwnerGetID (party));
    result.name = text_or_empty (gncOwnerGetName (party));
    result.address = owner_address (party);
    return result;
}

/* The book's Business options, the source of the Scheme "company table".
 * gnc:company-info reads the same KVP path. */
std::string
business_option (QofBook* book, const char* key)
{
    if (book == nullptr)
        return {};
    GSList tail{const_cast<gpointer> (static_cast<const void*> (key)), nullptr};
    GSList head{const_cast<gpointer> (static_cast<const void*> ("Business")),
                &tail};
    KvpValue* value = qof_book_get_option (book, &head);
    if (value == nullptr)
        return {};
    const char* text = value->get<const char*> ();
    return text_or_empty (text);
}

/* Split a company address option into the address lines the document
 * prints.  The option is a single multiline string. */
void
split_lines (const std::string& text, PostalAddress& address)
{
    std::string* lines[] = {&address.line1, &address.line2, &address.line3,
                            &address.line4};
    size_t index = 0;
    size_t start = 0;
    while (start <= text.size () && index < 4)
    {
        const size_t end = text.find ('\n', start);
        const std::string line =
            text.substr (start, end == std::string::npos ? std::string::npos
                                                         : end - start);
        if (!line.empty ())
            *lines[index++] = line;
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
}

DocumentOwner
company_owner (QofBook* book)
{
    DocumentOwner company;
    if (book == nullptr)
        return company;
    company.name = business_option (book, "Company Name");
    company.id = business_option (book, "Company ID");
    split_lines (business_option (book, "Company Address"), company.address);
    company.address.phone = business_option (book, "Company Phone Number");
    company.address.fax = business_option (book, "Company Fax Number");
    company.address.email = business_option (book, "Company Email Address");
    return company;
}

DocumentKind
document_kind (const GncInvoice* invoice)
{
    switch (gncInvoiceGetType (invoice))
    {
    case GNC_INVOICE_VEND_INVOICE:       return DocumentKind::VendorBill;
    case GNC_INVOICE_EMPL_INVOICE:       return DocumentKind::EmployeeVoucher;
    case GNC_INVOICE_CUST_CREDIT_NOTE:   return DocumentKind::CustomerCreditNote;
    case GNC_INVOICE_VEND_CREDIT_NOTE:   return DocumentKind::VendorCreditNote;
    case GNC_INVOICE_EMPL_CREDIT_NOTE:   return DocumentKind::EmployeeCreditNote;
    case GNC_INVOICE_CUST_INVOICE:
    default:                             return DocumentKind::CustomerInvoice;
    }
}

bool
customer_document (DocumentKind kind)
{
    return kind == DocumentKind::CustomerInvoice ||
           kind == DocumentKind::CustomerCreditNote;
}

/* The document amounts.  These are the engine's own rounded values, the same
 * calls the Scheme invoice report makes, so the two renderers cannot round
 * differently. */
DocumentLine
document_line (GncEntry* entry, bool customer_doc, bool credit_note)
{
    DocumentLine line;
    line.date = gncEntryGetDate (entry);
    line.description = text_or_empty (gncEntryGetDescription (entry));
    line.action = text_or_empty (gncEntryGetAction (entry));
    line.notes = text_or_empty (gncEntryGetNotes (entry));
    line.quantity = gncEntryGetDocQuantity (entry, credit_note ? TRUE : FALSE);
    line.unit_price = customer_doc ? gncEntryGetInvPrice (entry)
                                   : gncEntryGetBillPrice (entry);
    line.discount = customer_doc ? gncEntryGetInvDiscount (entry)
                                 : gnc_numeric_zero ();
    line.discount_type = customer_doc &&
                                 gncEntryGetInvDiscountType (entry) ==
                                     GNC_AMT_TYPE_PERCENT
                             ? DiscountType::Percent
                             : DiscountType::Value;
    line.taxable = customer_doc ? gncEntryGetInvTaxable (entry) != FALSE
                                : gncEntryGetBillTaxable (entry) != FALSE;
    GncTaxTable* table = customer_doc ? gncEntryGetInvTaxTable (entry)
                                      : gncEntryGetBillTaxTable (entry);
    if (table != nullptr)
    {
        line.tax_table = text_or_empty (gncTaxTableGetName (table));
        line.taxable = line.taxable && !line.tax_table.empty ();
    }
    else
        line.taxable = false;
    line.amount = gncEntryGetDocValue (entry, TRUE, customer_doc ? TRUE : FALSE,
                                       credit_note ? TRUE : FALSE);
    line.tax = gncEntryGetDocTaxValue (entry, TRUE, customer_doc ? TRUE : FALSE,
                                       credit_note ? TRUE : FALSE);
    line.discount_value = gncEntryGetDocDiscountValue (
        entry, TRUE, customer_doc ? TRUE : FALSE, credit_note ? TRUE : FALSE);
    return line;
}

/* The payment rows of a posted invoice: the A/R or A/P splits in its lot
 * that do not belong to the invoice's own transaction, in transaction order.
 * A vendor bill's payments are sign-reversed, as the Scheme report's
 * reverse-payments? does. */
std::vector<DocumentPayment>
document_payments (GncInvoice* invoice, bool amount_positive)
{
    std::vector<DocumentPayment> payments;
    GNCLot* lot = gncInvoiceGetPostedLot (invoice);
    Transaction* posted = gncInvoiceGetPostedTxn (invoice);
    if (lot == nullptr || posted == nullptr)
        return payments;

    std::vector<Split*> splits;
    for (GList* node = gnc_lot_get_split_list (lot); node != nullptr;
         node = node->next)
        splits.push_back (static_cast<Split*> (node->data));
    std::sort (splits.begin (), splits.end (),
               [] (Split* left, Split* right)
               {
                   return xaccTransOrder (xaccSplitGetParent (left),
                                          xaccSplitGetParent (right)) < 0;
               });
    for (Split* split : splits)
    {
        if (xaccSplitGetParent (split) == posted)
            continue;
        DocumentPayment payment;
        payment.date = xaccTransGetDate (xaccSplitGetParent (split));
        const gnc_numeric amount = xaccSplitGetAmount (split);
        payment.amount = amount_positive ? amount : gnc_numeric_neg (amount);
        payments.push_back (payment);
    }
    return payments;
}

std::vector<DocumentTax>
document_taxes (GncInvoice* invoice)
{
    std::vector<DocumentTax> taxes;
    AccountValueList* list = gncInvoiceGetTotalTaxList (invoice);
    for (GList* node = list; node != nullptr; node = node->next)
    {
        GncAccountValue* value =
            static_cast<GncAccountValue*> (node->data);
        if (value == nullptr)
            continue;
        DocumentTax tax;
        tax.account = text_or_empty (xaccAccountGetName (value->account));
        tax.amount = value->value;
        taxes.push_back (tax);
    }
    gncAccountValueDestroy (list);
    return taxes;
}

} // namespace

/* ---------------------------------------------------------- formatting */

std::string
format_money (gnc_numeric amount, gnc_commodity* currency)
{
    if (currency == nullptr)
        return {};
    /* The Scheme renderer rounds to the commodity's smallest unit before
     * printing (gnc:default-html-gnc-monetary-renderer); do the same. */
    const gnc_numeric rounded = gnc_numeric_convert (
        amount, gnc_commodity_get_fraction (currency), GNC_HOW_RND_ROUND);
    const char* text =
        xaccPrintAmount (rounded, gnc_commodity_print_info (currency, TRUE));
    return text_or_empty (text);
}

std::string
format_price (gnc_numeric price, gnc_commodity* currency)
{
    if (currency == nullptr)
        return {};
    const char* text =
        xaccPrintAmount (price, gnc_price_print_info (currency, TRUE));
    return text_or_empty (text);
}

std::string
format_quantity (gnc_numeric quantity)
{
    const char* text = xaccPrintAmount (quantity, gnc_default_print_info (FALSE));
    return text_or_empty (text);
}

std::string
format_percent (gnc_numeric percent)
{
    const char* text = xaccPrintAmount (percent, gnc_default_print_info (FALSE));
    return text_or_empty (text) + " %";
}

std::string
format_date (time64 when)
{
    char buffer[MAX_DATE_LENGTH + 1] = {0};
    qof_print_date_buff (buffer, sizeof buffer, when);
    return std::string (buffer);
}

/* --------------------------------------------------------------- models */

bool
PostalAddress::empty () const
{
    return name.empty () && line1.empty () && line2.empty () &&
           line3.empty () && line4.empty ();
}

std::string
PostalAddress::multiline () const
{
    std::string text;
    for (const std::string* line : {&name, &line1, &line2, &line3, &line4})
    {
        if (line->empty ())
            continue;
        if (!text.empty ())
            text += "\n";
        text += *line;
    }
    return text;
}

const char*
document_kind_name (DocumentKind kind)
{
    switch (kind)
    {
    case DocumentKind::VendorBill:        return "Bill";
    case DocumentKind::EmployeeVoucher:   return "Expense Voucher";
    case DocumentKind::CustomerCreditNote:
    case DocumentKind::VendorCreditNote:
    case DocumentKind::EmployeeCreditNote:
        return "Credit Note";
    case DocumentKind::CustomerInvoice:
    default:
        return "Invoice";
    }
}

bool
invoice_document (GncInvoice* invoice, InvoiceDocument& document)
{
    if (invoice == nullptr)
        return false;

    document = InvoiceDocument{};
    document.id = text_or_empty (gncInvoiceGetID (invoice));
    document.billing_id = text_or_empty (gncInvoiceGetBillingID (invoice));
    document.notes = text_or_empty (gncInvoiceGetNotes (invoice));
    document.owner = document_owner (gncInvoiceGetOwner (invoice));
    document.company = company_owner (gncInvoiceGetBook (invoice));
    document.kind = document_kind (invoice);
    document.posted = gncInvoiceIsPosted (invoice) != FALSE;
    document.paid = gncInvoiceIsPaid (invoice) != FALSE;
    document.credit_note = gncInvoiceGetIsCreditNote (invoice) != FALSE;
    document.amount_positive = gncInvoiceAmountPositive (invoice) != FALSE;
    document.date_opened = gncInvoiceGetDateOpened (invoice);
    document.date_posted = gncInvoiceGetDatePosted (invoice);
    document.date_due = gncInvoiceGetDateDue (invoice);
    GncBillTerm* terms = gncInvoiceGetTerms (invoice);
    if (terms != nullptr)
        document.terms = text_or_empty (gncBillTermGetDescription (terms));
    document.currency = gncInvoiceGetCurrency (invoice);
    if (document.currency != nullptr)
        document.currency_code =
            text_or_empty (gnc_commodity_get_mnemonic (document.currency));

    const bool customer_doc = customer_document (document.kind);
    const bool credit_note = document.credit_note;
    for (GList* node = gncInvoiceGetEntries (invoice); node != nullptr;
         node = node->next)
        document.entries.push_back (document_line (
            static_cast<GncEntry*> (node->data), customer_doc, credit_note));

    document.taxes = document_taxes (invoice);
    document.subtotal = gncInvoiceGetTotalSubtotal (invoice);
    document.tax_total = gncInvoiceGetTotalTax (invoice);
    document.total = gncInvoiceGetTotal (invoice);
    document.payments = document_payments (invoice, document.amount_positive);
    document.amount_due = document.total;
    for (const DocumentPayment& payment : document.payments)
        document.amount_due = gnc_numeric_add (document.amount_due,
                                               payment.amount, GNC_DENOM_AUTO,
                                               GNC_HOW_DENOM_LCD);
    return true;
}

namespace
{

struct InvoiceSearch
{
    const std::string* id = nullptr;
    GncInvoice* found = nullptr;
    bool by_guid = false;
    GncGUID guid{};
};

void
find_invoice_cb (QofInstance* entity, gpointer data)
{
    InvoiceSearch* search = static_cast<InvoiceSearch*> (data);
    if (search->found != nullptr || entity == nullptr)
        return;
    GncInvoice* invoice = GNC_INVOICE (entity);
    if (search->by_guid)
    {
        if (guid_equal (qof_instance_get_guid (QOF_INSTANCE (invoice)),
                        &search->guid))
            search->found = invoice;
        return;
    }
    const char* id = gncInvoiceGetID (invoice);
    if (id != nullptr && *search->id == id)
        search->found = invoice;
}

void
collect_invoice_cb (QofInstance* entity, gpointer data)
{
    if (entity == nullptr)
        return;
    auto* invoices = static_cast<std::vector<InvoiceDocument>*> (data);
    InvoiceDocument document;
    if (invoice_document (GNC_INVOICE (entity), document))
        invoices->push_back (document);
}

} // namespace

bool
find_invoice_document (Account* root, const std::string& id,
                       InvoiceDocument& document, std::string* error)
{
    if (root == nullptr)
    {
        if (error != nullptr)
            *error = "no book is open";
        return false;
    }
    if (id.empty ())
    {
        if (error != nullptr)
            *error = "no invoice selected";
        return false;
    }

    QofBook* book = gnc_account_get_book (root);
    QofCollection* collection =
        book != nullptr ? qof_book_get_collection (book, GNC_ID_INVOICE)
                        : nullptr;
    if (collection == nullptr)
    {
        if (error != nullptr)
            *error = "no invoice with id '" + id + "' in this book";
        return false;
    }

    InvoiceSearch search;
    search.id = &id;
    if (id.size () == 32 && string_to_guid (id.c_str (), &search.guid))
        search.by_guid = true;
    qof_collection_foreach (collection, find_invoice_cb, &search);
    if (search.found == nullptr)
    {
        if (error != nullptr)
            *error = "no invoice with id '" + id + "' in this book";
        return false;
    }
    return invoice_document (search.found, document);
}

std::vector<InvoiceDocument>
invoice_documents (Account* root)
{
    std::vector<InvoiceDocument> documents;
    if (root == nullptr)
        return documents;
    QofBook* book = gnc_account_get_book (root);
    QofCollection* collection =
        book != nullptr ? qof_book_get_collection (book, GNC_ID_INVOICE)
                        : nullptr;
    if (collection == nullptr)
        return documents;
    qof_collection_foreach (collection, collect_invoice_cb, &documents);
    std::sort (documents.begin (), documents.end (),
               [] (const InvoiceDocument& left, const InvoiceDocument& right)
               {
                   return left.id < right.id;
               });
    return documents;
}

} // namespace report
} // namespace gnc
