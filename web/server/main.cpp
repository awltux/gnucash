/********************************************************************\
 * gnucash-webd -- a local web server that owns the engine          *
 *                                                                  *
 * The engine is single-threaded (no locking model), so everything  *
 * happens on one thread: the engine initialises here, the book is  *
 * opened here, and every request is handled on the GMainLoop that  *
 * this file runs. No worker threads, no marshalling.               *
 *                                                                  *
 * This program is free software; you can redistribute it and/or    *
 * modify it under the terms of the GNU General Public License as   *
 * published by the Free Software Foundation; either version 2 of   *
 * the License, or (at your option) any later version.              *
\********************************************************************/

#include <config.h>

#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <set>
#include <string>
#include <vector>

#include <gio/gio.h>
#include <glib.h>
#ifdef GNC_WITH_REPORT
#include <libguile.h>
#endif

extern "C" {
#include <cashobjects.h>
#include <gnc-engine.h>
#ifdef GNC_WITH_REPORT
#include <gnc-report.h>
#endif
#include <gnc-prefs-utils.h>
#include <gnc-session.h>
#include <gnc-ui-balances.h>
#include <gnc-ui-util.h>
#include <qof.h>
#include <qofbook.h>
#include <qofsession.h>
#include <Account.h>
#include <Split.h>
#include <Transaction.h>
#include <gnc-commodity.h>
#include <gnc-numeric.h>
#include <gnc-date.h>
#include <guid.h>
}

#include <gnc-report.hpp>
#include <gnc-owner.hpp>

#include "json.hpp"
#include "report-spec.hpp"

namespace
{

/* ------------------------------------------------------------------ state */

struct Server
{
    std::string token;
    Account* root = nullptr;
    int top_level = 0;
    int accounts = 0;
};

Server g_server;

/* ------------------------------------------------------------------- json */

std::string
json_escape (const char* text)
{
    std::string out;
    if (text == nullptr)
        return out;

    for (const char* p = text; *p; ++p)
    {
        switch (*p)
        {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(*p) < 0x20)
            {
                char esc[8];
                g_snprintf (esc, sizeof esc, "\\u%04x", *p);
                out += esc;
            }
            else
                out += *p;
        }
    }
    return out;
}

std::string
account_guid_text (Account* account)
{
    gchar* text = guid_to_string (xaccAccountGetGUID (account));
    std::string result = text != nullptr ? text : "";
    g_free (text);
    return result;
}

std::string
account_json (Account* account)
{
    std::string out = "{\"name\":\"";
    out += json_escape (xaccAccountGetName (account));
    out += "\",\"guid\":\"";
    out += json_escape (account_guid_text (account).c_str ());
    out += "\",\"total\":\"";

    gboolean negative = FALSE;
    gchar* text = gnc_ui_account_get_print_balance (
        xaccAccountGetPresentBalanceInCurrency, account, TRUE, &negative);
    out += json_escape (text != nullptr ? text : "");
    g_free (text);

    out += "\",\"negative\":";
    out += negative ? "true" : "false";
    out += ",\"children\":[";

    GList* children = gnc_account_get_children (account);
    for (GList* node = children; node != nullptr; node = node->next)
    {
        if (node != children)
            out += ",";
        out += account_json (static_cast<Account*>(node->data));
    }
    g_list_free (children);

    out += "]}";
    return out;
}

/* ------------------------------------------------------------------- http */

void
send_response (GSocketConnection* connection, const char* status,
               const char* content_type, const std::string& body)
{
    std::string response = "HTTP/1.1 ";
    response += status;
    response += "\r\nContent-Type: ";
    response += content_type;
    response += "\r\nContent-Length: ";
    response += std::to_string (body.size());
    response += "\r\nConnection: close\r\n";
    response += "Cache-Control: no-store\r\n";
    response += "X-Content-Type-Options: nosniff\r\n\r\n";
    response += body;

    GOutputStream* out = g_io_stream_get_output_stream (G_IO_STREAM (connection));
    gsize written = 0;
    g_output_stream_write_all (out, response.data(), response.size(), &written,
                               nullptr, nullptr);
}

std::string
url_decode (const std::string& in)
{
    std::string out;
    for (size_t i = 0; i < in.size(); ++i)
    {
        if (in[i] == '%' && i + 2 < in.size())
        {
            const int high = g_ascii_xdigit_value (in[i + 1]);
            const int low = g_ascii_xdigit_value (in[i + 2]);
            if (high >= 0 && low >= 0)
            {
                out += static_cast<char> (high * 16 + low);
                i += 2;
                continue;
            }
        }
        out += in[i] == '+' ? ' ' : in[i];
    }
    return out;
}

#ifdef GNC_WITH_REPORT
std::string
reports_json ()
{
    /* The report system knows its own templates; ask it rather than keeping a
     * list here that would drift. */
    SCM names = scm_c_eval_string (
        "(let ((names '()))"
        "  (gnc:report-templates-for-each"
        "    (lambda (id tmpl)"
        "      (set! names (cons (gnc:report-template-name tmpl) names))))"
        "  names)");

    std::string body = "[";
    bool first = true;
    for (SCM node = names; scm_is_pair (node); node = scm_cdr (node))
    {
        char* name = scm_to_utf8_string (scm_car (node));
        if (!first)
            body += ",";
        first = false;
        body += "\"";
        body += json_escape (name);
        body += "\"";
        free (name);
    }
    body += "]";
    return body;
}
#else
std::string
native_reports_json ()
{
    std::string body = "[";
    bool first = true;
    for (const std::string& name : gnc::report::native_report_names ())
    {
        if (!first)
            body += ",";
        first = false;
        body += "\"";
        body += json_escape (name.c_str ());
        body += "\"";
    }
    body += "]";
    return body;
}

/* This build has no Guile and therefore no Scheme report engine.  Native
 * reports still exist, so api/reports answers from their registry; the
 * report routes themselves answer 501 for names the native layer does not
 * know.  A 501 says the feature is absent, where a generic 404 would read
 * as a bad report name. */
void
reports_unavailable (GSocketConnection* connection)
{
    send_response (connection, "501 Not Implemented", "text/plain",
                   "reports are not available in this build: "
                   "no Guile/report engine\n");
}
#endif

/* The built-in designer specs are reports in every build: without Guile they
 * are what the report list offers beyond the native fourteen, and with Guile
 * they sit beside the Scheme templates.  The plain /api/reports list stays
 * an array of names. */
std::string
with_builtin_report_names (std::string body)
{
    if (!body.empty () && body.back () == ']')
        body.pop_back ();
    bool first = body == "[";
    for (const std::string& name : web::builtin_spec_names ())
    {
        if (!first)
            body += ",";
        first = false;
        body += "\"";
        body += json_escape (name.c_str ());
        body += "\"";
    }
    body += "]";
    return body;
}

/* ---------------------------------------------------------- report options */

const char*
option_type_name (gnc::report::OptionType type)
{
    switch (type)
    {
    case gnc::report::OptionType::Date:        return "date";
    case gnc::report::OptionType::AccountList: return "account-list";
    case gnc::report::OptionType::Currency:    return "currency";
    case gnc::report::OptionType::Choice:      return "choice";
    case gnc::report::OptionType::Text:        return "text";
    case gnc::report::OptionType::Bool:        return "bool";
    case gnc::report::OptionType::Invoice:     return "invoice";
    case gnc::report::OptionType::Owner:       return "owner";
    }
    return "";
}

std::string
option_json (const gnc::report::OptionDescriptor& option)
{
    std::string out = "{\"name\":\"";
    out += json_escape (option.name.c_str ());
    out += "\",\"type\":\"";
    out += option_type_name (option.type);
    out += "\",\"label\":\"";
    out += json_escape (option.label.c_str ());
    out += "\",\"description\":\"";
    out += json_escape (option.description.c_str ());
    out += "\",\"default\":";
    /* An account selection defaults to the empty list, a boolean to a JSON
     * boolean, everything else to a string; keep the JSON types honest so a
     * form can use them as is. */
    if (option.type == gnc::report::OptionType::AccountList)
        out += "[]";
    else if (option.type == gnc::report::OptionType::Bool)
        out += option.default_value == "true" ? "true" : "false";
    else
    {
        out += "\"";
        out += json_escape (option.default_value.c_str ());
        out += "\"";
    }
    if (!option.choices.empty ())
    {
        out += ",\"choices\":[";
        bool first = true;
        for (const std::string& choice : option.choices)
        {
            if (!first)
                out += ",";
            first = false;
            out += "\"";
            out += json_escape (choice.c_str ());
            out += "\"";
        }
        out += "]";
    }
    out += "}";
    return out;
}

std::string
options_json (const std::vector<gnc::report::OptionDescriptor>& options)
{
    std::string body = "[";
    bool first = true;
    for (const auto& option : options)
    {
        if (!first)
            body += ",";
        first = false;
        body += option_json (option);
    }
    body += "]";
    return body;
}

std::string
report_options_json (const std::string& name)
{
    std::string body = "{\"report\":\"";
    body += json_escape (name.c_str ());
    body += "\",\"options\":";
    body += options_json (gnc::report::report_options (name));
    body += "}";
    return body;
}

/* /api/reports?options=1: the native reports, each with the options it
 * accepts.  The plain /api/reports list keeps its shape (an array of names)
 * because the UI and the smoke test already depend on it. */
std::string
native_reports_options_json ()
{
    std::string body = "[";
    bool first = true;
    for (const std::string& name : gnc::report::native_report_names ())
    {
        if (!first)
            body += ",";
        first = false;
        body += report_options_json (name);
    }
    body += "]";
    return body;
}

/* /api/spec/options: the vocabulary the designer form is generated from.
 * The columns, groupings and date/currency options are the native layer's,
 * not a list in JavaScript, so a new column or preset appears in the form
 * without a UI change. */
std::string
spec_options_json ()
{
    std::string body = "{\"columns\":[";
    bool first = true;
    for (const gnc::report::Column column : gnc::report::column_vocabulary ())
    {
        if (!first)
            body += ",";
        first = false;
        body += "{\"name\":\"";
        body += json_escape (gnc::report::column_name (column));
        body += "\",\"label\":\"";
        body += json_escape (gnc::report::column_heading (column));
        body += "\"}";
    }
    body += "],\"groupings\":["
            "{\"name\":\"none\",\"label\":\"Flat account list\"},"
            "{\"name\":\"account-tree\",\"label\":\"By account tree\"},"
            "{\"name\":\"period\",\"label\":\"By period\"}],"
            "\"period_units\":["
            "{\"name\":\"year\",\"label\":\"Year\"},"
            "{\"name\":\"month\",\"label\":\"Month\"}],"
            "\"period_modes\":["
            "{\"name\":\"activity\",\"label\":\"Period activity\"},"
            "{\"name\":\"cumulative\",\"label\":\"Balance at period end\"}],"
            "\"charts\":["
            "{\"name\":\"\",\"label\":\"No chart\"},"
            "{\"name\":\"bar\",\"label\":\"Bar chart\"},"
            "{\"name\":\"line\",\"label\":\"Line chart\"}],"
            "\"chart_series\":["
            "{\"name\":\"debit\",\"label\":\"Debit\"},"
            "{\"name\":\"credit\",\"label\":\"Credit\"},"
            "{\"name\":\"balance\",\"label\":\"Balance\"}],"
            "\"account_types\":[";

    first = true;
    for (const std::string& name :
         gnc::report::account_type_vocabulary ())
    {
        if (!first)
            body += ",";
        first = false;
        body += "{\"name\":\"";
        body += json_escape (name.c_str ());
        body += "\",\"label\":\"";
        body += json_escape (name.c_str ());
        body += "\"}";
    }
    body += "],\"options\":[";

    std::vector<gnc::report::OptionDescriptor> options;
    for (const auto& option :
         gnc::report::report_options ("Income Statement"))
    {
        if (option.name == "start" || option.name == "end")
            options.push_back (option);
    }
    gnc::report::OptionDescriptor preset;
    preset.name = "date_preset";
    preset.type = gnc::report::OptionType::Choice;
    preset.label = "Date range";
    preset.description =
        "Use the dates above, or resolve them from the accounting period; "
        "the two are alternatives, as on the report route.";
    preset.default_value = "";
    preset.choices = {"this-year", "last-year", "all"};
    options.push_back (preset);
    for (const auto& option :
         gnc::report::report_options ("Income Statement"))
    {
        if (option.name == "currency")
            options.push_back (option);
    }

    first = true;
    for (const auto& option : options)
    {
        if (!first)
            body += ",";
        first = false;
        body += option_json (option);
    }
    body += "]}";
    return body;
}

/* Find a saved spec by id.  The store is re-read per request: it is a small
 * file, and a save from another tab must be visible without a restart. */
bool
find_spec (const std::string& id, web::ReportSpec& spec, std::string& error)
{
    std::vector<web::ReportSpec> specs;
    if (!web::load_specs (specs, error))
        return false;
    for (const web::ReportSpec& candidate : specs)
    {
        if (candidate.id == id)
        {
            spec = candidate;
            return true;
        }
    }
    error = "no saved report with id '" + id + "'";
    return false;
}

std::string
register_json (Account* account)
{
    GNCPrintAmountInfo info = gnc_account_print_info (account, TRUE);
    std::string out = "{\"account\":\"";
    out += json_escape (xaccAccountGetName (account));
    out += "\",\"splits\":[";

    const bool newest_first = true;
    GList* splits = xaccAccountGetSplitList (account);
    if (newest_first)
        splits = g_list_reverse (g_list_copy (splits));
    else
        splits = g_list_copy (splits);

    bool first = true;
    for (GList* node = splits; node != nullptr; node = node->next)
    {
        auto* split = static_cast<Split*>(node->data);
        if (!first)
            out += ",";
        first = false;

        char date[16] = {0};
        qof_print_date_buff (date, sizeof date, xaccTransGetDate (xaccSplitGetParent (split)));

        out += "{\"date\":\"";
        out += date;
        out += "\",\"description\":\"";
        out += json_escape (xaccTransGetDescription (xaccSplitGetParent (split)));
        out += "\",\"account\":\"";
        out += json_escape (xaccAccountGetName (xaccSplitGetAccount (split)));
        out += "\",\"amount\":\"";
        const gchar* amount = xaccPrintAmount (xaccSplitGetAmount (split), info);
        out += json_escape (amount != nullptr ? amount : "");
        out += "\",\"reconciled\":";
        out += xaccSplitGetReconcile (split) == YREC ? "true" : "false";
        out += "}";
    }
    g_list_free (splits);


    out += "]}";
    return out;
}

std::string
html_escape (const char* text)
{
    std::string out;
    if (text == nullptr)
        return out;
    for (const char* p = text; *p; ++p)
    {
        switch (*p)
        {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        default: out += *p;
        }
    }
    return out;
}

/* A report links to its accounts with gnc-register:acct-guid=... , which is a
 * GnuCash URL type no browser can follow. Point those at this server instead,
 * and report-to-report links at the report route.  Native reports use the
 * same URL type, so this applies to both engines. */
std::string
rewrite_links (const std::string& html, const std::string& token)
{
    std::string out = html;
    const std::string register_prefix = "gnc-register:";
    const std::string register_route = "/" + token + "/register/";
    const std::string report_prefix = "gnc-report:";
    const std::string report_route = "/" + token + "/report-id/";
    /* The native renderer cannot know the server's token, so it writes the
     * vendored assets with this placeholder and the server fills the token
     * in.  It must not collide with a real URL type (gnc-register:, ...). */
    const std::string webui_prefix = "gnc-webui:";
    const std::string webui_route = "/" + token + "/";

    for (size_t pos = out.find (webui_prefix); pos != std::string::npos;
         pos = out.find (webui_prefix, pos))
    {
        out.replace (pos, webui_prefix.size (), webui_route);
        pos += webui_route.size ();
    }
    for (size_t pos = out.find (register_prefix); pos != std::string::npos;
         pos = out.find (register_prefix, pos))
    {
        out.replace (pos, register_prefix.size (), register_route);
        pos += register_route.size ();
    }
    for (size_t pos = out.find (report_prefix); pos != std::string::npos;
         pos = out.find (report_prefix, pos))
    {
        out.replace (pos, report_prefix.size (), report_route);
        pos += report_route.size ();
    }
    return out;
}

/* A server-rendered register page: the frame the report navigates into. */
std::string
register_html (Account* account)
{
    GNCPrintAmountInfo info = gnc_account_print_info (account, TRUE);
    std::string out = "<!DOCTYPE html><html><head><meta charset='utf-8'><title>";
    out += html_escape (xaccAccountGetName (account));
    out += "</title><style>"
           "body{font-family:system-ui,sans-serif;margin:1rem;color:#222}"
           "h1{font-size:1.2rem;margin:0 0 .25rem}"
           "p.total{color:#555;margin:0 0 1rem}"
           "table{border-collapse:collapse;width:100%}"
           "th,td{padding:.3rem .6rem;border-bottom:1px solid #e0e0e0;text-align:left}"
           "th{font-weight:600;color:#444}"
           "td.num{text-align:right;font-variant-numeric:tabular-nums}"
           "tr.reconciled td.num{color:#080}"
           "</style></head><body><h1>";
    out += html_escape (xaccAccountGetName (account));
    out += "</h1><p class='total'>Balance: ";
    const gchar* balance = xaccPrintAmount (xaccAccountGetBalance (account), info);
    out += html_escape (balance != nullptr ? balance : "");

    out += "</p><table><thead><tr><th>Date</th><th>Description</th>"
           "<th>Transfer</th><th class='num'>Amount</th>"
           "<th class='num'>Balance</th></tr></thead><tbody>";

    GList* splits = g_list_reverse (g_list_copy (xaccAccountGetSplitList (account)));
    /* Newest first, walking the account's balance backwards: each row shows the
     * balance after that entry, which is what a register column means. */
    gnc_numeric running = xaccAccountGetBalance (account);
    for (GList* node = splits; node != nullptr; node = node->next)
    {
        auto* split = static_cast<Split*>(node->data);
        char date[16] = {0};
        qof_print_date_buff (date, sizeof date,
                             xaccTransGetDate (xaccSplitGetParent (split)));

        out += "<tr";
        out += xaccSplitGetReconcile (split) == YREC ? " class='reconciled'" : "";
        out += "><td>";
        out += date;
        out += "</td><td>";
        out += html_escape (xaccTransGetDescription (xaccSplitGetParent (split)));
        out += "</td><td>";
        out += html_escape (xaccAccountGetName (xaccSplitGetAccount (split)));
        out += "</td><td class='num'>";
        const gchar* amount = xaccPrintAmount (xaccSplitGetAmount (split), info);
        out += html_escape (amount != nullptr ? amount : "");
        out += "</td><td class='num'>";
        const gchar* running_text = xaccPrintAmount (running, info);
        out += html_escape (running_text != nullptr ? running_text : "");
        out += "</td></tr>";

        running = gnc_numeric_sub (running, xaccSplitGetAmount (split),
                                   GNC_DENOM_AUTO, GNC_HOW_RND_ROUND_HALF_UP);
    }
    g_list_free (splits);


    out += "</tbody></table></body></html>";
    return out;
}

/* One request, already read.  The designer's POST bodies are small JSON
 * documents, so the whole body is read here rather than streamed; anything
 * above the limit is refused by the route instead of being buffered. */
struct HttpRequest
{
    std::string method;
    std::string path;
    std::string body;
    bool body_too_large = false;
};

constexpr size_t max_request_body = 1024 * 1024;

/* Reads the request line, the headers and (for POST) the body. Returns an
 * empty path if the request is malformed. */
HttpRequest
read_request (GSocketConnection* connection)
{
    GInputStream* in = g_io_stream_get_input_stream (G_IO_STREAM (connection));
    std::string request;
    char buffer[1024];
    size_t header_end = std::string::npos;

    while (header_end == std::string::npos && request.size () < 32 * 1024)
    {
        gssize read = g_input_stream_read (in, buffer, sizeof buffer, nullptr,
                                           nullptr);
        if (read <= 0)
            break;
        request.append (buffer, static_cast<size_t>(read));
        header_end = request.find ("\r\n\r\n");
    }

    HttpRequest result;
    const size_t line_end = request.find ("\r\n");
    if (line_end == std::string::npos)
        return result;

    const std::string line = request.substr (0, line_end);
    /* "GET <path> HTTP/1.1" */
    const size_t first = line.find (' ');
    const size_t second = line.find (' ', first + 1);
    if (first == std::string::npos || second == std::string::npos)
        return result;
    result.method = line.substr (0, first);
    result.path = line.substr (first + 1, second - first - 1);

    if (header_end == std::string::npos)
        return result;

    size_t content_length = 0;
    size_t pos = line_end + 2;
    while (pos < header_end)
    {
        const size_t end = request.find ("\r\n", pos);
        if (end == std::string::npos || end > header_end)
            break;
        const std::string header = request.substr (pos, end - pos);
        const size_t colon = header.find (':');
        if (colon != std::string::npos)
        {
            const std::string name = header.substr (0, colon);
            if (g_ascii_strcasecmp (name.c_str (), "Content-Length") == 0)
            {
                const std::string value = header.substr (colon + 1);
                content_length = static_cast<size_t> (
                    g_ascii_strtoull (value.c_str (), nullptr, 10));
            }
        }
        pos = end + 2;
    }

    result.body = request.substr (header_end + 4);
    if (content_length > max_request_body)
    {
        result.body_too_large = true;
        result.body.clear ();
        return result;
    }
    while (result.body.size () < content_length)
    {
        gssize read = g_input_stream_read (in, buffer, sizeof buffer, nullptr,
                                           nullptr);
        if (read <= 0)
            break;
        result.body.append (buffer, static_cast<size_t>(read));
    }
    if (result.body.size () > content_length)
        result.body.resize (content_length);
    return result;
}

/* The report route's option surface.  The names and defaults come from the
 * native layer's descriptors (report_options()), so a form and a caller can
 * ask the same questions.  Everything a caller sends is validated here:
 * unknown options, bad dates and unknown GUIDs are rejected with a message
 * naming the problem, never ignored.
 *
 * "period" is a convenience over the report's own date options: it resolves
 * to the same start/end pair the report's date options default to
 * (this-year), the previous period (last-year), or no bounds at all (all).
 * It is still checked against the descriptors, because a report whose dates
 * are not a free range (aging, lots) must reject it. */

struct QueryParam
{
    std::string name;
    std::string value;
};

bool
parse_query (const std::string& query, std::vector<QueryParam>& out,
             std::string& error)
{
    size_t pos = 0;
    while (pos < query.size ())
    {
        const size_t next = query.find ('&', pos);
        const std::string pair = query.substr (
            pos, next == std::string::npos ? std::string::npos : next - pos);
        if (!pair.empty ())
        {
            const size_t equals = pair.find ('=');
            if (equals == std::string::npos)
            {
                error = "malformed query parameter '" + pair +
                        "' (expected name=value)";
                return false;
            }
            out.push_back ({url_decode (pair.substr (0, equals)),
                            url_decode (pair.substr (equals + 1))});
        }
        if (next == std::string::npos)
            break;
        pos = next + 1;
    }
    return true;
}

/* A date parameter is exactly YYYY-MM-DD, so a typo cannot silently mean a
 * different day. */
bool
parse_iso_date (const std::string& text, bool day_end, time64& out)
{
    if (text.size () != 10 || text[4] != '-' || text[7] != '-')
        return false;

    int digits[8] = {0};
    for (int index = 0, digit = 0; index < 10; ++index)
    {
        if (index == 4 || index == 7)
            continue;
        if (!g_ascii_isdigit (text[index]))
            return false;
        digits[digit++] = text[index] - '0';
    }

    const int year = digits[0] * 1000 + digits[1] * 100 + digits[2] * 10 +
                     digits[3];
    const int month = digits[4] * 10 + digits[5];
    const int day = digits[6] * 10 + digits[7];
    if (!g_date_valid_dmy (day, static_cast<GDateMonth> (month), year))
        return false;

    out = day_end ? gnc_dmy2time64_end (day, month, year)
                  : gnc_dmy2time64 (day, month, year);
    return true;
}

/* Resolve the ?accounts selection.  Each GUID means that account and its
 * whole subtree, which is what a caller naming "Expenses" means; "all" means
 * every account under the root account.  Accounts reachable twice are listed
 * once. */
bool
apply_accounts (bool all, const std::vector<std::string>& guids,
                std::vector<Account*>& accounts, std::string& error)
{
    if (g_server.root == nullptr)
    {
        error = "no book is open";
        return false;
    }
    QofBook* book = gnc_account_get_book (g_server.root);

    std::vector<Account*> selected;
    if (all)
    {
        GList* descendants = gnc_account_get_descendants_sorted (g_server.root);
        for (GList* node = descendants; node != nullptr; node = node->next)
            selected.push_back (static_cast<Account*> (node->data));
        g_list_free (descendants);
    }
    else
    {
        for (const std::string& text : guids)
        {
            GncGUID guid;
            if (!string_to_guid (text.c_str (), &guid))
            {
                error = "invalid account GUID '" + text + "'";
                return false;
            }
            Account* account = xaccAccountLookup (&guid, book);
            if (account == nullptr)
            {
                error = "unknown account GUID '" + text + "'";
                return false;
            }
            selected.push_back (account);
            GList* descendants = gnc_account_get_descendants_sorted (account);
            for (GList* node = descendants; node != nullptr; node = node->next)
                selected.push_back (static_cast<Account*> (node->data));
            g_list_free (descendants);
        }
    }

    std::set<std::string> seen;
    for (Account* account : selected)
    {
        if (seen.insert (account_guid_text (account)).second)
            accounts.push_back (account);
    }
    return true;
}

bool
build_report_request (const std::string& report, const std::string& query,
                      gnc::report::ReportRequest& request, std::string& error)
{
    std::vector<QueryParam> params;
    if (!parse_query (query, params, error))
        return false;

    const std::vector<gnc::report::OptionDescriptor> descriptors =
        gnc::report::report_options (report);
    if (descriptors.empty ())
    {
        error = "report '" + report +
                "' is not a native report and takes no options";
        return false;
    }
    if (params.empty ())
        return true;

    /* The descriptors, not a hard-coded list, decide what an option means.
     * A name the descriptors do not publish is rejected before any value is
     * looked at, with the message the existing tests rely on. */
    auto descriptor_for = [&descriptors] (const std::string& name)
        -> const gnc::report::OptionDescriptor*
    {
        for (const gnc::report::OptionDescriptor& descriptor : descriptors)
        {
            if (descriptor.name == name)
                return &descriptor;
        }
        return nullptr;
    };
    auto route_option = [] (const std::string& name)
    {
        return name == "start" || name == "end" || name == "accounts" ||
               name == "currency" || name == "period";
    };

    bool have_start = false;
    bool have_end = false;
    bool have_accounts = false;
    bool all_accounts = false;
    bool have_period = false;
    bool have_currency = false;
    bool have_invoice = false;
    std::string period;
    std::string currency;
    std::vector<std::string> guids;

    for (const QueryParam& param : params)
    {
        const gnc::report::OptionDescriptor* descriptor =
            descriptor_for (param.name);
        if (descriptor == nullptr)
        {
            /* Keep the engine-facing wording the existing tests pin: the
             * account selection is "an 'accounts' option", every other
             * route-level option is "a '<name>' option". */
            if (param.name == "accounts")
                error = "report '" + report +
                        "' does not accept an 'accounts' option";
            else if (route_option (param.name))
                error = "report '" + report + "' does not accept a '" +
                        param.name + "' option";
            else
                error = "unknown option '" + param.name + "' for report '" +
                        report + "'";
            return false;
        }

        switch (descriptor->type)
        {
        case gnc::report::OptionType::Date:
        {
            if (param.name != "start" && param.name != "end")
            {
                error = "report '" + report + "' has no date option '" +
                        param.name + "'";
                return false;
            }
            time64 when = 0;
            if (!parse_iso_date (param.value, param.name == "end", when))
            {
                error = "invalid " + param.name + " date '" + param.value +
                        "' (expected YYYY-MM-DD)";
                return false;
            }
            if (param.name == "start")
            {
                request.start = when;
                have_start = true;
            }
            else
            {
                request.end = when;
                have_end = true;
            }
            break;
        }

        case gnc::report::OptionType::AccountList:
        {
            have_accounts = true;
            if (param.name != "accounts")
            {
                /* A report may publish more than one account selection (the
                 * IFRS report has five); they are independent lists and the
                 * report reads the extra ones by name. */
                std::vector<Account*> selected;
                if (!apply_accounts (false, {param.value}, selected, error))
                    return false;
                request.account_lists[param.name] = std::move (selected);
                break;
            }
            if (param.value == "all")
            {
                all_accounts = true;
                break;
            }
            if (param.value.empty ())
            {
                error = "empty 'accounts' value (expected GUIDs or 'all')";
                return false;
            }
            size_t pos = 0;
            while (pos <= param.value.size ())
            {
                const size_t comma = param.value.find (',', pos);
                const std::string guid = param.value.substr (
                    pos, comma == std::string::npos ? std::string::npos
                                                    : comma - pos);
                if (guid.empty ())
                {
                    error = "empty account GUID in 'accounts=" +
                            param.value + "'";
                    return false;
                }
                guids.push_back (guid);
                if (comma == std::string::npos)
                    break;
                pos = comma + 1;
            }
            break;
        }

        case gnc::report::OptionType::Currency:
        {
            if (param.name != "currency")
            {
                error = "report '" + report + "' has no currency option '" +
                        param.name + "'";
                return false;
            }
            if (param.value.empty ())
            {
                error = "empty 'currency' value (expected a commodity "
                        "mnemonic)";
                return false;
            }
            have_currency = true;
            currency = param.value;
            break;
        }

        case gnc::report::OptionType::Choice:
        {
            const auto choice_ok = [&descriptor] (const std::string& value)
            {
                return std::find (descriptor->choices.begin (),
                                  descriptor->choices.end (), value) !=
                       descriptor->choices.end ();
            };
            if (param.name == "period")
            {
                /* Like every other option, the preset is only accepted when
                 * the report publishes it: ages and lots have fixed date
                 * semantics, so a preset they cannot honour is a 400, not a
                 * silently ignored convenience. */
                have_period = true;
                period = param.value;
                break;
            }
            if (param.name == "alternate-period")
            {
                if (report != "Tax Schedule Report/TXF Export")
                {
                    error = "report '" + report +
                            "' has no choice option 'alternate-period'";
                    return false;
                }
                if (!choice_ok (param.value))
                {
                    error = "invalid alternate-period '" + param.value + "'";
                    return false;
                }
                request.tax_alternate_period = param.value;
                break;
            }
            if (param.name == "format")
            {
                if (report != "Tax Schedule Report/TXF Export")
                {
                    error = "report '" + report +
                            "' has no choice option 'format'";
                    return false;
                }
                if (!choice_ok (param.value))
                {
                    error = "invalid format '" + param.value +
                            "' (expected report or txf)";
                    return false;
                }
                request.tax_format = param.value;
                break;
            }
            if (!choice_ok (param.value))
            {
                error = "invalid " + param.name + " '" + param.value + "'";
                return false;
            }
            /* A choice the report publishes and this route has no special
             * meaning for belongs to the report: it reads the value by the
             * descriptor's name. */
            request.choices[param.name] = param.value;
            break;
        }
        case gnc::report::OptionType::Text:
        {
            if (param.name == "title")
            {
                if (report == "Tax Invoice" ||
                    report == "Australian Tax Invoice")
                    request.tax_invoice_options.custom_title = param.value;
                else
                    request.invoice_options.custom_title = param.value;
            }
            else
            {
                /* The report's own text option (the lots viewer's
                 * description filter, the IFRS fee action), read by name. */
                request.texts[param.name] = param.value;
            }
            break;
        }

        case gnc::report::OptionType::Bool:
        {
            bool value = false;
            if (param.value == "true" || param.value == "1")
                value = true;
            else if (param.value == "false" || param.value == "0")
                value = false;
            else
            {
                error = "invalid boolean '" + param.value + "' for option '" +
                        param.name + "' (expected true or false)";
                return false;
            }
            bool accepted = false;
            if (report == "Tax Schedule Report/TXF Export")
            {
                if (param.name == "suppress-zero")
                {
                    request.tax_suppress_zero = value;
                    accepted = true;
                }
            }
            else if (report == "Tax Invoice" ||
                     report == "Australian Tax Invoice")
                accepted = gnc::report::set_tax_invoice_option (
                    request.tax_invoice_options, param.name, value);
            else if (gnc::report::is_owner_report (report))
                accepted = gnc::report::set_owner_report_option (
                    request.owner_options, param.name, value);
            else if (report == "Customer Summary")
                accepted = gnc::report::set_customer_summary_option (
                    request.customer_summary_options, param.name, value);
            else
                accepted = gnc::report::set_invoice_print_option (
                    request.invoice_options, param.name, value);
            if (!accepted)
            {
                /* A switch the report publishes and this route has no
                 * dedicated field for: store it by the descriptor's name for
                 * the report to read, with its own default for an omitted
                 * one. */
                request.booleans[param.name] = value;
            }
            break;
        }

        case gnc::report::OptionType::Invoice:
        {
            if (param.name != "invoice")
            {
                error = "report '" + report + "' has no invoice option '" +
                        param.name + "'";
                return false;
            }
            if (param.value.empty ())
            {
                error = "empty 'invoice' value (expected an invoice id or "
                        "GUID)";
                return false;
            }
            have_invoice = true;
            request.invoice = param.value;
            break;
        }

        case gnc::report::OptionType::Owner:
        {
            if (param.name != "owner")
            {
                error = "report '" + report + "' has no owner option '" +
                        param.name + "'";
                return false;
            }
            if (param.value.empty ())
            {
                error = "empty 'owner' value (expected an owner GUID)";
                return false;
            }
            gnc::report::OwnerKind kind;
            if (!gnc::report::owner_report_kind (report, kind))
            {
                error = "report '" + report +
                        "' has no owner selection";
                return false;
            }
            GncOwner owner;
            if (!gnc::report::owner_lookup (g_server.root, kind, param.value,
                                            owner))
            {
                error = "unknown owner '" + param.value + "' for report '" +
                        report + "'";
                return false;
            }
            request.owner = param.value;
            break;
        }
        }
    }

    if (have_period)
    {
        if (have_start || have_end)
        {
            error = "'period' cannot be combined with 'start' or 'end'";
            return false;
        }
        gnc::report::DateRange range;
        if (!gnc::report::resolve_period (period, range))
        {
            error = "unknown period '" + period +
                    "' (expected this-year, last-year or all)";
            return false;
        }
        if (period == "all")
            request.all_dates = true;
        else
        {
            request.start = range.start;
            request.end = range.end;
        }
    }

    if (have_accounts)
    {
        if (all_accounts && !guids.empty ())
        {
            error = "'accounts=all' cannot be combined with account GUIDs";
            return false;
        }
        if (!apply_accounts (all_accounts, guids, request.accounts, error))
            return false;
    }

    if (have_currency)
    {
        QofBook* book = g_server.root != nullptr
                            ? gnc_account_get_book (g_server.root)
                            : nullptr;
        gnc_commodity* commodity = nullptr;
        if (book != nullptr)
            commodity = gnc_commodity_table_lookup (
                gnc_commodity_table_get_table (book),
                GNC_COMMODITY_NS_CURRENCY, currency.c_str ());
        if (commodity == nullptr)
        {
            error = "unknown currency '" + currency + "'";
            return false;
        }
        request.currency = commodity;
    }

    if (have_invoice)
    {
        gnc::report::InvoiceDocument document;
        std::string why;
        if (!gnc::report::find_invoice_document (g_server.root,
                                                 request.invoice, document,
                                                 &why))
        {
            error = "unknown invoice '" + request.invoice + "': " + why;
            return false;
        }
    }

    return true;
}

/* The first path segment is the token. Anything else is a 404, which is also
 * what an attacker without the token sees. */
bool
authorised (const std::string& path)
{
    const std::string prefix = "/" + g_server.token + "/";
    return path.compare (0, prefix.size(), prefix) == 0;
}

std::string
static_file (const char* name)
{
    std::string body;
    gchar* contents = nullptr;
    gsize length = 0;
    const gchar* dir = g_getenv ("GNC_WEBUI_DIR");

    if (dir == nullptr)
        dir = GNC_WEBUI_DIR;

    gchar* path = g_build_filename (dir, name, nullptr);
    if (g_file_get_contents (path, &contents, &length, nullptr))
        body.assign (contents, length);
    g_free (path);
    g_free (contents);
    return body;
}

/* The vendored Chart.js.  It is served straight out of borrowed/ so the tree
 * has one copy of a 200 KB library, not two: an installed build copies the
 * same file into webui/vendor at install time (see web/CMakeLists.txt), and
 * GNC_CHARTJS can point at it for a build that is run from somewhere else.
 * chart.umd.js is the file borrowed/chartjs-4 already installs and ships in
 * the distribution, so no new copy is added to the repository. */
std::string
chartjs_file ()
{
    const gchar* override = g_getenv ("GNC_CHARTJS");
    if (override != nullptr && override[0] != '\0')
    {
        std::string body;
        gchar* contents = nullptr;
        gsize length = 0;
        if (g_file_get_contents (override, &contents, &length, nullptr))
            body.assign (contents, length);
        g_free (contents);
        if (!body.empty ())
            return body;
    }

    const gchar* dir = g_getenv ("GNC_WEBUI_DIR");
    if (dir == nullptr)
        dir = GNC_WEBUI_DIR;

    /* Installed layout: <webui>/vendor/chart.umd.js.  Source layout:
     * <src>/web/ui/../../borrowed/chartjs-4/chart.umd.js. */
    gchar* installed = g_build_filename (dir, "vendor", "chart.umd.js",
                                         nullptr);
    gchar* source = g_build_filename (dir, "..", "..", "borrowed", "chartjs-4",
                                      "chart.umd.js", nullptr);
    std::string body;
    for (gchar* path : {installed, source})
    {
        gchar* contents = nullptr;
        gsize length = 0;
        if (g_file_get_contents (path, &contents, &length, nullptr))
        {
            body.assign (contents, length);
            g_free (contents);
            break;
        }
        g_free (contents);
    }
    g_free (installed);
    g_free (source);
    return body;
}

gboolean
on_incoming (GSocketService*, GSocketConnection* connection, GObject*, gpointer)
{
    /* Browsers open speculative connections and then say nothing on them.
     * This loop is single-threaded, so an unbounded read on one of those
     * blocks the document request behind it and the page never loads. A
     * local request arrives immediately; anything slower is not a request. */
    g_socket_set_timeout (g_socket_connection_get_socket (connection), 1);

    const HttpRequest request = read_request (connection);
    const std::string path = request.path;

    if (!authorised (path))
    {
        send_response (connection, "404 Not Found", "text/plain", "not found\n");
        return TRUE;
    }

    const std::string prefix = "/" + g_server.token + "/";
    std::string route = path.substr (prefix.size());
    std::string query;
    const size_t question = route.find ('?');
    if (question != std::string::npos)
    {
        query = route.substr (question + 1);
        route.resize (question);
    }

    if (route == "api/book")
    {
        std::string body = "{\"accounts\":";
        body += std::to_string (g_server.accounts);
        body += ",\"top_level\":";
        body += std::to_string (g_server.top_level);
        body += "}";
        send_response (connection, "200 OK", "application/json", body);
    }
    else if (route == "api/accounts")
    {
        send_response (connection, "200 OK", "application/json",
                       account_json (g_server.root));
    }
    else if (route.compare (0, 10, "report-id/") == 0)
    {
#ifdef GNC_WITH_REPORT
        /* Report-to-report links carry the engine's own id string. */
        const std::string id_string = url_decode (route.substr (10));
        char* html = nullptr;
        char* errmsg = nullptr;
        if (gnc_run_report_id_string_with_error_handling (id_string.c_str (),
                                                         &html, &errmsg))
        {
            send_response (connection, "200 OK", "text/html; charset=utf-8",
                           rewrite_links (html != nullptr ? html : "",
                                          g_server.token));
            g_free (html);
        }
        else
        {
            send_response (connection, "404 Not Found", "text/plain",
                           errmsg != nullptr ? errmsg : "no such report\n");
            g_free (errmsg);
        }
#else
        reports_unavailable (connection);
#endif
    }
    else if (route.compare (0, 9, "register/") == 0)
    {
        /* The drill-down target: an account's register, rendered here so a
         * link inside a report frame can simply navigate to it. */
        std::string spec = url_decode (route.substr (9));
        const std::string prefix = "acct-guid=";
        if (spec.compare (0, prefix.size (), prefix) == 0)
            spec = spec.substr (prefix.size ());

        GncGUID guid;
        Account* account = nullptr;
        if (string_to_guid (spec.c_str (), &guid))
            account = xaccAccountLookup (&guid, gnc_account_get_book (g_server.root));

        if (account == nullptr)
        {
            send_response (connection, "404 Not Found", "text/plain",
                           "no such account\n");
            return TRUE;
        }

        send_response (connection, "200 OK", "text/html; charset=utf-8",
                       register_html (account));
    }
    else if (route.compare (0, 13, "api/register/") == 0)
    {
        /* Reports link to accounts by GUID; this is the endpoint those links
         * will be rewritten to point at. Accepts both "acct-guid=<guid>" (the
         * form the report HTML uses) and a bare GUID. */
        std::string spec = url_decode (route.substr (13));
        const std::string prefix = "acct-guid=";
        if (spec.compare (0, prefix.size (), prefix) == 0)
            spec = spec.substr (prefix.size ());

        GncGUID guid;
        Account* account = nullptr;
        if (string_to_guid (spec.c_str (), &guid))
            account = xaccAccountLookup (&guid, gnc_account_get_book (g_server.root));

        if (account == nullptr)
        {
            send_response (connection, "404 Not Found", "text/plain",
                           "no such account\n");
            return TRUE;
        }

        send_response (connection, "200 OK", "application/json",
                       register_json (account));
    }
    else if (route == "api/reports")
    {
        /* ?options=1 lists the native reports with the options each accepts,
         * which is what an options form needs.  The bare list keeps its old
         * shape (an array of names) for the UI and the smoke test. */
        if (query == "options=1")
        {
            send_response (connection, "200 OK", "application/json",
                           native_reports_options_json ());
        }
        else
        {
#ifdef GNC_WITH_REPORT
            send_response (connection, "200 OK", "application/json",
                           with_builtin_report_names (reports_json ()));
#else
            send_response (connection, "200 OK", "application/json",
                           with_builtin_report_names (native_reports_json ()));
#endif
        }
    }
    else if (route == "api/spec/options")
    {
        send_response (connection, "200 OK", "application/json",
                       spec_options_json ());
    }
    else if (route == "api/spec/preview")
    {
        /* Preview an unsaved spec.  Nothing is written; the same validator
         * and renderer a saved report uses run here, so a preview cannot
         * disagree with what Save will produce. */
        if (request.method != "POST")
        {
            send_response (connection, "405 Method Not Allowed", "text/plain",
                           "previewing a spec needs POST\n");
        }
        else if (request.body_too_large)
        {
            send_response (connection, "413 Payload Too Large", "text/plain",
                           "the spec is larger than 1 MiB\n");
        }
        else
        {
            web::ReportSpec spec;
            std::string error;
            if (!web::spec_from_json (request.body, g_server.root, spec, error))
            {
                send_response (connection, "400 Bad Request", "text/plain",
                               error + "\n");
            }
            else
            {
                std::string html;
                if (!web::render_spec_html (spec, g_server.root, html, error))
                    send_response (connection, "400 Bad Request", "text/plain",
                                   error + "\n");
                else
                    send_response (connection, "200 OK",
                                   "text/html; charset=utf-8",
                                   rewrite_links (html, g_server.token));
            }
        }
    }
    else if (route == "api/specs")
    {
        if (request.method == "GET")
        {
            std::vector<web::ReportSpec> specs;
            std::string error;
            if (!web::load_specs (specs, error))
                send_response (connection, "500 Internal Server Error",
                               "text/plain", error + "\n");
            else
                send_response (connection, "200 OK", "application/json",
                               web::specs_to_json (specs));
        }
        else if (request.method == "POST")
        {
            if (request.body_too_large)
            {
                send_response (connection, "413 Payload Too Large",
                               "text/plain",
                               "the spec is larger than 1 MiB\n");
            }
            else
            {
                web::ReportSpec spec;
                std::string error;
                if (!web::spec_from_json (request.body, g_server.root, spec,
                                          error))
                {
                    send_response (connection, "400 Bad Request", "text/plain",
                                   error + "\n");
                }
                else
                {
                    std::vector<web::ReportSpec> specs;
                    if (!web::load_specs (specs, error))
                    {
                        send_response (connection, "500 Internal Server Error",
                                       "text/plain", error + "\n");
                    }
                    else
                    {
                        bool duplicate = false;
                        for (const web::ReportSpec& existing : specs)
                            duplicate = duplicate ||
                                        existing.name == spec.name;
                        for (const std::string& builtin :
                             web::builtin_spec_names ())
                            duplicate = duplicate || builtin == spec.name;
                        if (duplicate)
                        {
                            send_response (
                                connection, "400 Bad Request", "text/plain",
                                "a report named '" + spec.name +
                                    "' already exists\n");
                        }
                        else
                        {
                            gchar* id = g_uuid_string_random ();
                            spec.id = id != nullptr ? id : "";
                            g_free (id);
                            specs.push_back (spec);
                            if (!web::save_specs (specs, error))
                                send_response (
                                    connection, "500 Internal Server Error",
                                    "text/plain", error + "\n");
                            else
                                send_response (connection,
                                               "201 Created",
                                               "application/json",
                                               web::spec_to_json (spec));
                        }
                    }
                }
            }
        }
        else
        {
            send_response (connection, "405 Method Not Allowed", "text/plain",
                           "use GET to list saved reports, POST to create\n");
        }
    }
    else if (route.compare (0, 10, "api/specs/") == 0)
    {
        const std::string id = url_decode (route.substr (10));
        std::vector<web::ReportSpec> specs;
        std::string error;
        if (!web::load_specs (specs, error))
        {
            send_response (connection, "500 Internal Server Error",
                           "text/plain", error + "\n");
        }
        else
        {
            auto found = std::find_if (
                specs.begin (), specs.end (),
                [&id] (const web::ReportSpec& spec) { return spec.id == id; });
            web::ReportSpec builtin;
            const bool is_builtin =
                found == specs.end () && web::builtin_spec_by_id (id, builtin);
            if (request.method == "GET")
            {
                if (found != specs.end ())
                    send_response (connection, "200 OK", "application/json",
                                   web::spec_to_json (*found));
                else if (is_builtin)
                    send_response (connection, "200 OK", "application/json",
                                   web::spec_to_json (builtin, true));
                else
                    send_response (connection, "404 Not Found", "text/plain",
                                   "no saved report with id '" + id + "'\n");
            }
            else if (request.method == "DELETE")
            {
                if (is_builtin)
                    send_response (connection, "400 Bad Request", "text/plain",
                                   "the built-in report '" + builtin.name +
                                       "' cannot be deleted\n");
                else if (found == specs.end ())
                    send_response (connection, "404 Not Found", "text/plain",
                                   "no saved report with id '" + id + "'\n");
                else
                {
                    specs.erase (found);
                    if (!web::save_specs (specs, error))
                        send_response (
                            connection, "500 Internal Server Error",
                            "text/plain", error + "\n");
                    else
                        send_response (connection, "200 OK",
                                       "application/json",
                                       "{\"deleted\":\"" + id + "\"}");
                }
            }
            else
            {
                send_response (connection, "405 Method Not Allowed",
                               "text/plain",
                               "use GET or DELETE on a saved report\n");
            }
        }
    }
    else if (route.compare (0, 12, "report/spec/") == 0)
    {
        /* A saved spec renders through the native layer, exactly like a
         * built-in report; the id is the only thing in the path. */
        const std::string id = url_decode (route.substr (12));
        web::ReportSpec spec;
        std::string error;
        if (!find_spec (id, spec, error) &&
            !web::builtin_spec_by_id (id, spec))
        {
            send_response (connection, "404 Not Found", "text/plain",
                           error + "\n");
        }
        else
        {
            std::string html;
            if (!web::render_spec_html (spec, g_server.root, html, error))
                send_response (connection, "500 Internal Server Error",
                               "text/plain", error + "\n");
            else
                send_response (connection, "200 OK",
                               "text/html; charset=utf-8",
                               rewrite_links (html, g_server.token));
        }
    }
    else if (route.compare (0, 11, "api/report/") == 0)
    {
        /* /api/report/<name>/options -- the option descriptors as JSON.  A
         * native report always answers; a name the native layer does not
         * know is a 404, because there is no descriptor to invent. */
        const std::string rest = route.substr (11);
        const std::string suffix = "/options";
        if (rest.size () <= suffix.size () ||
            rest.compare (rest.size () - suffix.size (), suffix.size (),
                          suffix) != 0)
        {
            send_response (connection, "404 Not Found", "text/plain",
                           "not found\n");
        }
        else
        {
            const std::string name =
                url_decode (rest.substr (0, rest.size () - suffix.size ()));
            if (gnc::report::report_options (name).empty ())
                send_response (connection, "404 Not Found", "text/plain",
                               "no options for '" + name + "'\n");
            else
                send_response (connection, "200 OK", "application/json",
                               report_options_json (name));
        }
    }
    else if (route.compare (0, 7, "report/") == 0)
    {
        /* Native reports are rendered in this process and need no Guile.
         * When the build also has the Scheme engine, names the native layer
         * does not know fall through to it; without Guile they stay 501.
         * The query string is the report's option surface: start/end dates,
         * accounts, currency and the period presets. */
        const std::string name = url_decode (route.substr (7));

        gnc::report::ReportRequest request;
        std::string error;
        /* A report the native layer does not know falls through to the
         * Scheme engine (or the 501 when there is none); only a native
         * report, or one asked with options at all, goes through the
         * option validator. */
        if (!gnc::report::report_options (name).empty () || !query.empty ())
        {
            if (!build_report_request (name, query, request, error))
            {
                send_response (connection, "400 Bad Request", "text/plain",
                               error + "\n");
                return TRUE;
            }
        }

        /* The Tax Schedule report's second output: the TXF file is plain
         * text, not an HTML document, so it gets its own content type. */
        if (request.tax_format == "txf")
        {
            std::string txf;
            if (gnc::report::render_tax_export (name, g_server.root, request,
                                                txf))
            {
                send_response (connection, "200 OK",
                               "text/plain; charset=utf-8", txf);
                return TRUE;
            }
        }

        std::string native_html;
        if (gnc::report::render_report (name, g_server.root, request,
                                        native_html))
        {
            send_response (connection, "200 OK", "text/html; charset=utf-8",
                           rewrite_links (native_html, g_server.token));
        }
#ifdef GNC_WITH_REPORT
        else
        {
            /* Rendered here, in this process: the report engine is
             * toolkit-free, so there is no helper process to spawn and no
             * GTK to avoid. */
            SCM get_id = scm_c_eval_string ("gnc:cmdline-get-report-id");
            SCM id = scm_call_1 (get_id, scm_from_utf8_string (name.c_str ()));

            if (scm_is_false (id))
            {
                send_response (connection, "404 Not Found", "text/plain",
                               "no such report\n");
                return TRUE;
            }

            char* html = nullptr;
            char* errmsg = nullptr;
            if (gnc_run_report_with_error_handling (scm_to_int (id), &html,
                                                    &errmsg))
            {
                const std::string document = rewrite_links (
                    html != nullptr ? html : "", g_server.token);
                send_response (connection, "200 OK",
                               "text/html; charset=utf-8", document);
                g_free (html);
            }
            else
            {
                send_response (connection, "500 Internal Server Error",
                               "text/plain",
                               errmsg != nullptr ? errmsg
                                                 : "report failed\n");
                g_free (errmsg);
            }
        }
#else
        else
        {
            reports_unavailable (connection);
        }
#endif
    }
    else if (route == "vendor/chart.umd.js")
    {
        /* Vendored Chart.js, served from borrowed/ (see chartjs_file). */
        std::string script = chartjs_file ();
        if (script.empty ())
        {
            send_response (connection, "404 Not Found", "text/plain",
                           "chart.umd.js was not found in this build\n");
        }
        else
        {
            send_response (connection, "200 OK",
                           "application/javascript; charset=utf-8", script);
        }
    }
    else if (route.empty() || route == "index.html")
    {
        send_response (connection, "200 OK", "text/html; charset=utf-8",
                       static_file ("index.html"));
    }
    else
    {
        send_response (connection, "404 Not Found", "text/plain", "not found\n");
    }

    return TRUE;
}

/* ---------------------------------------------------------------- startup */

std::string
random_token ()
{
    char raw[16];
    for (char& byte : raw)
        byte = static_cast<char> (g_random_int_range (0, 256));

    gchar* hex = g_strdup_printf (
        "%02x%02x%02x%02x%02x%02x%02x%02x"
        "%02x%02x%02x%02x%02x%02x%02x%02x",
        (unsigned char) raw[0], (unsigned char) raw[1],
        (unsigned char) raw[2], (unsigned char) raw[3],
        (unsigned char) raw[4], (unsigned char) raw[5],
        (unsigned char) raw[6], (unsigned char) raw[7],
        (unsigned char) raw[8], (unsigned char) raw[9],
        (unsigned char) raw[10], (unsigned char) raw[11],
        (unsigned char) raw[12], (unsigned char) raw[13],
        (unsigned char) raw[14], (unsigned char) raw[15]);
    std::string token (hex);
    g_free (hex);
    return token;
}

struct Arguments
{
    const char* backend = nullptr;
    const char* book = nullptr;
};

void
webd_main (void* data, int argc, char** argv)
{
    auto* args = static_cast<Arguments*>(data);

#ifdef GNC_WITH_REPORT
    /* The same prelude the CLI uses for `-R run`: the Guile modules bring up
     * the engine (qof_init and the object registry) themselves, and doing it
     * again here trips the engine's own assertions. Backends are found through
     * GNC_MODULE_PATH by qof_session_begin. */
    scm_c_eval_string ("(debug-set! stack 200000)");
    scm_c_use_module ("gnucash utilities");
    scm_c_use_module ("gnucash app-utils");
    scm_c_use_module ("gnucash reports");

    gnc_report_init ();
    /* The native reports read preferences too -- the sign-reversal rule that
     * decides whether an equity or income running balance is shown negated
     * comes from gsettings -- and the Guile modules do not load the backend
     * for us. */
    gnc_prefs_init ();
#else
    /* No Guile and no report engine: bring the engine up directly. This is
     * what libgnucash-guile's module initialiser did in the Guile build --
     * qof_init, the core object registry and the backend modules.  The
     * app-utils preferences are part of that prelude as well: report
     * defaults (the accounting-period end date, print formats) read them. */
    gnc_engine_init (argc, argv);
    gnc_prefs_init ();
#endif

    QofSession* session = gnc_get_current_session ();
    if (session == nullptr)
    {
        std::fprintf (stderr, "webd: no session\n");
        exit (1);
    }

    qof_session_begin (session, args->book, SESSION_READ_ONLY);
    if (qof_session_get_error (session) != ERR_BACKEND_NO_ERR)
    {
        std::fprintf (stderr, "webd: cannot open %s: %s\n", args->book,
                      qof_session_get_error_message (session));
        exit (1);
    }

    qof_session_load (session, nullptr);
    if (qof_session_get_error (session) != ERR_BACKEND_NO_ERR)
    {
        std::fprintf (stderr, "webd: cannot load %s: %s\n", args->book,
                      qof_session_get_error_message (session));
        exit (1);
    }

    QofBook* book = qof_session_get_book (session);
    g_server.root = gnc_book_get_root_account (book);
    g_server.accounts = g_server.root ? gnc_account_n_descendants (g_server.root) : 0;
    g_server.top_level = g_server.root
        ? static_cast<int> (g_list_length (gnc_account_get_children (g_server.root)))
        : 0;
    g_server.token = random_token ();

    GSocketService* service = g_socket_service_new ();
    GInetAddress* loopback = g_inet_address_new_loopback (G_SOCKET_FAMILY_IPV4);
    GSocketAddress* address = g_inet_socket_address_new (loopback, 0);
    GError* error = nullptr;
    guint16 port = 0;

    GSocketAddress* effective = nullptr;
    if (!g_socket_listener_add_address (
            G_SOCKET_LISTENER (service), address, G_SOCKET_TYPE_STREAM,
            G_SOCKET_PROTOCOL_TCP, nullptr, &effective, &error))
    {
        std::fprintf (stderr, "webd: cannot listen: %s\n", error->message);
        exit (1);
    }

    port = g_inet_socket_address_get_port (G_INET_SOCKET_ADDRESS (effective));
    g_object_unref (effective);
    g_object_unref (address);
    g_object_unref (loopback);

    g_signal_connect (service, "incoming", G_CALLBACK (on_incoming), nullptr);
    g_socket_service_start (service);

    std::printf ("webd listening on http://127.0.0.1:%u/%s/\n", port,
                 g_server.token.c_str ());
    std::fflush (stdout);

    g_main_loop_run (g_main_loop_new (nullptr, FALSE));
}

} // namespace

int
main (int argc, char** argv)
{
    Arguments args;

    for (int i = 1; i < argc; ++i)
    {
        if (g_strcmp0 (argv[i], "--backend") == 0 && i + 1 < argc)
            args.backend = argv[++i];
        else if (args.book == nullptr)
            args.book = argv[i];
    }

    if (args.book == nullptr)
    {
        std::fprintf (stderr,
                      "usage: %s [--backend <backend-module>] <book>\n", argv[0]);
        return 2;
    }

#ifdef GNC_WITH_REPORT
    /* Guile owns the main thread; everything above happens inside this. */
    scm_boot_guile (argc, argv, webd_main, &args);
#else
    webd_main (&args, argc, argv);
#endif
    return 0;
}
