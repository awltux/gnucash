/********************************************************************\
 * gnc-report.cpp -- native report layer (no Guile)                 *
 *                                                                  *
 * The first native report is the Trial Balance, chosen because its *
 * output is simple enough to compare cell by cell with the Scheme   *
 * report it replaces.                                              *
 *                                                                  *
 * This program is free software; you can redistribute it and/or    *
 * modify it under the terms of the GNU General Public License as   *
 * published by the Free Software Foundation; either version 2 of   *
 * the License, or (at your option) any later version.              *
\********************************************************************/

#include <config.h>

#include <algorithm>
#include <sstream>
#include <string>
#include <vector>

#include "gnc-report.hpp"

extern "C"
{
#include <Account.h>
#include <Split.h>
#include <Transaction.h>
#include <gnc-accounting-period.h>
#include <gnc-commodity.h>
#include <gnc-date.h>
#include <gnc-numeric.h>
#include <gnc-ui-util.h>
#include <guid.h>
}

#include <Account.hpp>

namespace gnc
{
namespace report
{
namespace
{

/* ------------------------------------------------------------------ utils */

gnc_numeric
numeric_add (gnc_numeric left, gnc_numeric right)
{
    return gnc_numeric_add (left, right, GNC_DENOM_AUTO, GNC_HOW_DENOM_LCD);
}

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

std::string
format_amount (gnc_numeric amount, gnc_commodity* currency)
{
    if (currency == nullptr)
        return {};

    /* The Scheme renderer rounds to the commodity's smallest unit before
     * printing (gnc:default-html-gnc-monetary-renderer); do the same so the
     * two reports print identical strings. */
    const gnc_numeric rounded = gnc_numeric_convert (
        amount, gnc_commodity_get_fraction (currency), GNC_HOW_RND_ROUND);
    const char* text = xaccPrintAmount (
        rounded, gnc_commodity_print_info (currency, TRUE));
    return text != nullptr ? std::string (text) : std::string ();
}

std::string
account_guid (const Account* account)
{
    gchar* text = guid_to_string (xaccAccountGetGUID (account));
    std::string result = text != nullptr ? text : "";
    g_free (text);
    return result;
}

/* ------------------------------------------------ Trial Balance semantics */

bool
trial_balance_account_type (const Account* account)
{
    switch (xaccAccountGetType (account))
    {
    case ACCT_TYPE_BANK:
    case ACCT_TYPE_CASH:
    case ACCT_TYPE_CREDIT:
    case ACCT_TYPE_ASSET:
    case ACCT_TYPE_LIABILITY:
    case ACCT_TYPE_STOCK:
    case ACCT_TYPE_MUTUAL:
    case ACCT_TYPE_CURRENCY:
    case ACCT_TYPE_PAYABLE:
    case ACCT_TYPE_RECEIVABLE:
    case ACCT_TYPE_EQUITY:
    case ACCT_TYPE_INCOME:
    case ACCT_TYPE_EXPENSE:
    case ACCT_TYPE_TRADING:
        return true;
    default:
        return false;
    }
}

/* ------------------------------------------------------------- traversal */

gnc_numeric
subtree_sum (const Query& query, Account* account)
{
    gnc_numeric total = query.sum (account);

    GList* children = gnc_account_get_children_sorted (account);
    for (GList* node = children; node != nullptr; node = node->next)
        total = numeric_add (total,
                             subtree_sum (query,
                                          static_cast<Account*>(node->data)));
    g_list_free (children);
    return total;
}

bool
walk_accounts (const Query& query, Account* account, int depth,
               const Grouping& grouping, bool (*filter) (const Account*),
               Table& table)
{
    const bool include = filter == nullptr || filter (account);

    const gnc_numeric own = query.sum (account);
    const bool aggregate = grouping.rollup || grouping.subtotals;
    const gnc_numeric subtree = aggregate ? subtree_sum (query, account) : own;

    if (include)
    {
        Row row;
        row.type = RowType::Account;
        row.label = xaccAccountGetName (account) != nullptr
                        ? xaccAccountGetName (account)
                        : "";
        row.guid = account_guid (account);
        row.depth = depth;
        row.amount = grouping.rollup ? subtree : own;
        table.rows.push_back (std::move (row));
    }

    bool child_rows = false;
    GList* children = gnc_account_get_children_sorted (account);
    for (GList* node = children; node != nullptr; node = node->next)
    {
        const bool added = walk_accounts (query,
                                          static_cast<Account*>(node->data),
                                          depth + 1, grouping, filter, table);
        child_rows = child_rows || added;
    }
    g_list_free (children);

    if (grouping.subtotals && include && child_rows)
    {
        Row row;
        row.type = RowType::Subtotal;
        row.label = std::string ("Total ") +
                    (xaccAccountGetName (account) != nullptr
                         ? xaccAccountGetName (account)
                         : "");
        row.depth = depth;
        row.amount = subtree;
        table.rows.push_back (std::move (row));
    }

    return include || child_rows;
}

} // namespace

/* ------------------------------------------------------------------ query */

gnc_numeric
Query::sum (Account* account) const
{
    gnc_numeric total = gnc_numeric_zero ();

    gnc_account_foreach_split_between_dates (
        account, start, end, false,
        [&] (Split* split)
        {
            if (skip_voided &&
                xaccTransGetVoidStatus (xaccSplitGetParent (split)))
                return;
            if (reconciled_only &&
                xaccSplitGetReconcile (split) != YREC)
                return;
            total = numeric_add (total, xaccSplitGetAmount (split));
        });

    /* Amounts are in the account's commodity; a report that shows a single
     * currency converts them at the report date, as the Scheme exchange-fn
     * does. */
    gnc_commodity* account_currency = xaccAccountGetCommodity (account);
    if (currency != nullptr && account_currency != nullptr &&
        account_currency != currency)
    {
        const time64 when = end.has_value () ? *end
                                             : gnc_time64_get_today_end ();
        total = xaccAccountConvertBalanceToCurrencyAsOfDate (
            account, total, account_currency, currency, when);
    }
    return total;
}

/* -------------------------------------------------------- column vocabulary */

const char*
column_name (Column column)
{
    switch (column)
    {
    case Column::AccountName: return "account-name";
    case Column::Debit:       return "debit";
    case Column::Credit:      return "credit";
    case Column::Balance:     return "balance";
    case Column::Subtotal:    return "subtotal";
    }
    return "";
}

const char*
column_heading (Column column)
{
    switch (column)
    {
    case Column::AccountName: return "Account Name";
    case Column::Debit:       return "Debit";
    case Column::Credit:      return "Credit";
    case Column::Balance:     return "Balance";
    case Column::Subtotal:    return "Subtotal";
    }
    return "";
}

/* -------------------------------------------------------------- grouping */

Table
account_table (Account* root, const Query& query,
               const std::vector<Column>& columns, const Grouping& grouping,
               bool (*filter) (const Account*))
{
    Table table;
    table.columns = columns;
    table.currency = query.currency;

    Account* tree = root != nullptr ? root : query.scope;
    if (tree == nullptr)
        return table;

    GList* children = gnc_account_get_children_sorted (tree);
    for (GList* node = children; node != nullptr; node = node->next)
        walk_accounts (query, static_cast<Account*>(node->data), 0, grouping,
                       filter, table);
    g_list_free (children);
    return table;
}

/* --------------------------------------------------------------- reports */

Table
trial_balance (Account* root, time64 end_date)
{
    gnc_commodity* currency = gnc_default_report_currency ();
    if (currency == nullptr && root != nullptr)
        currency = xaccAccountGetCommodity (root);

    Query query;
    query.scope = root;
    query.end = end_date;
    query.currency = currency;

    /* The "current" variant shows each account's own balance (the Scheme
     * report reads "account-bal", not the recursive balance), in one running
     * debit/credit pair.  Rollups and subtotals are deliberately off. */
    Grouping grouping;
    Table table = account_table (root, query,
                                 {Column::AccountName, Column::Debit,
                                  Column::Credit},
                                 grouping, trial_balance_account_type);
    table.currency = currency;

    for (const Row& row : table.rows)
    {
        if (gnc_numeric_positive_p (row.amount))
            table.debit_total = numeric_add (table.debit_total, row.amount);
        else if (gnc_numeric_negative_p (row.amount))
            table.credit_total = numeric_add (table.credit_total,
                                              gnc_numeric_neg (row.amount));
    }
    /* With no accounts the Scheme report prints a warning instead of a
     * table; skipping the total row keeps the row comparison (zero rows on
     * both sides) honest for an empty book. */
    table.has_totals = !table.rows.empty ();
    return table;
}

time64
default_end_date ()
{
    /* The report's end date option defaults to END_ACCOUNTING_PERIOD (the
     * last entry in the end-date period list), which the engine resolves
     * from the book's fiscal-year setting and the account-summary prefs. */
    time64 end = gnc_accounting_period_fiscal_end ();
    if (end <= 0)
        end = gnc_time64_get_today_end ();
    return end;
}

std::string
render_html (const Table& table, const std::string& title)
{
    std::ostringstream out;
    out << "<!DOCTYPE html>\n<html><head><meta charset='utf-8'><title>"
        << html_escape (title)
        << "</title><style>"
           "body{font-family:system-ui,sans-serif;margin:1rem;color:#222}"
           "h3{font-size:1.1rem}"
           "table{border-collapse:collapse;width:100%}"
           "th,td{padding:.3rem .6rem;border-bottom:1px solid #e0e0e0;"
           "text-align:left}"
           "th{font-weight:600;color:#444}"
           "td.number-cell,td.total-number-cell{text-align:right;"
           "font-variant-numeric:tabular-nums}"
           "td.total-label-cell,td.total-number-cell{font-weight:600}"
           "</style></head><body><h3>"
        << html_escape (title) << "</h3>\n"
        << "<table cellspacing='0' cellpadding='4' border='0'><thead><tr>";

    for (const Column column : table.columns)
        out << "<th>" << column_heading (column) << "</th>";
    out << "</tr></thead><tbody>\n";

    for (const Row& row : table.rows)
    {
        const bool is_account = row.type == RowType::Account;
        out << "<tr>";
        for (const Column column : table.columns)
        {
            switch (column)
            {
            case Column::AccountName:
                if (is_account)
                {
                    out << "<td class='text-cell'>";
                    if (!row.guid.empty ())
                        out << "<a href='gnc-register:acct-guid="
                            << row.guid << "#'>";
                    out << html_escape (row.label);
                    if (!row.guid.empty ())
                        out << "</a>";
                    out << "</td>";
                }
                else
                {
                    out << "<td class='total-label-cell'>"
                        << html_escape (row.label) << "</td>";
                }
                break;

            case Column::Debit:
                if (gnc_numeric_positive_p (row.amount))
                    out << "<td class='number-cell'>"
                        << html_escape (format_amount (row.amount,
                                                       table.currency))
                        << "</td>";
                else
                    out << "<td align='right' valign='top'> </td>";
                break;

            case Column::Credit:
                if (gnc_numeric_negative_p (row.amount))
                    out << "<td class='number-cell'>"
                        << html_escape (format_amount (
                               gnc_numeric_neg (row.amount), table.currency))
                        << "</td>";
                else
                    out << "<td align='right' valign='top'> </td>";
                break;

            case Column::Balance:
                out << "<td class='number-cell'>"
                    << html_escape (format_amount (row.amount, table.currency))
                    << "</td>";
                break;

            case Column::Subtotal:
                out << "<td class='total-label-cell'></td>";
                break;
            }
        }
        out << "</tr>\n";
    }

    if (table.has_totals)
    {
        out << "<tr>";
        for (const Column column : table.columns)
        {
            switch (column)
            {
            case Column::AccountName:
                out << "<td class='total-label-cell'></td>";
                break;
            case Column::Debit:
                out << "<td class='total-number-cell'>"
                    << html_escape (format_amount (table.debit_total,
                                                   table.currency))
                    << "</td>";
                break;
            case Column::Credit:
                out << "<td class='total-number-cell'>"
                    << html_escape (format_amount (table.credit_total,
                                                   table.currency))
                    << "</td>";
                break;
            default:
                out << "<td class='total-number-cell'></td>";
                break;
            }
        }
        out << "</tr>\n";
    }

    out << "</tbody></table></body></html>\n";
    return out.str ();
}

std::vector<std::string>
native_report_names ()
{
    return {"Trial Balance"};
}

bool
render_report (const std::string& name, Account* root, std::string& html)
{
    if (name != "Trial Balance" || root == nullptr)
        return false;

    const time64 end = default_end_date ();
    char date[32] = {0};
    qof_print_date_buff (date, sizeof date, end);

    html = render_html (trial_balance (root, end),
                        std::string ("Trial Balance ") + date);
    return true;
}

} // namespace report
} // namespace gnc
