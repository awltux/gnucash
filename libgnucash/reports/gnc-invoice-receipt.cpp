/********************************************************************\
 * gnc-invoice-receipt.cpp -- the Receipt document                    *
 *                                                                  *
 * The second consumer of the shared invoice model, and a sibling of *
 * gnc-invoice-print.cpp: it renders the Scheme receipt.scm layout   *
 * from the same InvoiceDocument, with no engine access of its own.  *
 * The receipt is the paid-document layout, not a shortened invoice: *
 * it always prints the four product columns and the item count, and *
 * it prints the tax and total rows only when the invoice has a tax   *
 * table or an applied payment.  It is a customer document; the      *
 * Scheme report prints an explanation for a bill instead.           *
 *                                                                  *
 * The markup keeps the Scheme document's class names (entries,      *
 * total, subtotal, total_first, total_last, number-cell) so the      *
 * parity extractor reads both engines with one parser.              *
 *                                                                  *
 * This program is free software; you can redistribute it and/or     *
 * modify it under the terms of the GNU General Public License as    *
 * published by the Free Software Foundation; either version 2 of    *
 * the License, or (at your option) any later version.               *
\********************************************************************/

#include <config.h>

#include <cstdio>
#include <ctime>
#include <sstream>
#include <string>

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

/* The Scheme multiline-to-html-text: one line per <br/>. */
std::string
multiline_html (const std::string& text)
{
    std::string out;
    for (const char c : text)
    {
        if (c == '\n')
            out += "<br/>";
        else
            out += html_escape (std::string (1, c));
    }
    return out;
}

const char*
stylesheet ()
{
    /* receipt.css, plus the inline font rules the Scheme template adds.
     * The layout classes (entries, total, subtotal, total_first,
     * total_last) are the Scheme document's. */
    return
        "div#content{margin:1em}"
        "body{font-family:\"Noto Sans\",sans-serif;font-size:10pt;color:#222;"
        "background:#fff}"
        "table{border-collapse:collapse}"
        "table.entries{width:100%;border:none}"
        "table.entries th{text-align:left;border-bottom:1px solid #999;"
        "padding:2px 4px}"
        "table.entries td{padding:2px 4px;vertical-align:top}"
        "td.total,td.subtotal{font-weight:bold}"
        "td.total_first,td.total_last{font-weight:bold}"
        "span.nowrap{white-space:nowrap}"
        "@media print{div#content{margin:0}}";
}

/* The Scheme receipt's "Today date format" default:
 * "%l:%M %P, %e %B %Y" -- " 2:20 pm,  9 November 2026".  Implemented by
 * hand because %l and %P are C library extensions. */
std::string
today_text ()
{
    const time64 now = gnc_time (nullptr);
    struct tm local;
    if (gnc_localtime_r (&now, &local) == nullptr)
        return format_date (now);
    static const char* months[] = {"January", "February", "March", "April",
                                   "May", "June", "July", "August",
                                   "September", "October", "November",
                                   "December"};
    int hour = local.tm_hour % 12;
    if (hour == 0)
        hour = 12;
    const char* ampm = local.tm_hour < 12 ? "am" : "pm";
    char buffer[128] = {0};
    g_snprintf (buffer, sizeof buffer, "%2d:%02d %s, %2d %s %d", hour,
                local.tm_min, ampm, local.tm_mday,
                months[local.tm_mon >= 0 && local.tm_mon < 12 ? local.tm_mon
                                                              : 0],
                local.tm_year + 1900);
    return buffer;
}

bool
has_tax (const InvoiceDocument& document)
{
    for (const DocumentLine& line : document.entries)
    {
        if (!line.tax_table.empty ())
            return true;
    }
    return false;
}

void
total_row (std::ostringstream& out, const std::string& label,
           const std::string& amount, const char* css, bool first,
           bool last)
{
    out << "<tr valign='top'><td align='left' class='" << css;
    if (first)
        out << " total_first";
    if (last)
        out << " total_last";
    out << "' colspan='3'><strong>" << html_escape (label)
        << "</strong></td><td align='right' class='" << css;
    if (first)
        out << " total_first";
    if (last)
        out << " total_last";
    out << "' colspan='2'><strong>" << html_escape (amount)
        << "</strong></td></tr>\n";
}

} // namespace

std::string
render_receipt (const InvoiceDocument& document)
{
    const bool taxed = has_tax (document);
    const bool paid = !document.payments.empty ();

    std::ostringstream out;
    out << "<!DOCTYPE html>\n<html dir='auto'><head>"
           "<meta http-equiv='Content-Type' content='text/html;charset=utf-8'>"
           "<title>Invoice "
        << html_escape (document.id) << "</title><style>" << stylesheet ()
        << "</style></head><body>\n<div id='content'>\n"
        << "<h1>Invoice&nbsp;No. " << html_escape (document.id) << "</h1>\n"
        << "<h2>" << html_escape (today_text ()) << "</h2>\n";
    if (!document.owner.address.name.empty ())
        out << "<p>Attn: " << html_escape (document.owner.address.name)
            << "</p><br>\n";
    const std::string name_and_address =
        document.owner.name.empty ()
            ? document.owner.address.multiline ()
            : document.owner.name + "\n" + document.owner.address.multiline ();
    if (!name_and_address.empty ())
        out << "<p>" << multiline_html (name_and_address) << "</p>\n";

    out << "<div id='products'>\n<table border='0' width='100%' "
           "class='entries'><thead><tr>"
           "<th align='left'>Date</th><th align='left'>Descr.</th>"
           "<th align='right'>Qty</th><th align='right'>Unit Price</th>"
           "<th align='right'>Net Price</th></tr></thead><tbody>\n";

    for (const DocumentLine& line : document.entries)
    {
        out << "<tr valign='top'>"
            << "<td align='left'>" << html_escape (format_date (line.date))
            << "</td><td align='left'>" << html_escape (line.description)
            << "</td><td align='right'>"
            << html_escape (format_quantity (line.quantity))
            << "</td><td align='right'>"
            << html_escape (format_price (line.unit_price, document.currency))
            << "</td><td align='right' nowrap>"
            << html_escape (format_money (line.amount, document.currency));
        if (taxed)
            out << "&nbsp;T";
        else
            out << "&nbsp;&nbsp;";
        out << "</td></tr>\n";
        /* The Scheme template prints this row exactly when the entry has
         * no notes, not when it has them; keep that behaviour so the two
         * documents have the same rows. */
        if (line.notes.empty ())
            out << "<tr><td align='left'>&nbsp;</td>"
                   "<td align='left' colspan='4'></td></tr>\n";
    }

    out << "<tr valign='top'><td align='center' class='total total_first' "
           "colspan='5'>Total No. Items:&nbsp; "
        << document.entries.size () << "</td></tr>\n";

    if (taxed)
    {
        out << "<tr valign='top'><td align='left' class='subtotal' "
               "colspan='3'><strong>Net Price</strong></td>"
               "<td align='right' class='subtotal' colspan='2'><strong>"
            << html_escape (format_money (document.subtotal,
                                          document.currency))
            << "</strong></td></tr>\n"
               "<tr valign='top'><td align='left' class='subtotal' "
               "colspan='3'><strong>Tax Amount</strong></td>"
               "<td align='right' class='subtotal' colspan='2'><strong>"
            << html_escape (format_money (document.tax_total,
                                          document.currency))
            << "</strong></td></tr>\n";
    }

    if (paid)
        out << "<tr valign='top'><td align='left' class='subtotal' "
               "colspan='3'><strong>Total Price</strong></td>"
               "<td align='right' class='subtotal' colspan='2'><strong>"
            << html_escape (format_money (document.total, document.currency))
            << "</strong></td></tr>\n";

    for (const DocumentPayment& payment : document.payments)
    {
        out << "<tr valign='top'><td align='center'>"
            << html_escape (format_date (payment.date))
            << "</td><td align='left' colspan='2'>Payment received, "
               "thank you!</td><td align='right' colspan='2'>"
            << html_escape (format_money (payment.amount, document.currency))
            << "</td></tr>\n";
    }

    total_row (out, "Amount Due",
               format_money (document.amount_due, document.currency), "total",
               false, true);
    out << "</tbody></table></div>\n";

    if (!document.notes.empty ())
        out << "<p>" << multiline_html (document.notes) << "</p>\n";
    else
        out << "<p>\n";
    out << "<p>\n</div>\n</body></html>\n";
    return out.str ();
}

std::string
render_receipt_not_selected ()
{
    std::ostringstream out;
    out << "<!DOCTYPE html>\n<html dir='auto'><head>"
           "<meta http-equiv='Content-Type' content='text/html;charset=utf-8'>"
           "<title>Receipt</title><style>"
        << stylesheet ()
        << "</style></head><body><h2>Receipt</h2>\n"
           "<p>No invoice has been selected -- please use the Options menu to "
           "select one.</p>\n</body></html>\n";
    return out.str ();
}

std::string
render_receipt_not_customer ()
{
    std::ostringstream out;
    out << "<!DOCTYPE html>\n<html dir='auto'><head>"
           "<meta http-equiv='Content-Type' content='text/html;charset=utf-8'>"
           "<title>Receipt</title><style>"
        << stylesheet ()
        << "</style></head><body><h2>Receipt</h2>\n"
           "<p>This report is designed for customer (sales) invoices only. "
           "Please use the Options menu to select an <em>Invoice</em>, not a "
           "Bill or Expense Voucher.</p>\n</body></html>\n";
    return out.str ();
}

} // namespace report
} // namespace gnc
