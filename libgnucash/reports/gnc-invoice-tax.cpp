/********************************************************************\
 * gnc-invoice-tax.cpp -- the Tax Invoice document                    *
 *                                                                  *
 * The third consumer of the shared invoice model, and a sibling of  *
 * gnc-invoice-print.cpp: it renders the Scheme taxinvoice.scm       *
 * eguile layout from the same InvoiceDocument, with no engine read  *
 * of its own.  The Australian Tax Invoice is not a separate Scheme  *
 * renderer -- taxinvoice.scm registers it with the same renderer    *
 * and a different default option set -- so it is the same function  *
 * with `australian` true.                                           *
 *                                                                  *
 * The markup keeps the Scheme document's class names (coytable,     *
 * coyname, invoice, invnum, entries, subtotal, total) so the parity *
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
#include <vector>

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

/* The Scheme nl->br used by the template. */
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

/* The Scheme nbsp, which is a nowrap span around text with the spaces
 * converted to non-breaking ones. */
std::string
nbsp_text (const std::string& text)
{
    std::string out;
    for (const char c : text)
        out += c == ' ' ? "&nbsp;" : html_escape (std::string (1, c));
    return "<span style='white-space:nowrap;'>" + out + "</span>";
}

std::string
stylesheet (bool australian, bool border_collapse)
{
    std::string css =
        "div.main{margin:2em}"
        "table.entries{border:none;border-spacing:0.1em;empty-cells:show}"
        "table.entries th{background:#ccc;font-weight:bold;"
        "padding:0.2em 0.4em}"
        "table.entries td{padding:0.2em 0.4em}"
        "td.invnum{font-size:120%;font-weight:bold}"
        "td.subtotal{font-weight:bold}"
        "td.total{font-size:110%;font-weight:bold}"
        "table.coytable td,table.coytable tr{vertical-align:top}"
        "img.logo{margin-right:0.5em}";
    if (australian)
        css +=
            "h1.coyname{margin-top:0;padding-top:0;text-align:right;"
            "margin-bottom:0;font-size:200%}"
            "h2.invoice{padding-top:0;margin-top:0;text-align:left;"
            "margin-bottom:0;font-size:500%}";
    else
        css +=
            "h1.coyname{margin-top:0;padding-top:0;text-align:left}"
            "h2.invoice{padding-top:0;margin-top:0}";
    if (border_collapse)
        css += "table{border-collapse:collapse}";
    return css;
}

bool
customer_invoice (DocumentKind kind)
{
    return kind == DocumentKind::CustomerInvoice;
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

bool
has_units (const InvoiceDocument& document)
{
    for (const DocumentLine& line : document.entries)
    {
        if (!line.action.empty ())
            return true;
    }
    return false;
}

bool
has_quantity (const InvoiceDocument& document)
{
    for (const DocumentLine& line : document.entries)
    {
        if (!gnc_numeric_equal (line.quantity, gnc_numeric_create (1, 1)))
            return true;
    }
    return false;
}

bool
has_discount (const InvoiceDocument& document, bool customer)
{
    if (!customer)
        return false;
    for (const DocumentLine& line : document.entries)
    {
        if (!gnc_numeric_zero_p (line.discount))
            return true;
    }
    return false;
}

std::string
tax_rate_text (const DocumentLine& line, gnc_commodity* currency)
{
    if (!line.taxable || line.tax_rates.empty ())
        return "n/a";
    std::string out;
    for (const TaxRateEntry& rate : line.tax_rates)
    {
        if (!out.empty ())
            out += "&nbsp;+&nbsp;";
        if (rate.percent)
            out += html_escape (format_quantity (rate.amount) + "%");
        else
            out += html_escape (format_money (rate.amount, currency));
    }
    return out;
}

std::string
discount_rate_text (const DocumentLine& line, gnc_commodity* currency)
{
    if (line.discount_type == DiscountType::Percent)
        return html_escape (format_quantity (line.discount) + "%");
    return html_escape (format_money (line.discount, currency));
}

std::string
line_total (const DocumentLine& line, gnc_commodity* currency)
{
    const gnc_numeric total = gnc_numeric_add (line.amount, line.tax,
                                               GNC_DENOM_AUTO,
                                               GNC_HOW_RND_ROUND);
    return format_money (total, currency);
}

struct Layout
{
    bool date = false;
    bool description = true;
    bool units = false;
    bool quantity = false;
    bool price = false;
    bool discount = false;
    bool net_price = false;
    bool tax_rate = false;
    bool tax_amount = false;
    bool total = true;
    int columns = 0;     /* every header cell */
    int scheme_cols = 0; /* the Scheme tbl_cols: optional cells only */
};

Layout
layout_for (const InvoiceDocument& document, const TaxInvoiceOptions& options,
            bool customer)
{
    Layout layout;
    const bool tax = has_tax (document);
    const bool discount = has_discount (document, customer);
    const bool units = has_units (document);
    const bool quantity = has_quantity (document);
    layout.date = options.show_date;
    layout.units = units && options.show_units;
    layout.quantity = units || quantity;
    layout.price = units || quantity || discount;
    layout.discount = discount;
    layout.net_price = tax;
    layout.tax_rate = tax && options.show_tax_rate;
    layout.tax_amount = tax;
    layout.columns = (layout.date ? 1 : 0) + 1 + (layout.units ? 1 : 0) +
                     (layout.quantity ? 1 : 0) + (layout.price ? 1 : 0) +
                     (layout.discount ? 2 : 0) +
                     (layout.net_price ? 1 : 0) +
                     (layout.tax_rate ? 1 : 0) +
                     (layout.tax_amount ? 1 : 0) + 1;
    layout.scheme_cols = (layout.date ? 1 : 0) + (layout.units ? 1 : 0) +
                         (layout.quantity ? 1 : 0) + (layout.price ? 1 : 0) +
                         (layout.discount ? 2 : 0) +
                         (layout.net_price ? 1 : 0) +
                         (layout.tax_rate ? 1 : 0) +
                         (layout.tax_amount ? 1 : 0);
    return layout;
}

void
render_company (std::ostringstream& out, const InvoiceDocument& document,
                const TaxInvoiceOptions& options, const std::string& title)
{
    out << "<table class='coytable' border='0' width='100%'>"
           "<tr valign='top' style='vertical-align: top'>"
           "<td align='left'><h1 class='coyname'>"
        << html_escape (document.company.name.empty () ? "Company Name"
                                                        : document.company.name)
        << "</h1></td><td align='right'><h2 class='invoice'>"
        << html_escape (title);
    if (options.invoice_number_next_to_title)
        out << "&nbsp;" << html_escape (document.id);
    out << "</h2></td></tr></table>\n";

    out << "<table border='0' width='100%'><tr valign='top'>"
           "<td align='left'>";
    if (options.show_address && !document.company.address.empty ())
        out << multiline_html (document.company.address.multiline ())
            << "<br>";
    if (!document.company.id.empty ())
        out << "<strong>" << html_escape (document.company.id)
            << "</strong><br>";
    out << "</td><td align='right'><table border='0'>";
    if (options.show_contact && !document.company.contact.empty ())
        out << "<tr><th colspan='2' align='right'>"
            << html_escape (document.company.contact) << "</th></tr>";
    const std::pair<const char*, const std::string*> rows[] = {
        {"Phone", &document.company.address.phone},
        {"Fax", &document.company.address.fax},
        {"Email", &document.company.address.email},
        {"Website", &document.company.url},
    };
    for (const auto& row : rows)
    {
        if (row.second->empty ())
            continue;
        out << "<tr><td align='right'>" << row.first
            << ":&nbsp;</td><td align='right'>"
            << html_escape (*row.second) << "</td></tr>";
    }
    out << "</table></td></tr></table>\n<hr>\n";
}

void
detail_row (std::ostringstream& out, const std::string& label,
            const std::string& value)
{
    out << "<tr><td align='right'>" << label
        << ":&nbsp;</td><td align='right'>" << nbsp_text (value)
        << "</td></tr>";
}

void
render_parties (std::ostringstream& out, const InvoiceDocument& document,
                const TaxInvoiceOptions& options, bool australian)
{
    const std::string ref_text = australian ? "Reference:" : "Your ref:";
    const std::string job_name_text = australian ? "Engagement:" : "Job name:";
    out << "<table border='0' width='100%'><tr valign='top'>"
           "<th align='right' width='1%'>To:</th><td align='left'>";
    if (options.show_company_name && !document.owner.name.empty ())
        out << html_escape (document.owner.name) << "<br>";
    const std::string address = document.owner.address.multiline ();
    if (!address.empty ())
        out << multiline_html (address);
    out << "</td><td align='right'><table border='0'>";

    if (options.show_invoice_number)
    {
        const std::string label =
            australian ? "Invoice #:" : "Invoice number:";
        out << "<tr><td align='right' class='invnum'><big><strong>"
            << nbsp_text (label)
            << "</strong></big></td><td align='right' class='invnum'>"
               "<big><strong>"
            << html_escape (document.id)
            << "</strong></big></td></tr>";
    }
    if (!document.posted)
        out << "<tr><td colspan='2' align='right'>Invoice in progress&#8230;"
               "</td></tr>";
    else
    {
        detail_row (out, "Invoice Date", format_date (document.date_posted));
        detail_row (out, "Due Date", format_date (document.date_due));
    }
    if (!document.billing_id.empty ())
        out << "<tr><td align='right'>" << ref_text
            << "</td><td align='right'>" << html_escape (document.billing_id)
            << "</td></tr>";
    if (options.show_job_name && !document.job_name.empty ())
        out << "<tr><td align='right'>" << job_name_text
            << "</td><td align='right'>" << html_escape (document.job_name)
            << "</td></tr>";
    if (options.show_job_number && !document.job_number.empty ())
        out << "<tr><td align='right'>Job number:</td><td align='right'>"
            << html_escape (document.job_number) << "</td></tr>";
    if (!document.terms.empty ())
        out << "<tr><td colspan='2' align='right'>"
            << html_escape (document.terms) << "</td></tr>";
    out << "</table></td></tr></table>\n";
}

void
header_cells (std::ostringstream& out, const Layout& layout,
              bool australian)
{
    out << "<thead><tr bgcolor='#ccc' valign='bottom'>";
    if (layout.date)
        out << "<th align='center'>Date</th>";
    out << "<th align='left' width='80%'>Description</th>";
    if (layout.units)
        out << "<th align='left'>Units</th>";
    if (layout.quantity)
        out << "<th align='right'>Qty</th>";
    if (layout.price)
        out << "<th align='right'>"
            << (australian ? "Unit" : "Unit Price") << "</th>";
    if (layout.discount)
        out << "<th align='right'>Discount Rate</th>"
               "<th align='right'>Discount Amount</th>";
    if (layout.net_price)
    {
        out << "<th align='right'>Net Price</th>";
        if (layout.tax_rate)
            out << "<th align='right'>"
                << (australian ? "GST Rate" : "Tax Rate") << "</th>";
        out << "<th align='right'>"
            << (australian ? "GST Amount" : "Tax Amount") << "</th>";
    }
    out << "<th align='right'>Total Price</th></tr></thead><tbody>\n";
}

void
render_lines (std::ostringstream& out, const InvoiceDocument& document,
              const TaxInvoiceOptions& options, bool australian)
{
    const bool customer = customer_invoice (document.kind);
    const Layout layout = layout_for (document, options, customer);
    const bool tax = layout.net_price;
    const bool discount = layout.discount;
    const bool paid = !document.payments.empty ();

    out << "<table border='1' width='100%' cellpadding='4' class='entries'>\n";
    header_cells (out, layout, australian);

    gnc_numeric discount_total = gnc_numeric_zero ();
    for (const DocumentLine& line : document.entries)
    {
        out << "<tr valign='top'>";
        if (layout.date)
            out << "<td align='center'>" << nbsp_text (format_date (line.date))
                << "</td>";
        out << "<td align='left'>" << html_escape (line.description) << "</td>";
        if (layout.units)
            out << "<td align='left'>" << html_escape (line.action) << "</td>";
        if (layout.quantity)
            out << "<td align='right'>"
                << html_escape (format_quantity (line.quantity)) << "</td>";
        if (layout.price)
            out << "<td align='right'>"
                << nbsp_text (format_price (options.show_net_price
                                                ? line.net_unit_price
                                                : line.unit_price,
                                            document.currency))
                << "</td>";
        if (layout.discount)
        {
            out << "<td align='right'>"
                << discount_rate_text (line, document.currency)
                << "</td><td align='right'>"
                << html_escape (format_money (line.discount_value,
                                              document.currency))
                << "</td>";
            discount_total = gnc_numeric_add (
                discount_total, line.discount_value, GNC_DENOM_AUTO,
                GNC_HOW_DENOM_LCD);
        }
        if (layout.net_price)
        {
            out << "<td align='right'>"
                << html_escape (format_money (line.amount, document.currency))
                << "</td>";
            if (layout.tax_rate)
                out << "<td align='right'>"
                    << tax_rate_text (line, document.currency) << "</td>";
            out << "<td align='right'>"
                << html_escape (format_money (line.tax, document.currency))
                << "</td>";
        }
        out << "<td align='right'>" << html_escape (line_total (line,
                                                               document.currency))
            << "</td></tr>\n";
    }

    if (tax || discount || paid)
    {
        const int value_cells = (discount ? 1 : 0) + (tax ? 1 : 0) +
                                (layout.tax_rate ? 1 : 0) + (tax ? 1 : 0) + 1;
        const int label_span = layout.columns - value_cells;
        out << "<tr valign='top'><td align='left' class='subtotal' colspan='"
            << (label_span > 0 ? label_span : 1)
            << "'><strong>Sub-total</strong></td>";
        if (discount)
            out << "<td align='right' class='subtotal'><strong>"
                << html_escape (format_money (discount_total,
                                              document.currency))
                << "</strong></td>";
        if (tax)
        {
            out << "<td align='right' class='subtotal'><strong>"
                << html_escape (format_money (document.subtotal,
                                              document.currency))
                << "</strong></td>";
            if (layout.tax_rate)
                out << "<td>&nbsp;</td>";
            out << "<td align='right' class='subtotal'><strong>"
                << html_escape (format_money (document.tax_total,
                                              document.currency))
                << "</strong></td>";
        }
        out << "<td align='right' class='subtotal'><strong>"
            << html_escape (format_money (document.total, document.currency))
            << "</strong></td></tr>\n";
    }

    for (const DocumentPayment& payment : document.payments)
    {
        out << "<tr valign='top'>";
        if (layout.date)
            out << "<td align='center'>" << html_escape (format_date (
                   payment.date))
                << "</td>";
        out << "<td align='left' colspan='"
            << (layout.scheme_cols + (layout.date ? 0 : 1))
            << "'>Payment received, thank you!</td>"
               "<td align='right'>"
            << html_escape (format_money (payment.amount, document.currency))
            << "</td></tr>\n";
    }

    const std::string due_label =
        australian ? "Amount Due (inc GST)" : "Amount Due";
    out << "<tr valign='top'><td align='left' class='total' colspan='"
        << (layout.scheme_cols + 1) << "'><strong>" << due_label;
    if (!document.currency_code.empty ())
        out << ", " << html_escape (document.currency_code);
    out << "</strong></td><td align='right' class='total'><strong>"
        << html_escape (format_money (document.amount_due,
                                      document.currency))
        << "</strong></td></tr>\n</tbody></table>\n";
}

} // namespace

bool
set_tax_invoice_option (TaxInvoiceOptions& options, const std::string& name,
                        bool value)
{
    if (name == "show_date")                    options.show_date = value;
    else if (name == "show_tax_rate")           options.show_tax_rate = value;
    else if (name == "show_units")              options.show_units = value;
    else if (name == "show_contact")            options.show_contact = value;
    else if (name == "show_address")            options.show_address = value;
    else if (name == "show_invoice_number")     options.show_invoice_number = value;
    else if (name == "show_company_name")       options.show_company_name = value;
    else if (name == "invoice_number_next_to_title")
        options.invoice_number_next_to_title = value;
    else if (name == "show_job_name")           options.show_job_name = value;
    else if (name == "show_job_number")         options.show_job_number = value;
    else if (name == "show_net_price")          options.show_net_price = value;
    else if (name == "border_collapse")         options.border_collapse = value;
    else
        return false;
    return true;
}

std::string
render_tax_invoice (const InvoiceDocument& document,
                    const TaxInvoiceOptions& options, bool australian)
{
    const std::string default_title = australian ? "Tax Invoice" : "Invoice";
    const std::string title = options.custom_title.empty ()
                                  ? default_title
                                  : options.custom_title;
    std::ostringstream out;
    out << "<!DOCTYPE html>\n<html dir='auto'><head>"
           "<meta http-equiv='Content-Type' content='text/html;charset=utf-8'>"
           "<title>Invoice "
        << html_escape (document.id) << "</title><style>"
        << stylesheet (australian, options.border_collapse)
        << "</style></head><body>\n<div class='main'>\n";

    render_company (out, document, options, title);
    render_parties (out, document, options, australian);
    out << "<p>\n";
    render_lines (out, document, options, australian);

    if (!document.notes.empty ())
        out << "<p>" << multiline_html (document.notes) << "\n";
    else
        out << "<p>\n";
    out << "<p>Thank you for your patronage!\n</div>\n</body></html>\n";
    return out.str ();
}

std::string
render_tax_invoice_not_selected ()
{
    std::ostringstream out;
    out << "<!DOCTYPE html>\n<html dir='auto'><head>"
           "<meta http-equiv='Content-Type' content='text/html;charset=utf-8'>"
           "<title>Tax Invoice</title><style>"
        << stylesheet (false, false)
        << "</style></head><body><h2>Tax Invoice</h2>\n"
           "<p>No invoice has been selected -- please use the Options menu to "
           "select one.</p>\n</body></html>\n";
    return out.str ();
}

} // namespace report
} // namespace gnc
