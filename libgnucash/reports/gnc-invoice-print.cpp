/********************************************************************\
 * gnc-invoice-print.cpp -- the invoice document layouts             *
 *                                                                  *
 * The first consumer of the shared invoice model.  This file knows  *
 * how the Scheme invoice.scm documents are arranged and nothing     *
 * about how the engine stores an invoice; the receipt and the tax   *
 * invoices are siblings of this file, not changes to the model.     *
 *                                                                  *
 * invoice.scm registers three documents -- Printable, Easy and      *
 * Fancy Invoice -- from one renderer and one options-generator.     *
 * They are the same document: the variant only supplies two         *
 * defaults, the "CSS" text and the "Row 3 Right" slot, which is the *
 * print date in the first two and empty in the Fancy Invoice.  This *
 * file is that one renderer, with InvoiceLayout choosing the two    *
 * defaults, so a variant cannot drift from the other two.           *
 *                                                                  *
 * The markup keeps the Scheme document's class names               *
 * (main-table, client-table, company-table, invoice-details-table,  *
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

/* The Scheme multiline-to-html-text: every line is followed by a <br />,
 * including the last (gnc:html-markup-br after each split substring). */
std::string
multiline_html (const std::string& text)
{
    std::string out;
    for (const char c : text)
    {
        if (c == '\n')
            out += "<br />";
        else
            out += html_escape (std::string (1, c));
    }
    out += "<br />";
    return out;
}

/* The page chrome the native documents share.  The Scheme report's CSS
 * option (invoice_default_css) is applied after it, as the Scheme document
 * applies the option's text over the report engine's stylesheet. */
const char*
chrome_css ()
{
    return
        "html,body{margin:0 8px;color:#222;background:#fff;"
        "font-family:\"Noto Sans\",sans-serif;font-size:10pt}"
        "body,p,table,tr,td,a,th{vertical-align:top}"
        "table{border-collapse:collapse}"
        ".main-table{width:100%}"
        ".invoice-title{font-weight:bold;text-decoration:underline;"
        "font-size:14pt;margin:.4em 0}"
        ".client-table td,.company-table td,.invoice-details-table td{"
        "padding:0}"
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

std::string
stylesheet (const InvoicePrintOptions& options)
{
    std::string css = options.css.empty ()
                          ? invoice_default_css (options.layout)
                          : options.css;
    return std::string (chrome_css ()) + css;
}

/* The Scheme qof-print-date of (current-time): the day the document is
 * printed, in the book's date format. */
std::string
printed_today ()
{
    return format_date (gnc_time (nullptr));
}

/* make-file-url: "file:///" in front of anything that is not already a file
 * URL. */
std::string
file_url (const std::string& path)
{
    static const std::string prefix = "file:///";
    if (path.compare (0, prefix.size (), prefix) == 0)
        return path;
    return prefix + path;
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

/* The Scheme display-subtotal: when the Total column is hidden the document
 * prints the absolute value, so a credit or a customer payment does not read
 * as a negative total on a page with no Total column to explain it. */
std::string
display_amount (gnc_numeric amount, gnc_commodity* currency,
                const InvoicePrintOptions& options)
{
    if (!options.show_total && gnc_numeric_negative_p (amount))
        amount = gnc_numeric_neg (amount);
    return format_money (amount, currency);
}

/* ------------------------------------------------------------ header slots */

/* make-client-table: the name and address rows are unconditional, the owner
 * id and the REF rows are not.  This is the Scheme document's table, so it is
 * reproduced row for row rather than "when it has something to show". */
void
render_client_table (std::ostringstream& out, const InvoiceDocument& document,
                     const InvoicePrintOptions& options)
{
    out << "<div class='client-table'><table>";
    out << "<tr><td><div class='maybe-align-right client-name'>"
        << html_escape (document.owner.name) << "</div></td></tr>";
    const std::string address = document.owner.address.multiline ();
    out << "<tr><td><div class='maybe-align-right client-address'>"
        << multiline_html (address) << "</div></td></tr>";
    if (options.show_owner_id && !document.owner.id.empty ())
        out << "<tr><td><div class='maybe-align-right client-id'>"
            << multiline_html (document.owner.id) << "</div></td></tr>";
    if (options.show_references)
    {
        for (const std::string& reference : document.references)
            out << "<tr><td>REF " << html_escape (reference) << "</td></tr>";
    }
    out << "</table></div>";
}

/* make-company-table: every row is conditional on its value being non-empty,
 * unlike the client table.  The tax id is the book's Company ID and the tax
 * number its Tax / Tax Number option. */
void
render_company_table (std::ostringstream& out,
                      const InvoiceDocument& document)
{
    struct Row
    {
        const char* css;
        std::string value;
        bool multiline;
    };
    const DocumentOwner& company = document.company;
    const Row rows[] = {
        {"company-name", company.name, false},
        {"company-address", company.address.multiline (), true},
        {"company-phone", company.address.phone, false},
        {"company-fax", company.address.fax, false},
        {"company-email", company.address.email, false},
        {"company-url", company.url, false},
        {"company-tax-id", company.id, false},
        {"company-tax-nr", company.tax_number, false},
    };
    out << "<div class='company-table'><table>";
    for (const Row& row : rows)
    {
        if (row.value.empty ())
            continue;
        out << "<tr><td><div class='maybe-align-right " << row.css << "'>"
            << (row.multiline ? multiline_html (row.value)
                              : html_escape (row.value))
            << "</div></td></tr>";
    }
    out << "</table></div>";
}

/* make-invoice-details-table: date and due date (or the in-progress line),
 * the reference, the terms and the job rows. */
void
render_invoice_details (std::ostringstream& out,
                        const InvoiceDocument& document,
                        const InvoicePrintOptions& options)
{
    auto date_row = [&out] (const char* label, time64 when)
    {
        out << "<tr><td>" << label << ":</td><td><div class='div-align-right'>"
            << html_escape (format_date (when)) << "</div></td></tr>";
    };

    out << "<div class='invoice-details-table'><table>";
    if (document.posted)
    {
        date_row ("Date", document.date_posted);
        if (options.show_due_date && document.date_due != 0 &&
            document.date_due != G_MAXINT64)
            date_row ("Due Date", document.date_due);
    }
    else
        out << "<tr><td colspan='2'><span class='invoice-in-progress'>"
               "Invoice in progress&hellip;</span></td></tr>";

    if (options.show_billing_id && !document.billing_id.empty ())
    {
        out << "<tr><td>Reference:</td><td><div class='div-align-right'>"
            << multiline_html (document.billing_id) << "</div></td></tr>";
        /* The Scheme table appends an empty row after the reference. */
        out << "<tr></tr>";
    }
    if (options.show_terms && !document.terms.empty ())
        out << "<tr><td>Terms:</td><td><div class='div-align-right'>"
            << multiline_html (document.terms) << "</div></td></tr>";
    if (options.show_job_details && !document.job_number.empty ())
    {
        out << "<tr><td>Job number:</td><td><div class='div-align-right'>"
            << html_escape (document.job_number) << "</div></td></tr>";
        out << "<tr><td>Job name:</td><td><div class='div-align-right'>"
            << html_escape (document.job_name) << "</div></td></tr>";
    }
    out << "</table></div>";
}

void
render_picture (std::ostringstream& out, const InvoicePrintOptions& options)
{
    out << "<div class='picture'><img src='"
        << html_escape (file_url (options.picture)) << "'/></div>";
}

/* One slot's content.  The Scheme renderer for each slot is one of the
 * make-*-table functions above, make-today or make-picture; `none` is no
 * output at all. */
void
render_slot (std::ostringstream& out, InvoiceSlot slot,
             const InvoiceDocument& document,
             const InvoicePrintOptions& options)
{
    switch (slot)
    {
    case InvoiceSlot::Client:
        render_client_table (out, document, options);
        break;
    case InvoiceSlot::Company:
        render_company_table (out, document);
        break;
    case InvoiceSlot::Details:
        render_invoice_details (out, document, options);
        break;
    case InvoiceSlot::Today:
        out << "<div class='invoice-print-date'>" << printed_today ()
            << "</div>";
        break;
    case InvoiceSlot::Picture:
        render_picture (out, options);
        break;
    case InvoiceSlot::Empty:
    case InvoiceSlot::Default:
    default:
        break;
    }
}

/* One row of the Scheme main table: the left slot in a plain cell, the right
 * slot wrapped in div-align-right. */
void
header_row (std::ostringstream& out, InvoiceSlot left, InvoiceSlot right,
            const InvoiceDocument& document,
            const InvoicePrintOptions& options)
{
    out << "<tr><td>";
    render_slot (out, left, document, options);
    out << "</td><td><div class='div-align-right'>";
    render_slot (out, right, document, options);
    out << "</div></td></tr>\n";
}

/* ----------------------------------------------------------- entry table */

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

    /* The Scheme "Minimum # of entries" (the option the Fancy Invoice's
     * layout introduced): pad the table with blank rows, alternating the row
     * style the way the entry rows do. */
    for (int i = static_cast<int> (document.entries.size ());
         i < options.minimum_entries; ++i)
    {
        out << "<tr class='" << (odd ? "normal-row" : "alternate-row")
            << "'>";
        for (int column = 0; column < columns; ++column)
            out << "<td></td>";
        out << "</tr>\n";
        odd = !odd;
    }

    if (options.show_subtotal)
        total_row (out, "Net Price",
                   display_amount (document.subtotal, document.currency,
                                   options),
                   columns);
    if (options.show_detailed_tax)
    {
        /* One row per tax account, named as the account is, instead of the
         * single "Tax" row. */
        for (const DocumentTax& tax : document.taxes)
            total_row (out, tax.account,
                       display_amount (tax.amount, document.currency, options),
                       columns);
    }
    else
        total_row (out, "Tax",
                   display_amount (document.tax_total, document.currency,
                                   options),
                   columns);
    total_row (out, "Total Price",
               display_amount (document.total, document.currency, options),
               columns);

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
                << html_escape (display_amount (payment.amount,
                                                document.currency, options))
                << "</td></tr>";
        }
    }
    total_row (out, "Amount Due",
               display_amount (document.amount_due, document.currency,
                               options),
               columns);
    out << "</tbody></table></div>\n";
}

InvoiceSlot
resolved_slot (InvoiceSlot set, InvoiceLayout layout, int row, bool right)
{
    if (set != InvoiceSlot::Default)
        return set;
    InvoiceSlot fallback = InvoiceSlot::Empty;
    if (!invoice_default_slot (layout, row, right, fallback))
        return InvoiceSlot::Empty;
    return fallback;
}

} // namespace

std::string
render_invoice (const InvoiceDocument& document,
                const InvoicePrintOptions& options)
{
    const InvoiceLayout layout = options.layout;
    const std::string kinds = options.custom_title.empty ()
                                  ? document_kind_name (document.kind)
                                  : options.custom_title;
    const std::string title = kinds + " #" + document.id;

    std::ostringstream out;
    out << "<!DOCTYPE html>\n<html><head><meta charset='utf-8'><title>"
        << html_escape (title) << "</title><style>" << stylesheet (options)
        << "</style></head><body>\n<div class='main-table'><table>\n"
        << "<tr><td colspan='2'><div class='invoice-title'>"
        << html_escape (title) << "</div></td></tr>\n";

    header_row (out, resolved_slot (options.row1_left, layout, 1, false),
                resolved_slot (options.row1_right, layout, 1, true), document,
                options);
    header_row (out, resolved_slot (options.row2_left, layout, 2, false),
                resolved_slot (options.row2_right, layout, 2, true), document,
                options);
    header_row (out, resolved_slot (options.row3_left, layout, 3, false),
                resolved_slot (options.row3_right, layout, 3, true), document,
                options);

    out << "<tr><td colspan='2'>";
    render_entries (out, document, options);
    out << "</td></tr>\n";

    if (options.show_notes)
        out << "<tr><td colspan='2'><div class='invoice-notes'>"
            << multiline_html (document.notes) << "</div></td></tr>\n";

    if (options.show_payable_to && !document.company.name.empty ())
        out << "<tr><td colspan='2'><div class='invoice-footer-payable-to'>"
            << multiline_html (options.payable_to_string + ": " +
                               document.company.name)
            << "</div></td></tr>\n";

    if (options.show_company_contact && !document.company.contact.empty ())
        out << "<tr><td colspan='2'><div "
               "class='invoice-footer-company-contact'>"
            << multiline_html (options.company_contact_string + ": " +
                               document.company.contact)
            << "</div></td></tr>\n";

    /* The Scheme "Extra Notes" text option has a default and is always
     * printed (it has no display boolean); its default text is part of the
     * document. */
    out << "<tr><td colspan='2'><div class='invoice-notes'>"
        << multiline_html (options.extra_notes) << "</div></td></tr>\n";

    out << "</table></div>\n</body></html>\n";
    return out.str ();
}

std::string
render_easy_invoice (const InvoiceDocument& document,
                     const InvoicePrintOptions& options)
{
    InvoicePrintOptions easy = options;
    easy.layout = InvoiceLayout::Easy;
    return render_invoice (document, easy);
}

std::string
render_fancy_invoice (const InvoiceDocument& document,
                      const InvoicePrintOptions& options)
{
    InvoicePrintOptions fancy = options;
    fancy.layout = InvoiceLayout::Fancy;
    return render_invoice (document, fancy);
}

std::string
render_printable_invoice (const InvoiceDocument& document,
                          const InvoicePrintOptions& options)
{
    InvoicePrintOptions printable = options;
    printable.layout = InvoiceLayout::Printable;
    return render_invoice (document, printable);
}

std::string
render_invoice_not_selected ()
{
    /* The Scheme invoice report's gnc:html-make-generic-warning for an empty
     * "Invoice Number" option. */
    std::ostringstream out;
    out << "<!DOCTYPE html>\n<html><head><meta charset='utf-8'><title>"
           "Invoice</title><style>" << chrome_css ()
        << "</style></head><body><h3>Invoice:</h3>\n"
        << "<h3></h3>\n"
        << "<p>No valid invoice selected. Click on the Options button and "
           "select the invoice to use.</p>\n</body></html>\n";
    return out.str ();
}

} // namespace report
} // namespace gnc
