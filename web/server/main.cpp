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
#include <cstdlib>

#include <algorithm>
#include <set>
#include <string>
#include <vector>

#include <gio/gio.h>
#include <glib.h>
#ifndef G_OS_WIN32
#include <glib-unix.h>
#endif
#include <csignal>
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
#include <gnc-uri-utils.h>
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

    /* ADR 0013.  The session is the one this process owns; `writable` is fixed
     * at startup and is what every write route checks first.  `locked` means
     * writable was asked for but the book already had a lock, so the session
     * was opened read-only and saves are refused with the lock's path. */
    QofSession* session = nullptr;
    QofBook* book = nullptr;
    bool writable = false;
    bool locked = false;
    std::string lock_path;
    std::string book_path;

    /* ADR 0012.  Non-zero when --debug-port asked for a fixed loopback port.
     * In that mode the token segment is gone from the URL: every path is the
     * natural one, the banner and the startup warning say so, and the socket
     * still binds 127.0.0.1 only.  Zero is the normal per-run-token mode. */
    int debug_port = 0;
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

/* The account tree's total: the recursive present balance in the account's
 * own currency, sign-reversed for credit-normal accounts and formatted by the
 * engine's own balance printer.  /api/accounts and the register header both
 * call this, so the tree and the header cannot disagree: there is one
 * calculation, not two that could drift apart. */
std::string
account_total_text (Account* account, bool* negative = nullptr)
{
    gboolean tree_negative = FALSE;
    gchar* text = gnc_ui_account_get_print_balance (
        xaccAccountGetPresentBalanceInCurrency, account, TRUE, &tree_negative);
    if (negative != nullptr)
        *negative = tree_negative != FALSE;
    std::string result = text != nullptr ? text : "";
    g_free (text);
    return result;
}

/* The two figures the tree and a register header both show for one account.
 * `own` is the account's own balance; `subtree` is the recursive present
 * balance the account tree calls its total, sign-reversed for credit-normal
 * accounts and printed by account_total_text().  Comparing them in the
 * engine's signed space is what lets a header print one figure for a leaf and
 * two for a parent. */
struct AccountFigures
{
    gnc_numeric own{};
    gnc_numeric subtree{};
    std::string subtree_text;
    bool subtree_negative = false;
    bool own_equals_subtree = false;
};

AccountFigures
account_figures (Account* account)
{
    AccountFigures figures;
    figures.own = xaccAccountGetBalance (account);
    figures.subtree =
        xaccAccountGetPresentBalanceInCurrency (account, nullptr, TRUE);
    figures.subtree_text =
        account_total_text (account, &figures.subtree_negative);
    figures.own_equals_subtree =
        gnc_numeric_equal (figures.own, figures.subtree);
    return figures;
}

/* The account's direct children, in the tree's order.  The caller owns the
 * vector; the accounts belong to the book. */
std::vector<Account*>
account_children (Account* account)
{
    std::vector<Account*> children;
    GList* list = gnc_account_get_children (account);
    for (GList* node = list; node != nullptr; node = node->next)
        children.push_back (static_cast<Account*> (node->data));
    g_list_free (list);
    return children;
}

std::string
account_json (Account* account)
{
    std::string out = "{\"name\":\"";
    out += json_escape (xaccAccountGetName (account));
    out += "\",\"guid\":\"";
    out += json_escape (account_guid_text (account).c_str ());
    out += "\",\"total\":\"";

    bool negative = false;
    const std::string total = account_total_text (account, &negative);
    out += json_escape (total.c_str ());

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
 * are what the report list offers beyond the native thirty-one, and with Guile
 * they sit beside the Scheme templates.  A built-in whose name is already a
 * Scheme report replaces it in the list: the name means one report, and the
 * built-in is the native implementation of it.  The plain /api/reports list
 * stays an array of names. */
std::string
with_builtin_report_names (std::string body)
{
    if (!body.empty () && body.back () == ']')
        body.pop_back ();
    bool first = body == "[";
    for (const std::string& name : web::builtin_spec_names ())
    {
        const std::string quoted = "\"" + json_escape (name.c_str ()) + "\"";
        /* A Scheme template with the same name is already in the list; the
         * built-in takes its place so the user sees the name once. */
        if (body.find (quoted) != std::string::npos)
            continue;
        if (!first)
            body += ",";
        first = false;
        body += quoted;
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
    case gnc::report::OptionType::Number:      return "number";
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
     * boolean, a number to a JSON number, everything else to a string; keep
     * the JSON types honest so a form can use them as is. */
    if (option.type == gnc::report::OptionType::AccountList)
        out += "[]";
    else if (option.type == gnc::report::OptionType::Bool)
        out += option.default_value == "true" ? "true" : "false";
    else if (option.type == gnc::report::OptionType::Number)
        out += option.default_value.empty () ? "0" : option.default_value;
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
    body += "],\"sources\":["
            "{\"name\":\"accounts\",\"label\":\"Accounts and periods\"},"
            "{\"name\":\"splits\",\"label\":\"Transactions' splits\"},"
            "{\"name\":\"balance-sheet\",\"label\":\"Balance sheet (the "
            "report's fixed sections)\"},"
            "{\"name\":\"invoices\",\"label\":\"Invoices and bills (the aging "
            "reports)\"},"
            "{\"name\":\"lots\",\"label\":\"Lots (the Lot Viewer's transaction "
            "table)\"},"
            "{\"name\":\"budgets\",\"label\":\"Budgets (the Budget Report's "
            "budget-vs-actual table)\"},"
            "{\"name\":\"tax-codes\",\"label\":\"Tax codes (the US Tax "
            "Schedule's code totals)\"},"
            "{\"name\":\"cash-flow\",\"label\":\"Cash flow (the Cash Flow "
            "statement's rows)\"},"
            "{\"name\":\"document\",\"label\":\"Document (an invoice, bill, "
            "voucher or receipt, not a table)\"}],"
            "\"balance_sheet\":{\"columns\":[";
    first = true;
    for (const std::string& column : web::balance_sheet_columns ())
    {
        if (!first)
            body += ",";
        first = false;
        body += "\"";
        body += json_escape (column.c_str ());
        body += "\"";
    }
    body += "],\"options\":[\"end\",\"currency\"],"
            "\"note\":\"A balance-sheet spec is the native Balance Sheet's own "
            "table: the Assets, Liabilities and Equity sections in the "
            "report's order, liabilities and equity sign-reversed, the income "
            "and expense balances folded into one Retained Earnings line, and "
            "the report's totals and chart.  It takes an as-of date ('end') "
            "and a currency; it has no start date, date preset, grouping, "
            "column selection, account scope or chart option, and a request "
            "that sets one is refused by name rather than ignored.\"},"
            "\"invoices\":{\"owner_columns\":[";
    first = true;
    for (const std::string& column : web::invoice_owner_columns ())
    {
        if (!first)
            body += ",";
        first = false;
        body += "\"";
        body += json_escape (column.c_str ());
        body += "\"";
    }
    body += "],\"document_columns\":[";
    first = true;
    for (const std::string& column : web::invoice_document_columns ())
    {
        if (!first)
            body += ",";
        first = false;
        body += "\"";
        body += json_escape (column.c_str ());
        body += "\"";
    }
    body += "],\"owners\":["
            "{\"name\":\"receivable\",\"label\":\"Receivable Aging (every "
            "A/Receivable account)\"},"
            "{\"name\":\"payable\",\"label\":\"Payable Aging (every A/Payable "
            "account)\"}],"
            "\"groupings\":["
            "{\"name\":\"owner\",\"label\":\"One row per owner (the aging "
            "report's own table)\"},"
            "{\"name\":\"document\",\"label\":\"One row per invoice, credit "
            "note, bill or payment\"}],"
            "\"bucket_basis\":["
            "{\"name\":\"due\",\"label\":\"Due date (the report's default)\"},"
            "{\"name\":\"posted\",\"label\":\"Post date\"}],"
            "\"options\":[\"end\"],"
            "\"note\":\"An invoices spec is the aging reports' own rows and "
            "arithmetic: the owner rows of the Receivable Aging or the Payable "
            "Aging as of 'end' ('To'), with the report's six buckets "
            "(Pre-Payment, Current, 0-30, 31-60, 61-90 and 91+ days), its sort "
            "by owner name and its Total row per account.  An invoice "
            "contributes its remaining balance as of that date, so a partially "
            "paid one shows only what is still owed, and a payment contributes "
            "only its unapplied part, into Pre-Payment.  'invoice_owner' picks "
            "the side of the ledger, 'bucket_basis' the report's Due or Post "
            "Date option, and 'invoice_grouping' chooses the report's owner "
            "rows or one row per document.  The report ages every aging account "
            "and does not convert currencies, so there is no account scope, no "
            "currency, no start date or preset, no chart and no grouping: a "
            "request that sets one is refused by name rather than "
            "ignored.\"},"
            "\"lots\":{\"columns\":[";
    first = true;
    for (const std::string& column : web::lots_columns ())
    {
        if (!first)
            body += ",";
        first = false;
        body += "\"";
        body += json_escape (column.c_str ());
        body += "\"";
    }
    body += "],\"options\":[\"start\",\"end\",\"accounts\","
            "\"description_filter\"],"
            "\"note\":\"A lots spec is the Lot Viewer's own table: one row per "
            "transaction of the selected account in the period, in the "
            "report's order, with the transaction's date, description and "
            "type, one column per lot those transactions touch (the lot "
            "columns are the book's, not a choice: 'lots' in the column list "
            "stands for them), an Other Account column for the transaction's "
            "remaining splits, and the report's own lot titles, short GUIDs, "
            "Document row and Balance row.  The amounts are transaction "
            "amounts in each lot, printed in the selected account's commodity. "
            "'start' and 'end' are the report's two dates, 'accounts' is its "
            "single Account option -- one account, whose own splits are "
            "listed, and empty for the option's unset value, the whole book -- "
            "and 'description_filter' is its Description Filter.  The report "
            "does not convert currencies and has no account type, grouping, "
            "period, chart or preset option, so a request that sets one is "
            "refused by name rather than ignored.  The two portfolio reports "
            "are not this source: their rows are accounts with priced values, "
            "not lots.\"},"
            "\"budgets\":{\"columns\":[";
    first = true;
    for (const std::string& column : web::budget_columns ())
    {
        if (!first)
            body += ",";
        first = false;
        body += "\"";
        body += json_escape (column.c_str ());
        body += "\"";
    }
    body += "],\"options\":[\"accounts\"],"
            "\"note\":\"A budgets spec is the native Budget Report's own "
            "table: one row per account of its default selection (ASSET, "
            "LIABILITY, INCOME and EXPENSE) or of the accounts it names, laid "
            "out as the account tree to the report's display depth of two, "
            "and one 'Bgt <period start>' / 'Act <period start>' pair per "
            "period of the book's budget.  The periods are the budget's own, "
            "so 'budget' and 'actual' stand for the repeating pair and a "
            "budgets spec has no start, end, date preset, period unit or "
            "period mode; the report always uses the book's default budget, "
            "so there is no budget selection either.  The budget figure is "
            "the report's rolled-up value and the actual is the period's "
            "actual excluding closing entries, both sign-reversed for "
            "credit-normal accounts, and a zero budget prints the report's "
            "'.'.  'accounts' is the report's Account option (a named account "
            "means that account and its subtree); the report prints each "
            "amount in its account's commodity, so a request that sets a "
            "currency, account type, grouping, chart or preset is refused by "
            "name rather than ignored.  With no budget in the book the spec "
            "renders the report's 'No budgets exist. You must create at least "
            "one budget.' page.  The other four budget reports are not this "
            "source: the two budget statements and the budget balance sheet "
            "print whole-budget statement rows, and Budget Flow prints one "
            "type-sectioned period, each needing its own source.\"},"
            "\"tax-codes\":{\"columns\":[";
    first = true;
    for (const std::string& column : web::tax_codes_columns ())
    {
        if (!first)
            body += ",";
        first = false;
        body += "\"";
        body += json_escape (column.c_str ());
        body += "\"";
    }
    body += "],\"options\":[\"start\",\"end\",\"accounts\",\"currency\"],"
            "\"note\":\"A tax-codes spec is the US Tax Schedule report's code "
            "axis: one row per tax code, with the form the code prints on, its "
            "line, its description and the amount the schedule totals for that "
            "code, from the book's tax-related accounts and the report's own "
            "TXF code table.  The figures are the schedule's: its From/To "
            "dates ('start' and 'end'), its account selection ('accounts', "
            "empty for every tax-related account below the root) and its "
            "commodity ('currency').  The report also prints one row per "
            "account with that account's own total and its form and line "
            "headings, so the spec's cells are the report's code totals rather "
            "than its whole page.  A book with no tax setup renders the "
            "report's own 'No Tax Related accounts were found...' page.  The "
            "TXF export, the suppress-zero switch and the alternate-period "
            "presets are the report's own outputs and options, so a request "
            "that sets a grouping, a period unit or mode, a chart, an account "
            "type or a date preset is refused by name rather than "
            "ignored.\"},"
            "\"cash-flow\":{\"columns\":[";
    first = true;
    for (const std::string& column : web::cash_flow_columns ())
    {
        if (!first)
            body += ",";
        first = false;
        body += "\"";
        body += json_escape (column.c_str ());
        body += "\"";
    }
    body += "],\"options\":[\"start\",\"end\",\"accounts\",\"currency\"],"
            "\"note\":\"A cash-flow spec is the Cash Flow statement's own "
            "table: the 'Money into selected accounts comes from' and 'Money "
            "out of selected accounts goes to' sections, one row per "
            "contributing account with the amount that came in or went out, "
            "the Money In and Money Out totals and the Difference, all from "
            "the same classification (cash_flow_flows()) the statement and the "
            "Cash Flow Barchart plot.  A row carries one figure, in the column "
            "for its side: 'money-in', 'money-out' or 'net'.  'start' and "
            "'end' are the report's two dates, 'accounts' its account "
            "selection (empty for its default: every bank, cash, asset, stock "
            "and mutual fund account) and 'currency' the commodity it converts "
            "into.  The 'Selected Accounts' list above the table is page "
            "furniture, not a cell, so a table spec does not carry it, and the "
            "display switches are the report's own: a request that sets a "
            "grouping, a period unit or mode, a chart, an account type or a "
            "date preset is refused by name rather than ignored.  The barchart "
            "is not this source: its axis is a date-interval series, not the "
            "statement's account rows.\"},"
            "\"document\":{\"documents\":[";
    first = true;
    for (const std::string& report : web::document_reports ())
    {
        if (!first)
            body += ",";
        first = false;
        body += "{\"name\":\"";
        body += json_escape (report.c_str ());
        body += "\",\"label\":\"";
        body += json_escape (report.c_str ());
        body += "\",\"options\":[";
        bool first_option = true;
        for (const auto& option : gnc::report::report_options (report))
        {
            if (!first_option)
                body += ",";
            first_option = false;
            body += option_json (option);
        }
        body += "]}";
    }
    body += "],\"options\":[\"invoice\",\"document_options\"],"
            "\"note\":\"A document spec is not a table: it holds a document "
            "report's name ('document'), the document to print ('invoice', by "
            "id or GUID) and that report's own print options "
            "('document_options', by the names the report publishes), and it "
            "renders through the document renderer -- the same code the "
            "/report/<document> route runs, so the same invoice and the same "
            "options print the same bytes.  With no invoice the spec prints "
            "the report's 'no invoice selected' page and with no options its "
            "default document.  A document spec has no columns, grouping, "
            "date range, currency, account scope or chart: a request that sets "
            "one is refused by name rather than ignored.  The six documents "
            "are the four Scheme layouts and the two tax invoices; the owner "
            "reports (Customer, Vendor, Employee, Job and the Customer "
            "Summary) print a document too, and are not this source.\"},"
            "\"split_columns\":[";
    first = true;
    for (const gnc::report::SplitColumn column :
         gnc::report::split_column_vocabulary ())
    {
        if (!first)
            body += ",";
        first = false;
        body += "{\"name\":\"";
        body += json_escape (gnc::report::split_column_name (column));
        body += "\",\"label\":\"";
        body += json_escape (gnc::report::split_column_heading (column));
        body += "\"}";
    }
    body += "],\"split_groupings\":["
            "{\"name\":\"none\",\"label\":\"Flat split list\"},"
            "{\"name\":\"account\",\"label\":\"By account (Transaction "
            "Report)\"},"
            "{\"name\":\"account-code\",\"label\":\"By account code (General "
            "Ledger)\"},"
            "{\"name\":\"transaction\",\"label\":\"By transaction (General "
            "Journal)\"}],"
            "\"groupings\":["
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
            "\"chart_series_note\":\"One or two series, or all three on a "
            "period report: debit, credit and balance are that period's two "
            "sides and its net (income, expense and net profit; assets, "
            "liabilities and net worth). Balance cannot be combined with just "
            "one side.\","
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
guid_text (const GncGUID* guid)
{
    gchar* text = guid_to_string (guid);
    std::string result = text != nullptr ? text : "";
    g_free (text);
    return result;
}

/* One register row, exactly as the page and the JSON show it.
 *
 * The account's split list is the engine's own register order: the account
 * keeps its splits sorted with xaccSplitOrder(), which is what the desktop
 * register's default "BY_STANDARD" sort uses (gnc_split_reg_sort(), BY_STANDARD
 * -> QUERY_DEFAULT_SORT -> xaccSplitOrder).  The running balance is
 * xaccSplitGetBalance(), the number xaccAccountRecomputeBalance() folds into
 * the account balance while walking that same list, so the last row's balance
 * and xaccAccountGetBalance() are the same value rather than two calculations
 * that could drift apart. */
struct RegisterRow
{
    std::string guid;
    std::string transaction_guid;
    /* How many splits the transaction has.  The page uses it to tell a
     * two-split row (whose transfer is one account) from a split transaction
     * (whose transfer cell must offer the panel instead of naming an
     * account).  The engine's placeholder text would say the same thing, but
     * matching a display string is not a contract; the count is. */
    int split_count = 0;
    std::string date;
    std::string date_iso;
    std::string num;
    std::string description;
    std::string memo;
    std::string action;
    std::string transfer_account;
    std::string transfer_account_guid;
    std::string reconcile;
    bool reconciled = false;
    bool voided = false;
    std::string amount;
    std::string running_balance;
};

std::vector<RegisterRow>
register_rows (Account* account)
{
    /* Both are public engine calls and no-ops when nothing changed.  Sorting
     * first matters because the running balance depends on the order. */
    xaccAccountSortSplits (account, TRUE);
    xaccAccountRecomputeBalance (account);

    const GNCPrintAmountInfo info = gnc_account_print_info (account, TRUE);
    std::vector<RegisterRow> rows;

    for (GList* node = xaccAccountGetSplitList (account); node != nullptr;
         node = node->next)
    {
        auto* split = static_cast<Split*>(node->data);
        Transaction* transaction = xaccSplitGetParent (split);
        RegisterRow row;

        row.guid = guid_text (xaccSplitGetGUID (split));
        row.transaction_guid = guid_text (xaccTransGetGUID (transaction));
        row.split_count = xaccTransCountSplits (transaction);

        char date[16] = {0};
        qof_print_date_buff (date, sizeof date, xaccTransGetDate (transaction));
        row.date = date;
        char date_iso[64] = {0};
        gnc_time64_to_iso8601_buff (xaccTransGetDate (transaction), date_iso);
        row.date_iso = date_iso;

        const gchar* num = xaccTransGetNum (transaction);
        row.num = num != nullptr ? num : "";
        const gchar* description = xaccTransGetDescription (transaction);
        row.description = description != nullptr ? description : "";
        const gchar* memo = xaccSplitGetMemo (split);
        row.memo = memo != nullptr ? memo : "";
        const gchar* action = xaccSplitGetAction (split);
        row.action = action != nullptr ? action : "";

        /* The Transfer column: the other account of a two-split transaction,
         * or the engine's own placeholder for a split transaction. */
        Split* other = xaccSplitGetOtherSplit (split);
        if (other != nullptr)
        {
            Account* other_account = xaccSplitGetAccount (other);
            row.transfer_account = xaccAccountGetName (other_account);
            row.transfer_account_guid = account_guid_text (other_account);
        }
        else
        {
            const gchar* corr = xaccSplitGetCorrAccountName (split);
            row.transfer_account = corr != nullptr ? corr : "";
        }

        const char reconcile = xaccSplitGetReconcile (split);
        row.reconcile = std::string (1, reconcile);
        row.reconciled = reconcile == YREC;
        row.voided = xaccTransGetVoidStatus (transaction);

        const gchar* amount = xaccPrintAmount (xaccSplitGetAmount (split), info);
        row.amount = amount != nullptr ? amount : "";
        const gchar* running =
            xaccPrintAmount (xaccSplitGetBalance (split), info);
        row.running_balance = running != nullptr ? running : "";

        rows.push_back (std::move (row));
    }
    return rows;
}

std::string
register_row_json (const RegisterRow& row)
{
    std::string out = "{\"guid\":\"" + json_escape (row.guid.c_str ()) + "\"";
    out += ",\"transaction_guid\":\"" +
           json_escape (row.transaction_guid.c_str ()) + "\"";
    out += ",\"split_count\":" + std::to_string (row.split_count);
    out += ",\"date\":\"" + json_escape (row.date.c_str ()) + "\"";
    out += ",\"date_iso\":\"" + json_escape (row.date_iso.c_str ()) + "\"";
    out += ",\"num\":\"" + json_escape (row.num.c_str ()) + "\"";
    out += ",\"description\":\"" +
           json_escape (row.description.c_str ()) + "\"";
    out += ",\"memo\":\"" + json_escape (row.memo.c_str ()) + "\"";
    out += ",\"action\":\"" + json_escape (row.action.c_str ()) + "\"";
    out += ",\"transfer_account\":\"" +
           json_escape (row.transfer_account.c_str ()) + "\"";
    out += ",\"transfer_account_guid\":\"" +
           json_escape (row.transfer_account_guid.c_str ()) + "\"";
    out += ",\"reconcile\":\"" + json_escape (row.reconcile.c_str ()) + "\"";
    out += ",\"reconciled\":";
    out += row.reconciled ? "true" : "false";
    out += ",\"voided\":";
    out += row.voided ? "true" : "false";
    out += ",\"amount\":\"" + json_escape (row.amount.c_str ()) + "\"";
    out += ",\"running_balance\":\"" +
           json_escape (row.running_balance.c_str ()) + "\"";
    out += "}";
    return out;
}

/* Whether this register may be edited, and for a read-only session the
 * sentence that says why.  This is the same mode gate the write routes use
 * (ADR 0013 §1), so the page cannot offer an edit a route would refuse. */
bool
register_editable (std::string& reason)
{
    if (g_server.writable && !qof_book_is_readonly (g_server.book))
        return true;

    if (g_server.locked)
        reason = "the book is locked by another GnuCash process ("
                 + g_server.lock_path + "), so this session opened read-only; "
                 "close the other process and restart the server to edit";
    else if (!g_server.writable)
        reason = "this session is read-only; start the server with "
                 "--writable (or GNC_WEBUI_WRITABLE=1) to edit the register";
    else
        reason = "the book is marked read-only, so this register cannot be "
                 "edited";
    return false;
}

std::string
register_json (Account* account)
{
    const GNCPrintAmountInfo info = gnc_account_print_info (account, TRUE);
    const std::vector<RegisterRow> rows = register_rows (account);

    /* `own` keeps its old meaning (the account's own balance); the subtree
     * fields are the tree's total and the flags that choose the header form. */
    const AccountFigures figures = account_figures (account);
    const std::vector<Account*> children = account_children (account);

    std::string children_json = "[";
    bool first_child = true;
    for (Account* child : children)
    {
        if (!first_child)
            children_json += ",";
        first_child = false;
        children_json += "{\"name\":\"";
        children_json += json_escape (xaccAccountGetName (child));
        children_json += "\",\"guid\":\"";
        children_json += json_escape (account_guid_text (child).c_str ());
        children_json += "\"}";
    }
    children_json += "]";

    gchar* full_name = gnc_account_get_full_name (account);
    std::string out = "{\"account\":\"";
    out += json_escape (xaccAccountGetName (account));
    out += "\",\"account_guid\":\"";
    out += json_escape (account_guid_text (account).c_str ());
    out += "\",\"full_name\":\"";
    out += json_escape (full_name != nullptr ? full_name : "");
    g_free (full_name);
    out += "\",\"balance\":\"";
    const gchar* balance = xaccPrintAmount (figures.own, info);
    out += json_escape (balance != nullptr ? balance : "");
    out += "\",\"balance_negative\":";
    out += gnc_numeric_negative_p (figures.own) ? "true" : "false";
    out += ",\"subtree_balance\":\"";
    out += json_escape (figures.subtree_text.c_str ());
    out += "\",\"subtree_negative\":";
    out += figures.subtree_negative ? "true" : "false";
    out += ",\"placeholder\":";
    out += xaccAccountGetPlaceholder (account) ? "true" : "false";
    out += ",\"has_children\":";
    out += children.empty () ? "false" : "true";
    out += ",\"own_equals_subtree\":";
    out += figures.own_equals_subtree ? "true" : "false";
    out += ",\"children\":";
    out += children_json;
    out += ",\"row_count\":";
    out += std::to_string (rows.size ());
    out += ",\"editable\":";
    std::string edit_reason;
    const bool editable = register_editable (edit_reason);
    out += editable ? "true" : "false";
    out += ",\"edit_reason\":";
    out += editable ? "\"\"" : "\"" + json_escape (edit_reason.c_str ()) + "\"";
    out += ",\"splits\":[";

    bool first = true;
    for (const RegisterRow& row : rows)
    {
        if (!first)
            out += ",";
        first = false;
        out += register_row_json (row);
    }

    out += "]}";
    return out;
}

/* The read-only transaction panel's data: every split of one transaction, so
 * a register row can show what it is part of without inventing a second
 * register model.
 *
 * The panel also says what the transaction adds up to.  The total is the sum
 * of the debit (positive) values in the transaction's own currency: summing
 * the per-split `amount` strings would add figures from different account
 * commodities, which is not a total.  `balanced` is the engine's answer
 * (xaccTransIsBalanced, so trading accounts are respected); when it is false
 * the imbalance is printed too, in the same currency, rather than hiding the
 * difference. */
std::string
transaction_json (Transaction* transaction)
{
    gnc_commodity* currency = xaccTransGetCurrency (transaction);
    const GNCPrintAmountInfo currency_info =
        gnc_commodity_print_info (currency, TRUE);

    std::string out = "{\"guid\":\"";
    out += json_escape (guid_text (xaccTransGetGUID (transaction)).c_str ());
    out += "\",\"date\":\"";
    char date[16] = {0};
    qof_print_date_buff (date, sizeof date, xaccTransGetDate (transaction));
    out += json_escape (date);
    out += "\",\"date_iso\":\"";
    char date_iso[64] = {0};
    gnc_time64_to_iso8601_buff (xaccTransGetDate (transaction), date_iso);
    out += json_escape (date_iso);
    out += "\",\"num\":\"";
    const gchar* num = xaccTransGetNum (transaction);
    out += json_escape (num != nullptr ? num : "");
    out += "\",\"description\":\"";
    out += json_escape (xaccTransGetDescription (transaction));
    out += "\",\"currency\":\"";
    out += json_escape (currency != nullptr
                        ? gnc_commodity_get_mnemonic (currency) : "");
    out += "\",\"voided\":";
    out += xaccTransGetVoidStatus (transaction) ? "true" : "false";
    out += ",\"splits\":[";

    gnc_numeric total = gnc_numeric_zero ();
    bool first = true;
    for (GList* node = xaccTransGetSplitList (transaction); node != nullptr;
         node = node->next)
    {
        auto* split = static_cast<Split*>(node->data);
        Account* account = xaccSplitGetAccount (split);
        if (!first)
            out += ",";
        first = false;

        const gnc_numeric value = xaccSplitGetValue (split);
        if (gnc_numeric_positive_p (value))
            total = gnc_numeric_add (total, value, GNC_DENOM_AUTO,
                                     GNC_HOW_DENOM_EXACT);

        out += "{\"guid\":\"" +
               json_escape (guid_text (xaccSplitGetGUID (split)).c_str ()) + "\"";
        out += ",\"account\":\"" +
               json_escape (xaccAccountGetName (account)) + "\"";
        out += ",\"account_guid\":\"" +
               json_escape (account_guid_text (account).c_str ()) + "\"";
        const gchar* memo = xaccSplitGetMemo (split);
        out += ",\"memo\":\"" + json_escape (memo != nullptr ? memo : "") + "\"";
        const gchar* action = xaccSplitGetAction (split);
        out += ",\"action\":\"" +
               json_escape (action != nullptr ? action : "") + "\"";
        const char reconcile = xaccSplitGetReconcile (split);
        out += ",\"reconcile\":\"";
        out += json_escape (std::string (1, reconcile).c_str ());
        out += "\",\"reconciled\":";
        out += reconcile == YREC ? "true" : "false";
        const gchar* amount = xaccPrintAmount (
            xaccSplitGetAmount (split), gnc_account_print_info (account, TRUE));
        out += ",\"amount\":\"";
        out += json_escape (amount != nullptr ? amount : "");
        out += "\"}";
    }

    const gchar* total_text = xaccPrintAmount (total, currency_info);
    out += "],\"total\":\"";
    out += json_escape (total_text != nullptr ? total_text : "");
    out += "\",\"balanced\":";
    out += xaccTransIsBalanced (transaction) ? "true" : "false";
    const gchar* imbalance_text = xaccPrintAmount (
        xaccTransGetImbalanceValue (transaction), currency_info);
    out += ",\"imbalance\":\"";
    out += json_escape (imbalance_text != nullptr ? imbalance_text : "");
    out += "\"}";
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
    /* The base every rewritten link is placed under.  With a token it is
     * "/<token>/"; under --debug-port it is the root.  Concatenating
     * "/" + token + "/" would give "//" in that mode, which a browser reads as
     * a protocol-relative URL whose host is the next segment (the Chart.js
     * script became http://vendor/chart.umd.js and never loaded). */
    const std::string base = token.empty () ? std::string ("/")
                                            : "/" + token + "/";
    const std::string register_prefix = "gnc-register:";
    const std::string register_route = base + "register/";
    const std::string report_prefix = "gnc-report:";
    const std::string report_route = base + "report-id/";
    /* The native renderer cannot know the server's token, so it writes the
     * vendored assets with this placeholder and the server fills the token
     * in.  It must not collide with a real URL type (gnc-register:, ...). */
    const std::string webui_prefix = "gnc-webui:";
    const std::string webui_route = base;

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

/* A server-rendered register page: the frame the report navigates into.  It
 * walks the same RegisterRow list the JSON does, so the page's rows, order and
 * balances are the API's, not a second calculation. */
std::string
register_html (Account* account)
{
    const GNCPrintAmountInfo info = gnc_account_print_info (account, TRUE);
    const std::vector<RegisterRow> rows = register_rows (account);
    const AccountFigures figures = account_figures (account);
    const bool has_children = !account_children (account).empty ();
    const bool placeholder = xaccAccountGetPlaceholder (account);

    std::string out = "<!DOCTYPE html><html><head><meta charset='utf-8'><title>";
    out += html_escape (xaccAccountGetName (account));
    out += "</title><style>"
           "body{font-family:system-ui,sans-serif;margin:1rem;color:#222}"
           "h1{font-size:1.2rem;margin:0 0 .25rem}"
           "p.total{color:#555;margin:0 0 1rem}"
           "p.context{color:#555;margin:0 0 1rem}"
           "table{border-collapse:collapse;width:100%}"
           "th,td{padding:.3rem .6rem;border-bottom:1px solid #e0e0e0;text-align:left}"
           "th{font-weight:600;color:#444}"
           "td.num{text-align:right;font-variant-numeric:tabular-nums}"
           "tr.reconciled td.num{color:#080}"
           "tr.voided td{color:#999;text-decoration:line-through}"
           "</style></head><body><h1>";
    out += html_escape (xaccAccountGetName (account));
    out += "</h1><p class='total'>Balance: ";
    const gchar* balance = xaccPrintAmount (figures.own, info);
    out += html_escape (balance != nullptr ? balance : "");
    out += "</p>";

    /* The same two figures the shell's header shows: the account's own balance
     * above, and the tree's subtree total when they differ.  This page is the
     * report frame's drill-down target, so it explains a placeholder the same
     * way the shell does rather than showing an empty table with no reason. */
    if (!figures.own_equals_subtree)
    {
        out += "<p class='total'>Including sub-accounts: ";
        out += html_escape (figures.subtree_text.c_str ());
        out += "</p>";
    }
    if (rows.empty () && (placeholder || has_children))
    {
        out += "<p class='context'>";
        out += (placeholder
            ? "This account is a placeholder: its transactions are in its "
              "sub-accounts."
            : "This account has no transactions of its own: they are in its "
              "sub-accounts.");
        out += "</p>";
    }

    out += "<table><thead><tr><th>Date</th><th>Description</th>"
           "<th>Transfer</th><th class='num'>Amount</th>"
           "<th class='num'>Balance</th></tr></thead><tbody>";

    for (const RegisterRow& row : rows)
    {
        out += "<tr";
        if (row.reconciled)
            out += " class='reconciled'";
        else if (row.voided)
            out += " class='voided'";
        out += "><td>";
        out += html_escape (row.date.c_str ());
        out += "</td><td>";
        out += html_escape (row.description.c_str ());
        out += "</td><td>";
        out += html_escape (row.transfer_account.c_str ());
        out += "</td><td class='num'>";
        out += html_escape (row.amount.c_str ());
        out += "</td><td class='num'>";
        out += html_escape (row.running_balance.c_str ());
        out += "</td></tr>";
    }


    out += "</tbody></table></body></html>";
    return out;
}

/* The register routes accept both "acct-guid=<guid>" (the form the reports'
 * links use) and a bare GUID.  Strip the prefix once, and look the account up
 * in the book this process owns.  A null result means "no such account" for
 * both routes; the caller decides the status and the message. */
std::string
account_spec (std::string spec)
{
    const std::string prefix = "acct-guid=";
    if (spec.compare (0, prefix.size (), prefix) == 0)
        spec = spec.substr (prefix.size ());
    return spec;
}

Account*
lookup_account (const std::string& spec)
{
    GncGUID guid;
    if (!string_to_guid (spec.c_str (), &guid))
        return nullptr;
    return xaccAccountLookup (&guid, gnc_account_get_book (g_server.root));
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
    /* Fetch metadata.  Every browser sends Sec-Fetch-Dest on a request:
     * "document" for a top-level navigation, "iframe" when the request is a
     * frame's navigation, and nothing at all from a script's fetch() or from a
     * command-line client.  The page routes use it to serve the application
     * shell to a reader who navigates to a report or register URL, while the
     * report frame and every harness still receive the document they ask for
     * on the same path.  A client that sends no fetch metadata (curl, the
     * harnesses, an older browser) gets the document, which is what those
     * callers have always received. */
    bool page_navigation = false;
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
            else if (g_ascii_strcasecmp (name.c_str (), "Sec-Fetch-Dest") == 0)
            {
                /* "document" is the only value that means a reader navigated
                 * the top-level page here; "iframe", "empty", "image" and the
                 * rest are subresources and keep the document routes. */
                std::string value = header.substr (colon + 1);
                const size_t first = value.find_first_not_of (" \t");
                const size_t last = value.find_last_not_of (" \t");
                value = first == std::string::npos
                    ? std::string () : value.substr (first, last - first + 1);
                result.page_navigation =
                    g_ascii_strcasecmp (value.c_str (), "document") == 0;
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
            {
                /* The three invoice documents share one option surface, so
                 * the layout the report name selects does not change which
                 * switch names it accepts. */
                gnc::report::InvoiceLayout layout;
                accepted = gnc::report::invoice_layout_from_name (report,
                                                                  layout) &&
                           gnc::report::set_invoice_print_option (
                               request.invoice_options, param.name, value);
            }
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

        case gnc::report::OptionType::Number:
        {
            char* end_text = nullptr;
            const long value = std::strtol (param.value.c_str (), &end_text, 10);
            if (end_text == param.value.c_str () || *end_text != '\0')
            {
                error = "invalid number '" + param.value + "' for option '" +
                        param.name + "' (expected a whole number)";
                return false;
            }
            if ((descriptor->minimum != 0 || descriptor->maximum != 0) &&
                (value < descriptor->minimum || value > descriptor->maximum))
            {
                error = "option '" + param.name + "' must be between " +
                        std::to_string (descriptor->minimum) + " and " +
                        std::to_string (descriptor->maximum) + " (got " +
                        param.value + ")";
                return false;
            }
            request.texts[param.name] = std::to_string (value);
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

/* Percent-encode a query parameter's name or value.  The document spec's
 * options become the document route's own query parameters, so a value with a
 * space, an ampersand or a newline in it (the print options' CSS text) has to
 * survive the round trip. */
std::string
url_encode (const std::string& text)
{
    static const char* digits = "0123456789ABCDEF";
    std::string out;
    for (const unsigned char character : text)
    {
        const bool unreserved =
            (character >= 'A' && character <= 'Z') ||
            (character >= 'a' && character <= 'z') ||
            (character >= '0' && character <= '9') || character == '-' ||
            character == '_' || character == '.' || character == '~';
        if (unreserved)
        {
            out += static_cast<char> (character);
            continue;
        }
        out += '%';
        out += digits[character >> 4];
        out += digits[character & 0x0f];
    }
    return out;
}

/* A document spec is not a table: it holds a document report's name, the
 * invoice to print and that report's own print options.  Rendering it is the
 * /report/<document> route's own work -- the same option validation and the
 * same renderer -- so the two cannot drift apart: the spec's fields become the
 * route's query parameters and build_report_request() applies them. */
bool
render_document_spec (const web::ReportSpec& spec, std::string& html,
                      std::string& error)
{
    std::string query;
    auto add = [&query] (const std::string& name, const std::string& value)
    {
        if (!query.empty ())
            query += "&";
        query += url_encode (name) + "=" + url_encode (value);
    };
    if (!spec.invoice.empty ())
        add ("invoice", spec.invoice);
    for (const auto& option : spec.document_options)
        add (option.first, option.second);

    gnc::report::ReportRequest request;
    if (!build_report_request (spec.document, query, request, error))
        return false;
    if (!gnc::report::render_report (spec.document, g_server.root, request,
                                     html))
    {
        if (error.empty ())
            error = "the document '" + spec.document +
                    "' could not be rendered";
        return false;
    }
    return true;
}

/* The one entry the preview and saved-report routes call: a document spec goes
 * to the document renderer, every other spec to the table renderer. */
bool
render_spec_page (const web::ReportSpec& spec, std::string& html,
                  std::string& error)
{
    if (spec.source == "document")
        return render_document_spec (spec, html, error);
    return web::render_spec_html (spec, g_server.root, html, error);
}

/* The first path segment is the token. Anything else is a 404, which is also
 * what an attacker without the token sees.  --debug-port removes the token
 * segment by design (ADR 0012): every path is accepted, and the banner plus
 * the startup warning are the only notice. */
bool
authorised (const std::string& path)
{
    if (g_server.debug_port != 0)
        return !path.empty () && path[0] == '/';

    const std::string prefix = "/" + g_server.token + "/";
    return path.compare (0, prefix.size(), prefix) == 0;
}

/* The path with the token segment removed.  In token mode this is everything
 * after "/<token>/"; in --debug-port mode it is everything after the leading
 * "/", so the natural routes (/api/book, /report/<name>) work unchanged. */
std::string
route_of (const std::string& path)
{
    if (g_server.debug_port != 0)
        return path.substr (1);

    const std::string prefix = "/" + g_server.token + "/";
    return path.substr (prefix.size ());
}

/* The token rewrite_links() puts in front of every route.  --debug-port has no
 * token segment, so an empty token yields the natural paths. */
std::string
links_token ()
{
    return g_server.debug_port != 0 ? std::string () : g_server.token;
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

/* The UI page.  In --debug-port mode the server puts a visible banner at the
 * top of the body, because a URL without a token is otherwise invisible: the
 * page looks normal and any local process can drive it.  The normal page is
 * returned byte-for-byte (ADR 0013 §1). */
std::string
index_html ()
{
    std::string html = static_file ("index.html");
    if (g_server.debug_port == 0)
        return html;

    const std::string marker = "<body>";
    const std::string banner =
        "<div id=\"debug-port-banner\" role=\"alert\" "
        "style=\"background:#b00020;color:#fff;padding:.6rem 1rem;"
        "font:600 14px/1.4 system-ui,sans-serif\">"
        "Debug mode (--debug-port " + std::to_string (g_server.debug_port) +
        "): no token in the URL. Any local process can drive this UI; with "
        "--writable it can edit and save the book. Development only.</div>";
    const size_t at = html.find (marker);
    if (at == std::string::npos)
        return banner + html;
    return html.insert (at + marker.size (), banner);
}

/* The application shell: the one document every page URL serves.  The client
 * router reads the path and puts the page, the tab or the report on screen;
 * the server does not vary the document by page. */
void
serve_shell (GSocketConnection* connection)
{
    send_response (connection, "200 OK", "text/html; charset=utf-8",
                   index_html ());
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

/* ------------------------------------------------------------- write path
 *
 * ADR 0013.  Nothing here runs unless the process was started writable, and
 * every route asks `refuse_write()` before it touches the engine, so a
 * read-only process cannot save even if a route were reached by mistake.
 *
 * The server never writes the book itself: the save is qof_session_save, the
 * same call the desktop makes, so the lock and the timestamped backup are the
 * engine's, not a second implementation of them. */

std::string
json_string (const std::string& text)
{
    return "\"" + json_escape (text.c_str ()) + "\"";
}

/* The files next to the book that the XML backend's backup_file() creates:
 * <book>.<14-digit timestamp>.gnucash.  The server only *names* the backup a
 * save just produced; it does not create, copy or prune them. */
std::set<std::string>
book_backups (const std::string& book_path)
{
    std::set<std::string> found;

    gchar* dirname = g_path_get_dirname (book_path.c_str ());
    gchar* basename = g_path_get_basename (book_path.c_str ());
    const std::string prefix = std::string (basename) + ".";

    GDir* dir = g_dir_open (dirname, 0, nullptr);
    if (dir != nullptr)
    {
        const gchar* name = nullptr;
        while ((name = g_dir_read_name (dir)) != nullptr)
        {
            const std::string candidate (name);
            if (candidate.size () <= prefix.size ()
                || candidate.compare (0, prefix.size (), prefix) != 0)
                continue;
            if (!g_str_has_suffix (name, GNC_DATAFILE_EXT))
                continue;
            gchar* full = g_build_filename (dirname, name, nullptr);
            found.insert (full);
            g_free (full);
        }
        g_dir_close (dir);
    }

    g_free (dirname);
    g_free (basename);
    return found;
}

/* The backup the save just added: a name in `after` that was not in `before`.
 * If a save somehow produced more than one, the newest wins. */
std::string
new_backup (const std::set<std::string>& before,
            const std::set<std::string>& after)
{
    std::string newest;
    gint64 newest_time = 0;

    for (const std::string& path : after)
    {
        if (before.count (path) != 0)
            continue;
        GStatBuf info;
        if (g_stat (path.c_str (), &info) != 0)
            continue;
        if (newest.empty () || static_cast<gint64>(info.st_mtime) >= newest_time)
        {
            newest = path;
            newest_time = static_cast<gint64>(info.st_mtime);
        }
    }
    return newest;
}

/* The name the engine's backup_file() would use *now*:
 * <book>.<YYYYMMDDHHMMSS>.gnucash, in local time.  It calls the engine's own
 * timestamp helper rather than formatting the clock here, so the server does
 * not grow a second naming rule (ADR 0013 §3): the name is the engine's, the
 * server only uses it to recognise the same-second collision. */
std::string
backup_path_for_now (const std::string& book_path)
{
    gchar* stamp = gnc_date_timestamp ();
    std::string path = book_path + "." + stamp + GNC_DATAFILE_EXT;
    g_free (stamp);
    return path;
}

/* Sleep until the clock's second changes, with a hard cap.  The backup name
 * is per second, so this is what makes the retry name a different file.  The
 * server is single-threaded and this blocks it for up to a second; that is
 * acceptable because it only happens when a save actually collided, and it is
 * bounded (ADR 0014 §10). */
void
wait_for_next_second ()
{
    gchar* before = gnc_date_timestamp ();
    const std::string stamp (before);
    g_free (before);

    for (int tick = 0; tick < 200; ++tick)
    {
        g_usleep (10 * 1000);
        gchar* now = gnc_date_timestamp ();
        const bool changed = stamp != now;
        g_free (now);
        if (changed)
            return;
    }
}

/* Size of a regular file, or -1 when there is no regular file at that path. */
gint64
file_size (const std::string& path)
{
    GStatBuf info;
    if (g_stat (path.c_str (), &info) != 0)
        return -1;
    if (!g_file_test (path.c_str (), G_FILE_TEST_IS_REGULAR))
        return -1;
    return static_cast<gint64>(info.st_size);
}

std::string
backend_error_text (QofBackendError error)
{
    switch (error)
    {
    case ERR_BACKEND_LOCKED:
        return "the book is locked by another process";
    case ERR_BACKEND_READONLY:
        return "the book cannot be written (read-only filesystem, "
               "permissions, or anti-virus)";
    case ERR_FILEIO_BACKUP_ERROR:
        return "the engine could not make the pre-save backup";
    case ERR_FILEIO_WRITE_ERROR:
        return "the engine could not write the book";
    default:
        return "the engine reported a write error";
    }
}

/* The named refusal every write route starts with.  Returns true when it has
 * answered the request, so the caller must not touch the engine. */
void
send_json (GSocketConnection* connection, const char* status,
           const std::string& body)
{
    send_response (connection, status, "application/json", body);
}

bool
refuse_write (GSocketConnection* connection)
{
    if (g_server.writable)
        return false;

    std::string body;
    if (g_server.locked)
    {
        body = "{\"error\":\"locked\",\"message\":";
        body += json_string ("the book is locked by another GnuCash process, "
                             "so this server opened it read-only; the lock is "
                             + g_server.lock_path);
        body += ",\"lock\":";
        body += json_string (g_server.lock_path);
        body += "}";
        send_json (connection, "409 Conflict", body);
    }
    else
    {
        body = "{\"error\":\"read-only\",\"message\":";
        body += json_string ("this server was started read-only; start it "
                             "with --writable (or GNC_WEBUI_WRITABLE=1) to "
                             "enable saving");
        body += "}";
        send_json (connection, "403 Forbidden", body);
    }
    return true;
}

/* POST /api/book/save -- write the session's book with the engine's own save
 * path and report what was written.  See ADR 0013 for the policy: the backup
 * is made by the engine before the original is touched, and the route does not
 * report success for a book it cannot see afterwards. */

/* What one qof_session_save did, with no HTTP in it: the engine's error and
 * message, whether the book was dirty, whether the path still holds a
 * non-empty file, and the backup the engine's backup_file() added. */
struct SaveOutcome
{
    QofBackendError error = ERR_BACKEND_NO_ERR;
    std::string message;
    bool changed = false;
    bool verified = false;
    std::string backup;
    gint64 bytes = -1;
};

SaveOutcome
save_book (QofSession* session, QofBook* book, const std::string& path)
{
    SaveOutcome outcome;
    const std::set<std::string> backups_before = book_backups (path);
    outcome.changed = qof_book_session_not_saved (book);

    /* A failed save leaves its error on the session.  Clear it first so a
     * retry is a retry, not a replay of the previous failure. */
    qof_session_pop_error (session);

    qof_session_save (session, nullptr);

    outcome.error = qof_session_get_error (session);
    outcome.message = qof_session_get_error_message (session);
    outcome.backup = new_backup (backups_before, book_backups (path));
    outcome.bytes = file_size (path);
    outcome.verified = outcome.bytes > 0;
    return outcome;
}

/* Name the backup path and the reason when the engine refuses to make the
 * pre-save backup.  backup_file() returns false without setting a message for
 * this error, so without this the body would say only "could not make the
 * pre-save backup" and not which file blocked it. */
void
describe_backup_failure (SaveOutcome& outcome, const std::string& path,
                         bool retried)
{
    const std::string backup = backup_path_for_now (path);
    std::string reason;
    if (!outcome.message.empty ())
        reason = outcome.message;
    else if (g_file_test (backup.c_str (), G_FILE_TEST_EXISTS))
        reason = "a backup with this second's name already exists";
    else
        reason = "the engine could not create it (check that the book's "
                 "directory is writable)";
    if (retried)
        reason += "; the retry after the next second also failed";

    outcome.backup = backup;
    outcome.message = "the engine could not make the pre-save backup " +
                      backup + ": " + reason;
}

/* One save, plus the one retry the same-second backup collision needs.
 *
 * Two dirty saves inside one second collide on the engine's backup name:
 * backup_file() refuses a path that already exists, and the save fails with
 * ERR_FILEIO_BACKUP_ERROR with the book untouched.  A collision is
 * recognisable by the fact that this second's backup is already there; a
 * genuinely failing backup (unwritable directory, full disk) leaves the name
 * free.  On a collision, wait for the next second and retry once, because the
 * second attempt then names a different file.  The retry is not a silent
 * success: if it fails too, describe_backup_failure() names the backup path
 * and the reason (ADR 0014 §10). */
SaveOutcome
save_book_with_retry (QofSession* session, QofBook* book,
                      const std::string& path, bool& retried)
{
    retried = false;
    SaveOutcome outcome = save_book (session, book, path);
    if (outcome.error != ERR_FILEIO_BACKUP_ERROR)
        return outcome;

    /* A taken name is only a collision if the next second's name can be
     * created: an unwritable book directory leaves the expected name free too,
     * and must fail at once rather than pause for a second first. */
    const std::string collided = backup_path_for_now (path);
    gchar* dirname = g_path_get_dirname (path.c_str ());
    const bool can_retry = g_access (dirname, W_OK) == 0;
    g_free (dirname);
    if (!can_retry || !g_file_test (collided.c_str (), G_FILE_TEST_EXISTS))
        return outcome;

    retried = true;
    std::fprintf (stderr,
                  "webd: the pre-save backup %s already exists; waiting for "
                  "the next second and retrying the save once\n",
                  collided.c_str ());
    wait_for_next_second ();
    return save_book (session, book, path);
}

void
send_save_lost (GSocketConnection* connection, const SaveOutcome& outcome,
                const std::string& path)
{
    /* The engine said the save worked but the book is not there.  This is the
     * unlink-then-link window in the XML backend; the backup named here is the
     * complete pre-save copy. */
    std::string body = "{\"error\":\"write-lost\",\"message\":";
    body += json_string ("the engine reported a successful save but " + path +
                         " is missing or empty; the pre-save copy is " +
                         (outcome.backup.empty () ? "not next to the book"
                                                  : outcome.backup));
    body += ",\"path\":" + json_string (path);
    body += ",\"backup\":";
    body += outcome.backup.empty () ? "null" : json_string (outcome.backup);
    body += "}";
    send_json (connection, "500 Internal Server Error", body);
}

void
send_save_failure (GSocketConnection* connection, const SaveOutcome& outcome,
                   const std::string& path)
{
    const bool locked = outcome.error == ERR_BACKEND_LOCKED;
    std::string body = "{\"error\":";
    body += json_string (locked ? "locked"
                        : outcome.error == ERR_FILEIO_BACKUP_ERROR
                            ? "backup-failed"
                            : "save-failed");
    body += ",\"message\":";
    body += json_string (outcome.message.empty ()
                             ? backend_error_text (outcome.error) + ": " + path
                             : outcome.message);
    body += ",\"engine_error\":";
    body += std::to_string (static_cast<int>(outcome.error));
    body += ",\"path\":" + json_string (path);
    body += ",\"path_exists\":";
    body += outcome.bytes > 0 ? "true" : "false";
    body += ",\"backup\":";
    body += outcome.backup.empty () ? "null" : json_string (outcome.backup);
    body += "}";
    send_json (connection,
               locked ? "409 Conflict" : "500 Internal Server Error", body);
}

void
send_save_success (GSocketConnection* connection, const SaveOutcome& outcome,
                   const std::string& path, bool retried)
{
    std::string body = "{\"saved\":true,\"changed\":";
    body += outcome.changed ? "true" : "false";
    if (retried)
        body += ",\"retried\":true";
    body += ",\"path\":" + json_string (path);
    body += ",\"bytes\":" + std::to_string (outcome.bytes);
    body += ",\"backup\":";
    body += outcome.backup.empty () ? "null" : json_string (outcome.backup);
    if (!outcome.backup.empty ())
        body += ",\"backup_bytes\":" +
                std::to_string (file_size (outcome.backup));
    if (outcome.changed && outcome.backup.empty ())
        body += ",\"warning\":\"the save wrote the book but no backup file "
                "was found next to it\"";
    body += "}";
    send_json (connection, "200 OK", body);
}

void
handle_book_save (GSocketConnection* connection, const HttpRequest& request)
{
    if (request.method != "POST")
    {
        send_json (connection, "405 Method Not Allowed",
                   "{\"error\":\"method\",\"message\":\"saving needs POST\"}");
        return;
    }
    if (refuse_write (connection))
        return;

    const std::string path = g_server.book_path;
    bool retried = false;
    SaveOutcome outcome = save_book_with_retry (g_server.session, g_server.book,
                                               path, retried);

    if (outcome.error != ERR_BACKEND_NO_ERR)
    {
        if (outcome.error == ERR_FILEIO_BACKUP_ERROR)
            describe_backup_failure (outcome, path, retried);
        send_save_failure (connection, outcome, path);
    }
    else if (!outcome.verified)
        send_save_lost (connection, outcome, path);
    else
        send_save_success (connection, outcome, path, retried);
}

/* --------------------------------------------------------- split editing
 *
 * ADR 0014.  The three editable fields are strings and are set with the
 * engine's own setters -- never by writing XML.  None of them can change an
 * amount, so an edit cannot move a balance: `description` and `num` belong to
 * the transaction (so every row of it in every account changes) and `memo`
 * belongs to the split.
 *
 * `amount` is not accepted here.  xaccSplitSetAmount() maintains the
 * transaction's balance by rewriting the *other* splits (Split.h:236-240), so
 * one cell edit could change rows in other accounts; this slice has no way to
 * show the user which rows moved, and a "safe" subset (two same-commodity
 * splits) would be a second rule the register cannot explain.  The body is
 * checked before anything is written, so a bad field changes nothing. */

constexpr size_t max_edit_text = 2048;

struct SplitEdit
{
    bool has_memo = false;
    bool has_description = false;
    bool has_num = false;
    std::string memo;
    std::string description;
    std::string num;
};

const char*
json_type_name (const web::json::Value& value)
{
    switch (value.type)
    {
    case web::json::Type::Null: return "null";
    case web::json::Type::Bool: return "a boolean";
    case web::json::Type::Number: return "a number";
    case web::json::Type::String: return "a string";
    case web::json::Type::Array: return "an array";
    case web::json::Type::Object: return "an object";
    }
    return "unknown";
}

void
send_edit_refusal (GSocketConnection* connection, const char* status,
                   const char* slug, const std::string& message,
                   const std::string& field = "")
{
    std::string body = "{\"error\":" + json_string (slug);
    body += ",\"message\":" + json_string (message);
    if (!field.empty ())
        body += ",\"field\":" + json_string (field);
    body += "}";
    send_json (connection, status, body);
}

/* One string field: a string, short enough, valid UTF-8, no control
 * characters.  Anything else is a named refusal, never a silent trim. */
bool
edit_text_field (const web::json::Value& value, const char* field,
                 std::string& out, std::string& slug, std::string& message)
{
    const std::string name (field);
    if (!value.is_string ())
    {
        slug = "bad-type";
        message = "field '" + name + "' must be a string, not " +
                  json_type_name (value) + ".";
        return false;
    }
    if (value.string.size () > max_edit_text)
    {
        slug = "too-long";
        message = "field '" + name + "' is " +
                  std::to_string (value.string.size ()) + " bytes; the limit is " +
                  std::to_string (max_edit_text) + ".";
        return false;
    }
    for (const unsigned char byte : value.string)
    {
        if (byte < 0x20 || byte == 0x7f)
        {
            slug = "bad-value";
            message = "field '" + name + "' contains a control character; the "
                      "register stores plain text.";
            return false;
        }
    }
    if (!g_utf8_validate (value.string.c_str (), -1, nullptr))
    {
        slug = "bad-value";
        message = "field '" + name + "' is not valid UTF-8.";
        return false;
    }
    out = value.string;
    return true;
}

/* The row a split shows up as, recomputed the way the register computes it, so
 * the edit response is the row the page will draw and not a second shape. */
bool
register_row_for_split (Split* split, RegisterRow& row)
{
    Account* account = xaccSplitGetAccount (split);
    if (account == nullptr)
        return false;

    const std::string guid = guid_text (xaccSplitGetGUID (split));
    for (const RegisterRow& candidate : register_rows (account))
    {
        if (candidate.guid == guid)
        {
            row = candidate;
            return true;
        }
    }
    return false;
}

/* The parse-and-validate stage of an edit, with no HTTP side effects: either
 * it fills `edit`, or it fills `refusal` and the caller answers with it.  The
 * whole body is checked before the engine is touched, so a refused request
 * cannot apply half of itself. */
struct EditRefusal
{
    const char* status = "400 Bad Request";
    std::string slug;
    std::string message;
    std::string field;
};

bool
parse_split_edit (const std::string& body, SplitEdit& edit, EditRefusal& refusal)
{
    web::json::Value object;
    std::string parse_error;
    if (!web::json::parse (body, object, parse_error))
    {
        refusal = {"400 Bad Request", "bad-json",
                   "the body is not valid JSON: " + parse_error, ""};
        return false;
    }
    if (!object.is_object ())
    {
        refusal = {"400 Bad Request", "bad-body",
                   "the body must be a JSON object of the fields being "
                   "changed, not " + std::string (json_type_name (object)), ""};
        return false;
    }
    if (object.object.empty ())
    {
        refusal = {"400 Bad Request", "no-fields",
                   "the body names no field to change; the editable fields are "
                   "description, memo and num", ""};
        return false;
    }

    for (const auto& [name, value] : object.object)
    {
        if (name == "amount")
        {
            refusal = {"400 Bad Request", "unsupported-field",
                       "the amount is not editable in this stage: the engine "
                       "rebalances the transaction's other splits, which this "
                       "register cannot show yet; edit the transaction in the "
                       "desktop", name};
            return false;
        }

        bool* has = nullptr;
        std::string* target = nullptr;
        if (name == "memo")
        {
            has = &edit.has_memo;
            target = &edit.memo;
        }
        else if (name == "description")
        {
            has = &edit.has_description;
            target = &edit.description;
        }
        else if (name == "num")
        {
            has = &edit.has_num;
            target = &edit.num;
        }
        else
        {
            refusal = {"400 Bad Request", "unknown-field",
                       "unknown field '" + name + "'; the editable fields are "
                       "description, memo and num", name};
            return false;
        }

        if (!edit_text_field (value, name.c_str (), *target, refusal.slug,
                              refusal.message))
        {
            refusal.status = "400 Bad Request";
            refusal.field = name;
            return false;
        }
        *has = true;
    }
    return true;
}

/* POST /api/split/<guid> with a JSON object of the fields being changed.
 * Answers with the updated row (the same object the register's `splits` array
 * holds), or a named 4xx/5xx. */
void
handle_split_edit (GSocketConnection* connection, const HttpRequest& request,
                   const std::string& spec)
{
    if (request.method != "POST")
    {
        send_json (connection, "405 Method Not Allowed",
                   "{\"error\":\"method\",\"message\":\"editing a split needs "
                   "POST\"}");
        return;
    }
    if (refuse_write (connection))
        return;

    /* The second line of defence (ADR 0013 §1): a book this process marked
     * read-only refuses the edit even if a gate were forgotten. */
    if (qof_book_is_readonly (g_server.book))
    {
        send_edit_refusal (connection, "403 Forbidden", "book-readonly",
                           "the book is marked read-only in this process, so "
                           "it cannot be edited");
        return;
    }

    GncGUID guid;
    Split* split = nullptr;
    if (string_to_guid (spec.c_str (), &guid))
        split = xaccSplitLookup (&guid, gnc_account_get_book (g_server.root));
    if (split == nullptr)
    {
        send_edit_refusal (connection, "400 Bad Request", "unknown-split",
                           "no such split: " + spec);
        return;
    }

    if (request.body_too_large)
    {
        send_edit_refusal (connection, "413 Payload Too Large", "body-too-large",
                           "the edit body is larger than 1 MiB");
        return;
    }

    SplitEdit edit;
    EditRefusal refusal;
    if (!parse_split_edit (request.body, edit, refusal))
    {
        send_edit_refusal (connection, refusal.status, refusal.slug.c_str (),
                           refusal.message, refusal.field);
        return;
    }

    Transaction* transaction = xaccSplitGetParent (split);
    const gchar* current_description = xaccTransGetDescription (transaction);
    const gchar* current_num = xaccTransGetNum (transaction);
    const gchar* current_memo = xaccSplitGetMemo (split);

    const bool changed =
        (edit.has_description
         && edit.description != (current_description != nullptr
                                     ? current_description : ""))
        || (edit.has_num
            && edit.num != (current_num != nullptr ? current_num : ""))
        || (edit.has_memo
            && edit.memo != (current_memo != nullptr ? current_memo : ""));

    /* An edit that sets the value it already had is not a change: the book is
     * left clean, so a later Save can honestly answer changed:false. */
    if (changed)
    {
        xaccTransBeginEdit (transaction);
        if (edit.has_memo)
            xaccSplitSetMemo (split, edit.memo.c_str ());
        if (edit.has_description)
            xaccTransSetDescription (transaction, edit.description.c_str ());
        if (edit.has_num)
            xaccTransSetNum (transaction, edit.num.c_str ());
        xaccTransCommitEdit (transaction);
    }

    RegisterRow row;
    if (!register_row_for_split (split, row))
    {
        send_edit_refusal (connection, "500 Internal Server Error",
                           "row-unavailable",
                           "the split was edited but its register row could not "
                           "be recomputed");
        return;
    }

    send_json (connection, "200 OK", register_row_json (row));
}

/* Accounts the self-test may use: real accounts (not placeholders, not the
 * root) that carry a commodity. */
void
collect_usable_accounts (Account* parent, std::vector<Account*>& out)
{
    GList* children = gnc_account_get_children (parent);
    for (GList* node = children; node != nullptr; node = node->next)
    {
        auto* child = static_cast<Account*>(node->data);
        if (!xaccAccountGetPlaceholder (child)
            && xaccAccountGetCommodity (child) != nullptr
            && xaccAccountGetType (child) != ACCT_TYPE_ROOT)
            out.push_back (child);
        collect_usable_accounts (child, out);
    }
    g_list_free (children);
}

/* The first pair of usable accounts that share a commodity. */
bool
find_transfer_accounts (Account* root, Account*& from, Account*& to)
{
    std::vector<Account*> accounts;
    collect_usable_accounts (root, accounts);

    from = nullptr;
    to = nullptr;
    for (size_t i = 0; i < accounts.size () && from == nullptr; ++i)
    {
        for (size_t j = i + 1; j < accounts.size (); ++j)
        {
            gnc_commodity* left = xaccAccountGetCommodity (accounts[i]);
            gnc_commodity* right = xaccAccountGetCommodity (accounts[j]);
            if (left != nullptr && gnc_commodity_equal (left, right))
            {
                from = accounts[i];
                to = accounts[j];
                break;
            }
        }
    }
    return from != nullptr;
}

/* One balanced two-split transaction of one unit of the accounts' commodity,
 * through the same begin/commit path the register will use. */
Transaction*
create_transfer (QofBook* book, Account* from, Account* to,
                 const char* description)
{
    gnc_commodity* commodity = xaccAccountGetCommodity (from);
    const gint64 fraction = gnc_commodity_get_fraction (commodity);
    const gnc_numeric amount = gnc_numeric_create (fraction, fraction);

    Transaction* transaction = xaccMallocTransaction (book);
    xaccTransBeginEdit (transaction);
    xaccTransSetCurrency (transaction, commodity);
    xaccTransSetDatePostedSecsNormalized (transaction, gnc_time (nullptr));
    xaccTransSetDescription (transaction, description);

    Split* debit = xaccMallocSplit (book);
    xaccSplitSetParent (debit, transaction);
    xaccSplitSetAccount (debit, from);
    xaccSplitSetValue (debit, amount);
    xaccSplitSetAmount (debit, amount);

    Split* credit = xaccMallocSplit (book);
    xaccSplitSetParent (credit, transaction);
    xaccSplitSetAccount (credit, to);
    xaccSplitSetValue (credit, gnc_numeric_neg (amount));
    xaccSplitSetAmount (credit, gnc_numeric_neg (amount));

    xaccTransCommitEdit (transaction);
    return transaction;
}

/* The smallest honest proof that the write path works before a register
 * exists: one balanced two-split transaction through the engine.  It is
 * stage-1 scaffolding; the register API replaces it. */
void
handle_book_selftest (GSocketConnection* connection, const HttpRequest& request)
{
    if (request.method != "POST")
    {
        send_json (connection, "405 Method Not Allowed",
                   "{\"error\":\"method\",\"message\":\"the self-test needs POST\"}");
        return;
    }
    if (refuse_write (connection))
        return;

    Account* from = nullptr;
    Account* to = nullptr;
    if (!find_transfer_accounts (g_server.root, from, to))
    {
        send_json (connection, "409 Conflict",
                   "{\"error\":\"no-accounts\",\"message\":\"the book has "
                   "no two non-placeholder accounts sharing a commodity, "
                   "so the self-test created nothing\"}");
        return;
    }

    gnc_commodity* commodity = xaccAccountGetCommodity (from);
    const char* description = "gnc-webd write-path selftest";
    Transaction* transaction = create_transfer (
        gnc_account_get_book (g_server.root), from, to, description);

    gchar* transaction_guid = guid_to_string (xaccTransGetGUID (transaction));
    gchar* from_guid = guid_to_string (xaccAccountGetGUID (from));
    gchar* to_guid = guid_to_string (xaccAccountGetGUID (to));

    std::string body = "{\"created\":true,\"guid\":";
    body += json_string (transaction_guid);
    body += ",\"description\":" + json_string (description);
    body += ",\"from\":{\"account\":";
    body += json_string (xaccAccountGetName (from));
    body += ",\"guid\":" + json_string (from_guid) + "}";
    body += ",\"to\":{\"account\":";
    body += json_string (xaccAccountGetName (to));
    body += ",\"guid\":" + json_string (to_guid) + "}";
    body += ",\"commodity\":";
    body += json_string (gnc_commodity_get_mnemonic (commodity));
    body += "}";

    g_free (transaction_guid);
    g_free (from_guid);
    g_free (to_guid);

    send_json (connection, "200 OK", body);
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

    std::string route = route_of (path);
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
    else if (route == "api/book/save")
    {
        handle_book_save (connection, request);
    }
    else if (route == "api/session")
    {
        /* What a page needs before it opens a register: the mode this process
         * was started in, why writes are refused when they are, and the book.
         * A new route rather than new fields on /api/book, because ADR 0013
         * keeps the read-only bodies of the existing routes byte-identical. */
        std::string reason;
        const bool editable = register_editable (reason);
        std::string body = "{\"writable\":";
        body += editable ? "true" : "false";
        body += ",\"locked\":";
        body += g_server.locked ? "true" : "false";
        body += ",\"lock\":";
        body += g_server.locked ? json_string (g_server.lock_path) : "null";
        body += ",\"book\":";
        body += json_string (g_server.book_path);
        body += ",\"edit_reason\":";
        body += editable ? "\"\"" : json_string (reason);
        /* Only in --debug-port mode, so the normal body stays byte-identical
         * (ADR 0013 §1).  The UI uses it to say the token is gone. */
        if (g_server.debug_port != 0)
            body += ",\"debug_port\":" + std::to_string (g_server.debug_port);
        body += "}";
        send_json (connection, "200 OK", body);
    }
    else if (route == "api/book/selftest")
    {
        handle_book_selftest (connection, request);
    }
    else if (route == "api/accounts")
    {
        send_response (connection, "200 OK", "application/json",
                       account_json (g_server.root));
    }
    else if (route == "accounts" || route == "accounts/")
    {
        /* The Book page, Accounts tab.  Every page route serves the same
         * shell: the client router reads the path and puts the tab, the
         * report list or the open report on screen.  A reader who navigates
         * here directly, reloads, or walks back to it gets the page they were
         * on, which a fragment-only scheme could not promise. */
        serve_shell (connection);
    }
    else if (route == "reports" || route == "reports/")
    {
        /* The Reports page: the report list, the option form, the designer
         * and the frame.  The desktop has report pages as tabs of the main
         * window; the separate page is a web divergence recorded in
         * doc/web-ui-pages.md. */
        serve_shell (connection);
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
                                          links_token ()));
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
        /* Two resources on one path.  A reader who navigated here wants the
         * Book page with this account's register tab; the report frame's
         * drill-down link (Sec-Fetch-Dest: iframe) and every harness that
         * reads the route want the standalone register document, which is
         * what this path has always served.  The document is unchanged. */
        if (request.page_navigation)
        {
            serve_shell (connection);
            return TRUE;
        }

        /* The drill-down target: an account's register, rendered here so a
         * link inside a report frame can simply navigate to it. */
        const std::string spec = account_spec (url_decode (route.substr (9)));
        Account* account = lookup_account (spec);

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
        const std::string spec = account_spec (url_decode (route.substr (13)));
        Account* account = lookup_account (spec);

        if (account == nullptr)
        {
            /* A register for an account that does not exist is a bad request,
             * not a missing page, and it names the GUID that was asked for. */
            std::string body = "{\"error\":\"unknown-account\",\"message\":";
            body += "\"no such account: " + json_escape (spec.c_str ()) + "\"}";
            send_json (connection, "400 Bad Request", body);
            return TRUE;
        }

        send_response (connection, "200 OK", "application/json",
                       register_json (account));
    }
    else if (route.compare (0, 16, "api/transaction/") == 0)
    {
        /* The read-only split panel behind a register row: every split of one
         * transaction, looked up by the GUID the register row carries. */
        const std::string spec = url_decode (route.substr (16));

        GncGUID guid;
        Transaction* transaction = nullptr;
        if (string_to_guid (spec.c_str (), &guid))
            transaction = xaccTransLookup (
                &guid, gnc_account_get_book (g_server.root));

        if (transaction == nullptr)
        {
            std::string body = "{\"error\":\"unknown-transaction\",\"message\":";
            body += "\"no such transaction: " + json_escape (spec.c_str ()) + "\"}";
            send_json (connection, "400 Bad Request", body);
            return TRUE;
        }

        send_response (connection, "200 OK", "application/json",
                       transaction_json (transaction));
    }
    else if (route.compare (0, 10, "api/split/") == 0)
    {
        /* The edit route ADR 0014 adds: one split at a time, a JSON object of
         * the fields being changed, the updated row back. */
        handle_split_edit (connection, request, url_decode (route.substr (10)));
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
                if (!render_spec_page (spec, html, error))
                    send_response (connection, "400 Bad Request", "text/plain",
                                   error + "\n");
                else
                    send_response (connection, "200 OK",
                                   "text/html; charset=utf-8",
                                   rewrite_links (html, links_token ()));
            }
        }
    }
    else if (route == "api/specs")
    {
        if (request.method == "GET")
        {
            /* ?tag=<slug> keeps the reports carrying that tag; ?q=<text>
             * keeps the ones whose name, description or tag contains it
             * (case-insensitive).  Both compose, an unknown tag is an empty
             * list rather than an error, and both the saved and the built-in
             * specs are filtered. */
            std::vector<QueryParam> params;
            std::string error;
            std::string tag;
            std::string text;
            bool bad_query = !parse_query (query, params, error);
            for (size_t index = 0; !bad_query && index < params.size ();
                 ++index)
            {
                if (params[index].name == "tag")
                    tag = params[index].value;
                else if (params[index].name == "q")
                    text = params[index].value;
                else
                {
                    error = "unknown query parameter '" +
                            params[index].name + "'";
                    bad_query = true;
                }
            }

            std::vector<web::ReportSpec> specs;
            if (bad_query)
                send_response (connection, "400 Bad Request", "text/plain",
                               error + "\n");
            else if (!web::load_specs (specs, error))
                send_response (connection, "500 Internal Server Error",
                               "text/plain", error + "\n");
            else
                send_response (connection, "200 OK", "application/json",
                               web::specs_to_json (specs, tag, text));
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
        /* The same two-resource path as report/: a reader navigating here
         * gets the Reports page with this saved report open; the designer's
         * preview frame and the harnesses get the rendered document. */
        if (request.page_navigation)
        {
            serve_shell (connection);
            return TRUE;
        }

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
            if (!render_spec_page (spec, html, error))
                send_response (connection, "500 Internal Server Error",
                               "text/plain", error + "\n");
            else
                send_response (connection, "200 OK",
                               "text/html; charset=utf-8",
                               rewrite_links (html, links_token ()));
        }
    }
    else if (route.compare (0, 11, "api/report/") == 0)
    {
        /* /api/report/<name>/options -- the option descriptors as JSON.
         * /api/report/<name>/designer -- where the C++ implementation lives,
         * and the starting-point spec when the designer can honestly offer
         * one.  A native report always answers; a name the native layer does
         * not know is a 404, because there is nothing to invent. */
        const std::string rest = route.substr (11);
        const std::string options_suffix = "/options";
        const std::string designer_suffix = "/designer";
        const bool has_options =
            rest.size () > options_suffix.size () &&
            rest.compare (rest.size () - options_suffix.size (),
                          options_suffix.size (), options_suffix) == 0;
        const bool has_designer =
            rest.size () > designer_suffix.size () &&
            rest.compare (rest.size () - designer_suffix.size (),
                          designer_suffix.size (), designer_suffix) == 0;
        if (!has_options && !has_designer)
        {
            send_response (connection, "404 Not Found", "text/plain",
                           "not found\n");
        }
        else
        {
            const std::string suffix =
                has_options ? options_suffix : designer_suffix;
            const std::string name =
                url_decode (rest.substr (0, rest.size () - suffix.size ()));
            if (gnc::report::report_options (name).empty ())
                send_response (connection, "404 Not Found", "text/plain",
                               (has_options ? "no options for '"
                                            : "no native report '") +
                                   name + "'\n");
            else if (has_options)
                send_response (connection, "200 OK", "application/json",
                               report_options_json (name));
            else
            {
                web::NativeDesignerInfo info;
                if (!web::native_designer_info (name, info))
                    send_response (connection, "404 Not Found", "text/plain",
                                   "no designer description for '" + name +
                                       "'\n");
                else
                    send_response (connection, "200 OK", "application/json",
                                   web::native_designer_json (info));
            }
        }
    }
    else if (route.compare (0, 7, "report/") == 0)
    {
        /* A reader who navigated here gets the Reports page with this report
         * open; the page's own frame (Sec-Fetch-Dest: iframe) and the
         * harnesses get the rendered report document on the same path.  This
         * is the one path where the page URL and the frame's document URL are
         * the same, so the distinction is the request's fetch metadata, not a
         * second route: /report/<name> must keep meaning "the report
         * document" to every existing caller.  The captured value is the
         * HttpRequest's, read before the report request below shadows the
         * name. */
        const bool page_navigation = request.page_navigation;
        if (page_navigation)
        {
            serve_shell (connection);
            return TRUE;
        }

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
                           rewrite_links (native_html, links_token ()));
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
                    html != nullptr ? html : "", links_token ());
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
        serve_shell (connection);
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

/* --debug-port [port] -- the fixed, tokenless development URL (ADR 0012).
 * The value is optional: bare --debug-port means 8984.  A value is a port or
 * "host:port"; whatever it says, only loopback is accepted (127.0.0.1 or
 * localhost), because the flag removes the token and must not widen the
 * socket.  A bad value is refused by name before the engine starts, so it
 * cannot take the book's lock and then fail. */
constexpr int default_debug_port = 8984;

/* True when the argument is a port or a host:port (the forms --debug-port
 * accepts).  Used to tell a bare flag from a following book path. */
bool
looks_like_debug_port (const char* text)
{
    if (text == nullptr || text[0] == '\0')
        return false;
    const char* colon = strrchr (text, ':');
    const char* digits = colon != nullptr ? colon + 1 : text;
    if (digits[0] == '\0')
        return false;
    for (const char* p = digits; *p != '\0'; ++p)
    {
        if (*p < '0' || *p > '9')
            return false;
    }
    return true;
}

bool
parse_debug_port (const char* text, int& port, std::string& error)
{
    if (text == nullptr)
    {
        port = default_debug_port;
        return true;
    }

    std::string spec (text);
    std::string host;
    const size_t colon = spec.rfind (':');
    if (colon != std::string::npos)
    {
        host = spec.substr (0, colon);
        spec = spec.substr (colon + 1);
    }

    if (!host.empty () && host != "127.0.0.1"
        && g_ascii_strcasecmp (host.c_str (), "localhost") != 0)
    {
        error = "--debug-port refuses to bind " + host +
                ": it is loopback only (127.0.0.1 or localhost)";
        return false;
    }

    bool digits = !spec.empty () && spec.size () <= 5;
    for (const char character : spec)
    {
        if (character < '0' || character > '9')
            digits = false;
    }
    if (!digits)
    {
        error = "--debug-port needs a port between 1 and 65535, not '" +
                std::string (text) + "'";
        return false;
    }

    const long value = std::strtol (spec.c_str (), nullptr, 10);
    if (value < 1 || value > 65535)
    {
        error = "--debug-port needs a port between 1 and 65535, not '" +
                spec + "'";
        return false;
    }

    port = static_cast<int> (value);
    return true;
}

struct Arguments
{
    const char* backend = nullptr;
    const char* book = nullptr;
    bool writable = false;
    bool break_lock = false;
    int debug_port = 0; /* 0 = no --debug-port (the normal token mode) */
};

GMainLoop* g_loop = nullptr;

gboolean
quit_on_signal (gpointer)
{
    if (g_loop != nullptr)
        g_main_loop_quit (g_loop);
    return G_SOURCE_REMOVE;
}

#ifdef G_OS_WIN32
void
quit_on_signal_win (int)
{
    if (g_loop != nullptr)
        g_main_loop_quit (g_loop);
}
#endif

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

    /* ADR 0013: writable is a mode of the process, chosen here and nowhere
     * else.  The default is SESSION_READ_ONLY, exactly as before. */
    bool writable = args->writable;
    const gchar* writable_env = g_getenv ("GNC_WEBUI_WRITABLE");
    if (writable_env != nullptr && writable_env[0] != '\0'
        && g_strcmp0 (writable_env, "0") != 0)
        writable = true;

    if (writable && !gnc_uri_targets_local_fs (args->book))
    {
        std::fprintf (stderr,
                      "webd: --writable supports local book files only, not "
                      "%s: the lock and backup guarantees are the file "
                      "backend's (ADR 0013)\n", args->book);
        exit (1);
    }

    SessionOpenMode mode = SESSION_READ_ONLY;
    if (writable)
        mode = args->break_lock ? SESSION_BREAK_LOCK : SESSION_NORMAL_OPEN;

    qof_session_begin (session, args->book, mode);
    QofBackendError open_error = qof_session_get_error (session);

    if (writable && open_error == ERR_BACKEND_LOCKED)
    {
        /* No dialog in a server, so take the branch the desktop offers as
         * "Open Read-Only" and say which lock blocked the write. */
        g_server.locked = true;
        const char* opened = qof_session_get_file_path (session);
        gchar* lock_path = gnc_uri_get_path (args->book);
        g_server.lock_path = opened != nullptr ? std::string (opened)
                            : lock_path != nullptr ? std::string (lock_path)
                                                   : std::string (args->book);
        g_server.lock_path += ".LCK";
        g_free (lock_path);

        std::fprintf (stderr,
                      "webd: %s is locked (%s); opening it read-only, so "
                      "saves will be refused with 409\n",
                      args->book, g_server.lock_path.c_str ());

        qof_session_begin (session, args->book, SESSION_READ_ONLY);
        open_error = qof_session_get_error (session);
    }

    if (open_error != ERR_BACKEND_NO_ERR)
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
    g_server.session = session;
    g_server.book = book;
    g_server.writable = writable && !g_server.locked;

    /* The route gate is the first line of defence; this is the second.  The
     * engine's own save paths check the book's read-only flag
     * (gnc-xml-backend.cpp:315, :364), so a future route that forgot its gate
     * still cannot write a book this process opened read-only.  The desktop
     * does the same after "Open Read-Only" (gnc-file.c:1045-1050). */
    if (!g_server.writable)
        qof_book_mark_readonly (book);

    const char* opened_path = qof_session_get_file_path (session);
    if (opened_path != nullptr)
        g_server.book_path = opened_path;
    else
    {
        gchar* uri_path = gnc_uri_get_path (args->book);
        g_server.book_path = uri_path != nullptr ? uri_path : args->book;
        g_free (uri_path);
    }

    g_server.root = gnc_book_get_root_account (book);
    g_server.accounts = g_server.root ? gnc_account_n_descendants (g_server.root) : 0;
    g_server.top_level = g_server.root
        ? static_cast<int> (g_list_length (gnc_account_get_children (g_server.root)))
        : 0;
    g_server.token = random_token ();
    g_server.debug_port = args->debug_port;

    GSocketService* service = g_socket_service_new ();
    GInetAddress* loopback = g_inet_address_new_loopback (G_SOCKET_FAMILY_IPV4);
    /* --debug-port asks for a fixed port; the normal mode asks for 0 and lets
     * the kernel pick one.  Both bind the loopback address, which is the only
     * address this server has ever bound or will bind. */
    const guint16 requested_port =
        args->debug_port != 0 ? static_cast<guint16> (args->debug_port) : 0;
    GSocketAddress* address = g_inet_socket_address_new (loopback,
                                                         requested_port);
    GError* error = nullptr;
    guint16 port = 0;

    /* Bind a socket ourselves so SO_REUSEADDR can be set (g_socket_bind's
     * allow_reuse): a fixed --debug-port must be rebindable straight after a
     * restart, even while the previous run's accepted connections are in
     * TIME_WAIT.  It does not let a second server take a port that is really
     * listening -- that still fails with G_IO_ERROR_ADDRESS_IN_USE. */
    GSocket* listener = g_socket_new (G_SOCKET_FAMILY_IPV4,
                                      G_SOCKET_TYPE_STREAM,
                                      G_SOCKET_PROTOCOL_TCP, &error);
    bool listening = false;
    if (listener != nullptr)
    {
        listening = g_socket_bind (listener, address, TRUE, &error)
                    && g_socket_listen (listener, &error)
                    && g_socket_listener_add_socket (
                        G_SOCKET_LISTENER (service), listener, nullptr, &error);
    }

    if (!listening)
    {
        if (args->debug_port != 0)
        {
            /* A stable URL that quietly is not stable is worse than a
             * refusal: never fall back to an ephemeral port here. */
            if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_ADDRESS_IN_USE))
            {
                std::fprintf (stderr,
                              "webd: --debug-port %d is already in use on "
                              "127.0.0.1; refusing to fall back to an "
                              "ephemeral port\n", args->debug_port);
            }
            else
            {
                std::fprintf (stderr,
                              "webd: cannot listen on 127.0.0.1:%d: %s\n",
                              args->debug_port,
                              error != nullptr ? error->message : "unknown");
            }
        }
        else
        {
            std::fprintf (stderr, "webd: cannot listen: %s\n",
                          error != nullptr ? error->message : "unknown");
        }
        if (error != nullptr)
            g_error_free (error);
        if (listener != nullptr)
            g_object_unref (listener);
        /* Do not leave the book's .LCK behind when the bind is what failed. */
        qof_session_end (session);
        exit (1);
    }

    GSocketAddress* effective = g_socket_get_local_address (listener, nullptr);
    if (effective != nullptr)
    {
        port = g_inet_socket_address_get_port (
            G_INET_SOCKET_ADDRESS (effective));
        g_object_unref (effective);
    }
    g_object_unref (listener);
    g_object_unref (address);
    g_object_unref (loopback);

    g_signal_connect (service, "incoming", G_CALLBACK (on_incoming), nullptr);
    g_socket_service_start (service);

    if (args->debug_port != 0)
    {
        /* The natural paths, no token segment: a stable development URL.  It
         * is loud about what that means, both here and in the page banner. */
        std::printf ("webd listening on http://127.0.0.1:%u/\n", port);
        std::fprintf (stderr,
                      "webd: WARNING: --debug-port %u serves without a token: "
                      "any local process can drive this UI%s. Development "
                      "only; not how to run the app (ADR 0012).\n",
                      port, writable ? "; with --writable it can edit and "
                                       "save the book" : "");
        if (writable)
        {
            std::fprintf (stderr,
                          "webd: WARNING: --writable with --debug-port %u: any "
                          "local process can edit and save %s\n",
                          port, g_server.book_path.c_str ());
        }
    }
    else
    {
        std::printf ("webd listening on http://127.0.0.1:%u/%s/\n", port,
                     g_server.token.c_str ());
    }
    std::fflush (stdout);

    if (writable)
    {
        const std::string mode =
            g_server.locked
                ? "read-only (locked: " + g_server.lock_path + ")"
                : "writable";
        std::printf ("webd mode: %s\n", mode.c_str ());
        std::fflush (stdout);
    }

    g_loop = g_main_loop_new (nullptr, FALSE);
#ifndef G_OS_WIN32
    g_unix_signal_add (SIGINT, quit_on_signal, nullptr);
    g_unix_signal_add (SIGTERM, quit_on_signal, nullptr);
#else
    std::signal (SIGINT, quit_on_signal_win);
    std::signal (SIGTERM, quit_on_signal_win);
#endif
    g_main_loop_run (g_loop);

    /* An ordinary Ctrl-C must release the .LCK, or every stop would leave a
     * lock that refuses the next writable run. */
    qof_session_end (session);
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
        else if (g_strcmp0 (argv[i], "--writable") == 0)
            args.writable = true;
        else if (g_strcmp0 (argv[i], "--break-lock") == 0)
            args.break_lock = true;
        else if (g_strcmp0 (argv[i], "--debug-port") == 0)
        {
            /* The value is optional: consume the next argument only when it
             * looks like a port (or host:port), never a book path. */
            const char* value = nullptr;
            if (i + 1 < argc && looks_like_debug_port (argv[i + 1]))
                value = argv[++i];
            std::string error;
            if (!parse_debug_port (value, args.debug_port, error))
            {
                std::fprintf (stderr, "webd: %s\n", error.c_str ());
                return 2;
            }
        }
        else if (args.book == nullptr)
            args.book = argv[i];
    }

    if (args.book == nullptr)
    {
        std::fprintf (stderr,
                      "usage: %s [--backend <backend-module>] [--writable "
                      "[--break-lock]] [--debug-port [port]] <book>\n",
                      argv[0]);
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
