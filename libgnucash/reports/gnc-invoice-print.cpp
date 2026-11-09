/********************************************************************\
 * gnc-invoice-print.cpp -- the Printable Invoice document            *
 *                                                                  *
 * The first consumer of the shared invoice model.  This file knows  *
 * how the "Printable Invoice" layout is arranged and nothing about  *
 * how the engine stores an invoice; a receipt or a tax invoice      *
 * renderer would be a sibling of this file, not a change to the     *
 * model.  The markup keeps the Scheme document's class names         *
 * (client-table, company-table, invoice-details-table,              *
 * entries-table, number-cell, total-number-cell) so the parity      *
 * extractor reads both engines with one parser.                     *
 *                                                                  *
 * This program is free software; you can redistribute it and/or     *
 * modify it under the terms of the GNU General Public License as    *
 * published by the Free Software Foundation; either version 2 of    *
 * the License, or (at your option) any later version.               *
\********************************************************************/

#include <config.h>

#include <sstream>
#include <string>
#include <utility>

#include <glib.h>

#include "gnc-invoice.hpp"

namespace gnc
{
namespace report
{
namespace
{

std::string
html_escape (const std::string& text)
{
    std::string out;
    out.reserve (text.size ());
    for (const char c : text)
    {
        switch (c)
        {
        case '&':  out += "&amp;"; break;
        case '<':  out += "&lt;"; break;
        case '>':  out += "&gt;"; break;
        case '"':  out += "&quot;"; break;
        case '\'': out += "&#39;"; break;
        default:   out += c;
        }
    }
    return out;
}

/* The Scheme multiline-to-html-text: one line per <br>. */
std::string
multiline_html (const std::string& text)
{
    std::string out;
    for (const char c : text)
    {
        if (c == '\n')
            out += "<br>";
        else
            out += html_escape (std::string (1, c));
    }
    return out;
}

const char*
stylesheet ()
{
    /* The invoice's own stylesheet, as the Scheme report's "CSS" option
     * carries it: the shared base plus the entries-table rules. */
    return
        "html,body{margin:0 8px;color:#222;background:#fff;"
        "font-family:\"Noto Sans\",sans-serif;font-size:10pt}"
        "body,p,table,tr,td,a,th{vertical-align:top}"
        "table{border-collapse:collapse}"
        ".main-table{width:100%}"
        ".invoice-title{font-weight:bold;text-decoration:underline;"
        "font-size:14pt;margin:.4em 0}"
        ".client-name,.company-name{font-weight:bold;font-size:12pt}"
        ".client-table,.company-table,.invoice-details-table{padding:0}"
        ".client-table td,.company-table td,.invoice-details-table td{padding:0}"
        ".div-align-right{text-align:right}"
        ".entries-table{width:100%;margin-top:1em}"
        ".entries-table table{width:100%;border-collapse:collapse}"
        ".entries-table th,.entries-table td{border:1px solid #999;"
        "padding:3px}"
        ".entries-table th{text-align:left}"
        ".entries-table td.number-cell,.entries-table td.total-number-cell,"
        ".entries-table th.number-heading{text-align:right}"
        ".entries-table td.total-label-cell,.entries-table td.total-number-cell"
        "{font-weight:bold}"
        ".invoice-notes{margin-top:16px}"
        "@media print{.main-table>table{width:100%}}";
}

bool
customer_document (DocumentKind kind)
{
    return kind == DocumentKind::CustomerInvoice ||
           kind == DocumentKind::CustomerCreditNote;
}

int
column_count (const InvoicePrintOptions& options)
{
    int count = 0;
    count += options.show_date ? 1 : 0;
    count += options.show_description ? 1 : 0;
    count += options.show_action ? 1 : 0;
    count += options.show_quantity ? 1 : 0;
    count += options.show_price ? 1 : 0;
    count += options.show_discount ? 1 : 0;
    count += options.show_taxable ? 1 : 0;
    count += options.show_tax_amount ? 1 : 0;
    count += options.show_total ? 1 : 0;
    return count;
}

void
number_cell (std::ostringstream& out, const std::string& text)
{
    out << "<td class='number-cell'>" << html_escape (text) << "</td>";
}

void
total_row (std::ostringstream& out, const std::string& label,
           const std::string& amount, int columns)
{
    out << "<tr class='grand-total'><td class='total-label-cell'>"
        << html_escape (label) << "</td><td class='total-number-cell' colspan='"
        << (columns > 1 ? columns - 1 : 1) << "'>" << html_escape (amount)
        << "</td></tr>";
}

void
render_owner_table (std::ostringstream& out, const char* css,
                    const DocumentOwner& owner, bool show_id)
{
    if (owner.name.empty () && owner.address.empty () &&
        !(show_id && !owner.id.empty ()))
        return;
    out << "<div class='" << css << "'><table>";
    if (!owner.name.empty ())
        out << "<tr><td class='" << css << "-name'>"
            << html_escape (owner.name) << "</td></tr>";
    const std::string address = owner.address.multiline ();
    if (!address.empty ())
        out << "<tr><td class='" << css << "-address'>"
            << multiline_html (address) << "</td></tr>";
    if (show_id && !owner.id.empty ())
        out << "<tr><td class='" << css << "-id'>" << html_escape (owner.id)
            << "</td></tr>";
    /* The Scheme company table also prints the company's contact details;
     * the client table does not, so these rows are company-only. */
    if (std::string (css) == "company-table")
    {
        const std::pair<const char*, const std::string*> company_rows[] = {
            {"company-phone", &owner.address.phone},
            {"company-fax", &owner.address.fax},
            {"company-email", &owner.address.email},
        };
        for (const auto& row : company_rows)
        {
            if (row.second->empty ())
                continue;
            out << "<tr><td class='" << row.first << "'>"
                << html_escape (*row.second) << "</td></tr>";
        }
    }
    out << "</table></div>";
}

void
detail_row (std::ostringstream& out, const std::string& label,
            const std::string& value)
{
    out << "<tr><td>" << html_escape (label)
        << ":</td><td><div class='div-align-right'>"
        << multiline_html (value) << "</div></td></tr>";
}

void
render_invoice_details (std::ostringstream& out,
                        const InvoiceDocument& document,
                        const InvoicePrintOptions& options)
{
    out << "<div class='invoice-details-table'><table>";
    if (document.posted)
    {
        detail_row (out, "Date", format_date (document.date_posted));
        if (options.show_due_date && document.date_due != 0 &&
            document.date_due != G_MAXINT64)
            detail_row (out, "Due Date", format_date (document.date_due));
    }
    else
        out << "<tr><td colspan='2'><span class='invoice-in-progress'>"
               "Invoice in progress&hellip;</span></td></tr>";
    if (options.show_billing_id && !document.billing_id.empty ())
        detail_row (out, "Reference", document.billing_id);
    if (options.show_terms && !document.terms.empty ())
        detail_row (out, "Terms", document.terms);
    out << "</table></div>";
}

void
render_entries (std::ostringstream& out, const InvoiceDocument& document,
                const InvoicePrintOptions& options)
{
    const int columns = column_count (options);
    const bool customer = customer_document (document.kind);
    out << "<div class='entries-table'><table cellspacing='0' cellpadding='4' "
           "border='0'><thead><tr>";
    if (options.show_date)
        out << "<th>Date</th>";
    if (options.show_description)
        out << "<th>Description</th>";
    if (options.show_action)
        out << "<th>Action</th>";
    if (options.show_quantity)
        out << "<th class='number-heading'>Quantity</th>";
    if (options.show_price)
        out << "<th class='number-heading'>Unit Price</th>";
    if (options.show_discount)
        out << "<th class='number-heading'>Discount</th>";
    if (options.show_taxable)
        out << "<th>Taxable</th>";
    if (options.show_tax_amount)
        out << "<th class='number-heading'>Tax Amount</th>";
    if (options.show_total)
        out << "<th class='number-heading'>Total</th>";
    out << "</tr></thead><tbody>\n";

    bool odd = true;
    for (const DocumentLine& line : document.entries)
    {
        out << "<tr class='" << (odd ? "normal-row" : "alternate-row")
            << "'>";
        if (options.show_date)
            out << "<td>" << html_escape (format_date (line.date))
                << "</td>";
        if (options.show_description)
            out << "<td>" << html_escape (line.description) << "</td>";
        if (options.show_action)
            out << "<td>" << html_escape (line.action) << "</td>";
        if (options.show_quantity)
            number_cell (out, format_quantity (line.quantity));
        if (options.show_price)
            number_cell (out, format_price (line.unit_price,
                                            document.currency));
        if (options.show_discount)
        {
            if (customer)
                number_cell (out,
                             line.discount_type == DiscountType::Percent
                                 ? format_percent (line.discount)
                                 : format_money (line.discount,
                                                 document.currency));
            else
                out << "<td></td>";
        }
        if (options.show_taxable)
            out << "<td>" << (line.taxable ? "T" : "") << "</td>";
        if (options.show_tax_amount)
            number_cell (out, format_money (line.tax, document.currency));
        if (options.show_total)
            number_cell (out, format_money (line.amount, document.currency));
        out << "</tr>\n";
        odd = !odd;
    }

    if (options.show_subtotal)
        total_row (out, "Net Price",
                   format_money (document.subtotal, document.currency),
                   columns);
    total_row (out, "Tax", format_money (document.tax_total, document.currency),
               columns);
    total_row (out, "Total Price",
               format_money (document.total, document.currency), columns);

    if (options.show_payments)
    {
        for (const DocumentPayment& payment : document.payments)
        {
            /* The Scheme document prints a payment as a date and the words
             * "Payment, thank you!" with the amount in one spanning cell. */
            int used = 0;
            out << "<tr class='grand-total'>";
            if (options.show_date)
            {
                out << "<td>" << html_escape (format_date (payment.date))
                    << "</td>";
                ++used;
            }
            if (options.show_description)
            {
                out << "<td>Payment, thank you!</td>";
                ++used;
            }
            out << "<td class='total-number-cell' colspan='"
                << (columns - used > 1 ? columns - used : 1) << "'>"
                << html_escape (format_money (payment.amount,
                                              document.currency))
                << "</td></tr>";
        }
    }
    total_row (out, "Amount Due",
               format_money (document.amount_due, document.currency), columns);
    out << "</tbody></table></div>\n";
}

} // namespace

bool
set_invoice_print_option (InvoicePrintOptions& options,
                          const std::string& name, bool value)
{
    if (name == "show_date")         options.show_date = value;
    else if (name == "show_description") options.show_description = value;
    else if (name == "show_action")  options.show_action = value;
    else if (name == "show_quantity") options.show_quantity = value;
    else if (name == "show_price")   options.show_price = value;
    else if (name == "show_discount") options.show_discount = value;
    else if (name == "show_taxable") options.show_taxable = value;
    else if (name == "show_tax_amount") options.show_tax_amount = value;
    else if (name == "show_total")   options.show_total = value;
    else if (name == "show_due_date") options.show_due_date = value;
    else if (name == "show_subtotal") options.show_subtotal = value;
    else if (name == "show_billing_id") options.show_billing_id = value;
    else if (name == "show_owner_id") options.show_owner_id = value;
    else if (name == "show_notes")   options.show_notes = value;
    else if (name == "show_terms")   options.show_terms = value;
    else if (name == "show_payments") options.show_payments = value;
    else
        return false;
    return true;
}

std::string
render_printable_invoice (const InvoiceDocument& document,
                          const InvoicePrintOptions& options)
{
    const std::string kinds = options.custom_title.empty ()
                                  ? document_kind_name (document.kind)
                                  : options.custom_title;
    const std::string title = kinds + " #" + document.id;

    std::ostringstream out;
    out << "<!DOCTYPE html>\n<html><head><meta charset='utf-8'><title>"
        << html_escape (title) << "</title><style>" << stylesheet ()
        << "</style></head><body>\n<div class='main-table'>\n"
        << "<div class='invoice-title'>" << html_escape (title) << "</div>\n"
        << "<table class='header-table'><tr><td>";
    render_owner_table (out, "client-table", document.owner,
                        options.show_owner_id);
    out << "</td><td>";
    render_invoice_details (out, document, options);
    out << "</td></tr><tr><td>";
    render_owner_table (out, "company-table", document.company, true);
    out << "</td><td></td></tr></table>\n";

    render_entries (out, document, options);

    if (options.show_notes && !document.notes.empty ())
        out << "<div class='invoice-notes'>" << multiline_html (document.notes)
            << "</div>\n";

    /* The Scheme report's "Extra Notes" text option has a default and is
     * always printed (it has no display boolean); its default text is part of
     * the document. */
    out << "<div class='invoice-notes'>Thank you for your patronage!</div>\n";

    out << "</div>\n</body></html>\n";
    return out.str ();
}

std::string
render_invoice_not_selected ()
{
    /* The Scheme invoice report's gnc:html-make-generic-warning for an empty
     * "Invoice Number" option. */
    std::ostringstream out;
    out << "<!DOCTYPE html>\n<html><head><meta charset='utf-8'><title>"
           "Invoice</title><style>" << stylesheet ()
        << "</style></head><body><h3>Invoice:</h3>\n"
        << "<h3></h3>\n"
        << "<p>No valid invoice selected. Click on the Options button and "
           "select the invoice to use.</p>\n</body></html>\n";
    return out.str ();
}

} // namespace report
} // namespace gnc
