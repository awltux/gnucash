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
#include <cstring>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <glib.h>

#include "gnc-report.hpp"

extern "C"
{
#include <Account.h>
#include <Split.h>
#include <Transaction.h>
#include <gnc-accounting-period.h>
#include <gnc-commodity.h>
#include <gnc-date.h>
#include <gnc-lot.h>
#include <gnc-numeric.h>
#include <gnc-unicode.h>
#include <gnc-ui-util.h>
#include <gncInvoice.h>
#include <gncOwner.h>
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

/* The stylesheet the native reports carry inline.  The Scheme reports ship
 * their default stylesheet in the document as well (gnucash/report/
 * stylesheets/css.scm, "default-css"), so this is deliberately the same
 * vocabulary: the same font, table cell classes, right-aligned numbers and
 * bold 12pt totals.  The parity harness reads cells by class, not by style,
 * so restyling cannot change the numbers; keeping the classes identical is
 * what keeps both renderers readable by the same extractor. */
const char*
report_stylesheet ()
{
    return
        "html,body{height:100vh;margin:0 8px;color:#222;background:#fff;"
        "font-family:\"Noto Sans\",sans-serif;font-size:10pt}"
        "body,p,table,tr,td,a,th{vertical-align:top}"
        "h3{font-size:15pt;font-weight:bold;margin:.4em 0}"
        "a{font-style:italic;color:#0645ad}"
        "table{border-collapse:collapse;width:100%}"
        "td,th{padding:4px;border-bottom:1px solid #e0e0e0}"
        "th{text-align:left;font-weight:bold;color:#444}"
        "tr{page-break-inside:avoid!important}"
        "td.total-number-cell,td.total-label-cell,td.centered-label-cell{"
        "font-size:12pt;font-weight:bold}"
        "th.column-heading-left{text-align:left}"
        "td.centered-label-cell,th.column-heading-center{text-align:center}"
        "td.number-header,th.column-heading-right,td.number-cell,"
        "td.total-number-cell{text-align:right}"
        "td.neg{color:red}"
        "td.number-cell,td.total-number-cell,td.anchor-cell,td.date-cell{"
        "white-space:nowrap}"
        "td.highlight{background-color:#e1e1e1}"
        ".report-chart{margin:1.5rem 0 .5rem;max-width:48rem}"
        ".report-chart h4{font-size:12pt;font-weight:bold;margin:.25rem 0 .5rem}"
        ".report-chart canvas{width:640px;height:320px;max-width:100%;"
        "border:1px solid #e0e0e0;background:#fff}"
        "@media print{html,body{height:unset}}";
}

std::string
json_escape (const std::string& text)
{
    std::string out;
    out.reserve (text.size ());
    for (const char c : text)
    {
        switch (c)
        {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        /* A chart's labels are account names, and the data block is embedded
         * in a <script> element: an unescaped "<" could close it early. */
        case '<':  out += "\\u003c"; break;
        case '>':  out += "\\u003e"; break;
        case '&':  out += "\\u0026"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20)
            {
                char escape[8];
                g_snprintf (escape, sizeof escape, "\\u%04x",
                            static_cast<unsigned char>(c));
                out += escape;
            }
            else
                out += c;
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

/* The currency a report converts into: the caller's explicit option value,
 * else the report default, falling back to the root account's commodity. */
gnc_commodity*
report_currency (Account* root, gnc_commodity* requested)
{
    if (requested != nullptr)
        return requested;
    gnc_commodity* currency = gnc_default_report_currency ();
    if (currency == nullptr && root != nullptr)
        currency = xaccAccountGetCommodity (root);
    return currency;
}

/* ------------------------------------------------ account type buckets */

/* The same buckets gnc:decompose-accountlist() uses: the balance sheet and
 * the P&L show the accounts of one bucket together, and the bucket decides
 * the sign of the displayed amount. */
enum class Bucket
{
    Asset,
    Liability,
    Equity,
    Income,
    Expense,
    Trading,
    Other,
};

Bucket
bucket_of (const Account* account)
{
    switch (xaccAccountGetType (account))
    {
    case ACCT_TYPE_ASSET:
    case ACCT_TYPE_BANK:
    case ACCT_TYPE_CASH:
    case ACCT_TYPE_CHECKING:
    case ACCT_TYPE_SAVINGS:
    case ACCT_TYPE_MONEYMRKT:
    case ACCT_TYPE_RECEIVABLE:
    case ACCT_TYPE_STOCK:
    case ACCT_TYPE_MUTUAL:
    case ACCT_TYPE_CURRENCY:
        return Bucket::Asset;
    case ACCT_TYPE_LIABILITY:
    case ACCT_TYPE_PAYABLE:
    case ACCT_TYPE_CREDIT:
    case ACCT_TYPE_CREDITLINE:
        return Bucket::Liability;
    case ACCT_TYPE_EQUITY:    return Bucket::Equity;
    case ACCT_TYPE_INCOME:    return Bucket::Income;
    case ACCT_TYPE_EXPENSE:   return Bucket::Expense;
    case ACCT_TYPE_TRADING:   return Bucket::Trading;
    default:                  return Bucket::Other;
    }
}

/* The Scheme report asks gnc_reverse_balance() whether to flip a running
 * balance.  That reads the "reversed accounts" preference, whose default is
 * the credit accounts (liability, payable, equity, income and credit card).
 * gnc_reverse_balance() answers false in the report process because the
 * preference backend is not loaded there, so use the default rule directly;
 * matching a user preference is stage 3's options work. */
bool
shows_reversed (const Account* account)
{
    switch (xaccAccountGetType (account))
    {
    case ACCT_TYPE_LIABILITY:
    case ACCT_TYPE_PAYABLE:
    case ACCT_TYPE_EQUITY:
    case ACCT_TYPE_INCOME:
    case ACCT_TYPE_CREDIT:
        return true;
    default:
        return false;
    }
}

bool
closing_entry_p (const Query& query, Split* split)
{
    if (!query.skip_closing)
        return false;

    const char* description =
        xaccTransGetDescription (xaccSplitGetParent (split));
    if (description == nullptr || description[0] == '\0')
        return false;

    gchar* haystack = g_ascii_strdown (description, -1);
    gchar* needle = g_ascii_strdown (query.closing_pattern.c_str (), -1);
    const bool found = std::strstr (haystack, needle) != nullptr;
    g_free (haystack);
    g_free (needle);
    return found;
}

/* Every account below `root`, parents before children, in the engine's
 * sorted order -- gnc-account-get-descendants-sorted()'s order, which is
 * what the Scheme reports get their account lists in. */
void
collect_accounts (Account* root, std::vector<Account*>& out)
{
    GList* children = gnc_account_get_children_sorted (root);
    for (GList* node = children; node != nullptr; node = node->next)
    {
        auto* child = static_cast<Account*>(node->data);
        out.push_back (child);
        collect_accounts (child, out);
    }
    g_list_free (children);
}

std::string
account_name (const Account* account)
{
    const char* name = xaccAccountGetName (account);
    return name != nullptr ? name : "";
}

/* --------------------------------------------------- depth-limited display */

/* The Scheme balance sheet and P&L display three levels of the account tree
 * (the "Levels of Subaccounts" option defaults to 3, counting from 0) and,
 * because "Flatten list to depth limit" defaults to false, they summarise
 * everything below the deepest level into its ancestor there: the ancestor's
 * row shows the subtree total instead of its own balance.  On a book whose
 * leaves are at the deepest displayed level this makes no difference; on
 * taxreport.gnucash it does, and the two engines disagree if it is ignored. */
constexpr int display_depth_limit = 2;

struct BucketEntry
{
    Account* account;
    gnc_numeric amount;
};

gnc_numeric
bucket_subtree_sum (const Query& query, Account* account, Bucket bucket)
{
    gnc_numeric total = bucket_of (account) == bucket ? query.sum (account)
                                                     : gnc_numeric_zero ();
    GList* children = gnc_account_get_children_sorted (account);
    for (GList* node = children; node != nullptr; node = node->next)
        total = numeric_add (total,
                             bucket_subtree_sum (query,
                                                 static_cast<Account*>(node->data),
                                                 bucket));
    g_list_free (children);
    return total;
}

void
collect_bucket_rows (const Query& query, Account* account, int depth,
                     Bucket bucket, std::vector<BucketEntry>& out)
{
    if (bucket_of (account) == bucket)
    {
        out.push_back ({account,
                        depth >= display_depth_limit
                            ? bucket_subtree_sum (query, account, bucket)
                            : query.sum (account)});
    }
    if (depth >= display_depth_limit)
        return;

    GList* children = gnc_account_get_children_sorted (account);
    for (GList* node = children; node != nullptr; node = node->next)
        collect_bucket_rows (query, static_cast<Account*>(node->data), depth + 1,
                             bucket, out);
    g_list_free (children);
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
            if (closing_entry_p (*this, split))
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

std::vector<Column>
column_vocabulary ()
{
    return {Column::AccountName, Column::Debit, Column::Credit,
            Column::Balance, Column::Subtotal};
}

bool
column_from_name (const std::string& name, Column& column)
{
    for (const Column candidate : column_vocabulary ())
    {
        if (name == column_name (candidate))
        {
            column = candidate;
            return true;
        }
    }
    return false;
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

/* ------------------------------------------------------------ report spec */

namespace
{

/* The GUIDs a spec's account selection means: each named account and its
 * whole subtree, which is the route's "accounts" rule.  An empty selection
 * is the whole book; an empty set here would mean the same thing, so callers
 * use is_empty() to distinguish "all" from "nothing". */
std::set<std::string>
spec_scope (const std::vector<Account*>& accounts)
{
    std::set<std::string> scope;
    for (Account* account : accounts)
    {
        scope.insert (account_guid (account));
        GList* descendants = gnc_account_get_descendants_sorted (account);
        for (GList* node = descendants; node != nullptr; node = node->next)
            scope.insert (account_guid (static_cast<Account*> (node->data)));
        g_list_free (descendants);
    }
    return scope;
}

bool
spec_includes (const std::set<std::string>& scope, Account* account)
{
    return scope.empty () || scope.count (account_guid (account)) > 0;
}

void
spec_flat_rows (const Query& query, Account* account,
                const std::set<std::string>& scope, Table& table)
{
    if (spec_includes (scope, account))
    {
        Row row;
        row.type = RowType::Account;
        row.label = account_name (account);
        row.guid = account_guid (account);
        row.amount = query.sum (account);
        table.rows.push_back (std::move (row));
    }

    GList* children = gnc_account_get_children_sorted (account);
    for (GList* node = children; node != nullptr; node = node->next)
        spec_flat_rows (query, static_cast<Account*> (node->data), scope,
                        table);
    g_list_free (children);
}

bool
spec_tree_rows (const Query& query, Account* account,
                const std::set<std::string>& scope, const Grouping& grouping,
                int depth, Table& table)
{
    const bool include = spec_includes (scope, account);
    if (include)
    {
        Row row;
        row.type = RowType::Account;
        row.label = account_name (account);
        row.guid = account_guid (account);
        row.depth = depth;
        row.amount = grouping.rollup ? subtree_sum (query, account)
                                     : query.sum (account);
        table.rows.push_back (std::move (row));
    }

    bool child_rows = false;
    GList* children = gnc_account_get_children_sorted (account);
    for (GList* node = children; node != nullptr; node = node->next)
    {
        child_rows = spec_tree_rows (query, static_cast<Account*> (node->data),
                                     scope, grouping, depth + 1, table) ||
                     child_rows;
    }
    g_list_free (children);

    if (grouping.subtotals && include && child_rows)
    {
        Row row;
        row.type = RowType::Subtotal;
        row.label = "Total " + account_name (account);
        row.depth = depth;
        row.amount = subtree_sum (query, account);
        table.rows.push_back (std::move (row));
    }
    return include || child_rows;
}

std::string
period_label (const GDate& date, PeriodUnit unit)
{
    gchar buffer[64] = {0};
    g_date_strftime (buffer, sizeof buffer,
                     unit == PeriodUnit::Month ? "%B %Y" : "%Y", &date);
    return buffer;
}

/* One row per period, each the net of every account in scope over that
 * period.  Only balance-sheet-style net amounts make sense here, so the
 * caller's column choice still decides how they are printed. */
bool
spec_period_rows (const TableSpec& spec, const Query& base,
                  const std::vector<Account*>& accounts, Table& table,
                  std::vector<std::string>& labels,
                  std::vector<gnc_numeric>& values, std::string& error)
{
    if (!spec.start.has_value () || !spec.end.has_value ())
    {
        error = "a period-grouped report needs both a start and an end date";
        return false;
    }

    GDate cursor = time64_to_gdate (*spec.start);
    GDate last = time64_to_gdate (*spec.end);
    if (spec.period_unit == PeriodUnit::Month)
    {
        g_date_set_day (&cursor, 1);
    }
    else
    {
        g_date_set_day (&cursor, 1);
        g_date_set_month (&cursor, G_DATE_JANUARY);
    }
    if (!g_date_valid (&cursor) || !g_date_valid (&last))
    {
        error = "cannot resolve the report's date range";
        return false;
    }

    gnc_numeric total = gnc_numeric_zero ();
    while (g_date_compare (&cursor, &last) <= 0)
    {
        GDate next = cursor;
        if (spec.period_unit == PeriodUnit::Month)
            g_date_add_months (&next, 1);
        else
            g_date_add_years (&next, 1);
        GDate period_last = next;
        g_date_subtract_days (&period_last, 1);

        Query query = base;
        query.start = gnc_time64_get_day_start_gdate (&cursor);
        query.end = gnc_time64_get_day_end_gdate (&period_last);

        gnc_numeric amount = gnc_numeric_zero ();
        for (Account* account : accounts)
            amount = numeric_add (amount, query.sum (account));

        const std::string label = period_label (cursor, spec.period_unit);
        Row row;
        row.type = RowType::Account;
        row.label = label;
        row.amount = amount;
        table.rows.push_back (std::move (row));
        labels.push_back (label);
        values.push_back (amount);
        total = numeric_add (total, amount);

        cursor = next;
    }

    if (table.rows.empty ())
    {
        error = "the date range does not contain a period";
        return false;
    }

    Row row;
    row.type = RowType::Total;
    row.label = "Total";
    row.amount = total;
    table.rows.push_back (std::move (row));
    /* The chart shows the periods, not the grand total: a total bar would
     * dwarf every other one and say nothing new. */
    return true;
}

} // namespace

bool
render_table_spec (const TableSpec& spec, Account* root, std::string& html)
{
    if (root == nullptr)
        return false;
    if (spec.columns.empty ())
        return false;

    Query query;
    query.scope = root;
    query.start = spec.start;
    query.end = spec.end;
    /* A spec without a currency means the report default, exactly like the
     * built-in reports; without this the amounts would print as empty
     * strings. */
    query.currency = report_currency (root, spec.currency);

    Table table;
    table.columns = spec.columns;
    table.currency = query.currency;

    std::vector<std::string> chart_labels;
    std::vector<gnc_numeric> chart_values;

    if (spec.grouping == GroupingMode::Period)
    {
        std::vector<Account*> accounts = spec.accounts;
        if (accounts.empty ())
            collect_accounts (root, accounts);
        std::string error;
        if (!spec_period_rows (spec, query, accounts, table, chart_labels,
                               chart_values, error))
            return false;
    }
    else
    {
        const std::set<std::string> scope = spec_scope (spec.accounts);
        if (spec.grouping == GroupingMode::AccountTree)
        {
            Grouping grouping;
            grouping.rollup = true;
            grouping.subtotals = true;

            /* Start at the book root so ancestors keep their place in the
             * tree, but only selected rows (and the subtotals of selected
             * parents) are emitted. */
            GList* children = gnc_account_get_children_sorted (root);
            for (GList* node = children; node != nullptr;
                 node = node->next)
                spec_tree_rows (query, static_cast<Account*> (node->data),
                                scope, grouping, 0, table);
            g_list_free (children);

            /* Totals would double-count a rolled-up tree; the subtotals are
             * the totals.  A chart, if asked for, uses the top-level section
             * subtotals rather than every nested one. */
            for (const Row& row : table.rows)
            {
                if (row.type == RowType::Subtotal && row.depth == 0)
                {
                    chart_labels.push_back (row.label);
                    chart_values.push_back (row.amount);
                }
            }
        }
        else
        {
            GList* children = gnc_account_get_children_sorted (root);
            for (GList* node = children; node != nullptr; node = node->next)
                spec_flat_rows (query, static_cast<Account*> (node->data),
                                scope, table);
            g_list_free (children);

            for (const Row& row : table.rows)
            {
                if (chart_labels.size () >= 12)
                    break;
                if (row.type == RowType::Account &&
                    !gnc_numeric_zero_p (row.amount))
                {
                    chart_labels.push_back (row.label);
                    chart_values.push_back (row.amount);
                }
            }
        }

        /* A flat account list sums one row per account, so its debit/credit
         * columns have the same totals the Trial Balance prints.  The tree
         * case deliberately has none (see above). */
        if (spec.grouping == GroupingMode::None)
        {
            bool has_debit_or_credit = false;
            for (const Column column : spec.columns)
                has_debit_or_credit = has_debit_or_credit ||
                                      column == Column::Debit ||
                                      column == Column::Credit;
            for (const Row& row : table.rows)
            {
                if (gnc_numeric_positive_p (row.amount))
                    table.debit_total = numeric_add (table.debit_total,
                                                     row.amount);
                else if (gnc_numeric_negative_p (row.amount))
                    table.credit_total = numeric_add (table.credit_total,
                                                      gnc_numeric_neg (row.amount));
            }
            table.has_totals = has_debit_or_credit && !table.rows.empty ();
        }
    }

    if (spec.chart && !chart_labels.empty ())
    {
        Chart chart;
        chart.type = ChartType::Bar;
        chart.title = spec.title.empty () ? "Report Totals" : spec.title;
        chart.labels = std::move (chart_labels);
        chart.values = std::move (chart_values);
        table.chart = std::move (chart);
    }

    html = render_html (table, spec.title.empty () ? "Custom Report"
                                                   : spec.title);
    return true;
}

/* ------------------------------------------------------- transaction report */

namespace
{

std::string
account_full_name (const Account* account)
{
    const char* name = gnc_account_get_full_name (
        const_cast<Account*> (account));
    return name != nullptr ? name : "";
}

/* The Scheme report shows one split per line, grouped by account name, with
 * the account's splits in register order (the order the engine keeps them
 * in, which is date order).  The splits are collected here in the order the
 * Scheme query sorts them -- primary key account full name, secondary
 * register order -- so the groups come out in the same sequence. */
struct SplitRow
{
    Split* split;
    std::string account_full_name;
    std::string account_name;
};

void
collect_splits (const Query& query, const std::vector<Account*>& accounts,
                std::vector<SplitRow>& out)
{
    /* The Scheme report's account list is the selection itself; it does not
     * silently add descendants.  A caller that wants the whole book passes
     * the whole book (see the "accounts=all" request in the web server). */
    for (Account* account : accounts)
    {
        for (GList* node = xaccAccountGetSplitList (account); node != nullptr;
             node = node->next)
        {
            auto* split = static_cast<Split*> (node->data);
            Transaction* parent = xaccSplitGetParent (split);
            if (query.skip_voided && xaccTransGetVoidStatus (parent))
                continue;
            if (query.reconciled_only &&
                xaccSplitGetReconcile (split) != YREC)
                continue;
            if (closing_entry_p (query, split))
                continue;
            const time64 date = xaccTransGetDate (parent);
            if (query.start.has_value () && date < *query.start)
                continue;
            if (query.end.has_value () && date > *query.end)
                continue;
            out.push_back ({split, account_full_name (account),
                            account_name (account)});
        }
    }
}

std::string
split_date (Split* split)
{
    char date[32] = {0};
    qof_print_date_buff (date, sizeof date,
                         xaccTransGetDate (xaccSplitGetParent (split)));
    return date;
}

std::string
text_or_empty (const char* text)
{
    return text != nullptr ? text : "";
}

std::string
transfer_name (Split* split)
{
    Transaction* trans = xaccSplitGetParent (split);
    if (xaccTransCountSplits (trans) <= 1)
        return {};

    Split* other = xaccSplitGetOtherSplit (split);
    if (other == nullptr)
        return "Split Transaction";
    return account_name (xaccSplitGetAccount (other));
}

Cell
amount_cell (gnc_numeric amount, gnc_commodity* currency)
{
    return {format_amount (amount, currency), "number-cell"};
}

} // namespace

CellTable
transaction_report (Account* root, const std::vector<Account*>& accounts,
                    std::optional<time64> start, std::optional<time64> end)
{
    gnc_commodity* currency = report_currency (root, nullptr);

    Query query;
    query.scope = root;
    query.start = start;
    query.end = end;
    query.currency = currency;
    query.skip_voided = true;
    query.skip_closing = true;

    std::vector<SplitRow> splits;
    collect_splits (query, accounts, splits);

    std::stable_sort (splits.begin (), splits.end (),
                      [] (const SplitRow& left, const SplitRow& right)
                      {
                          const int by_name = xaccSplitCompareAccountFullNames (
                              left.split, right.split);
                          if (by_name != 0)
                              return by_name < 0;
                          /* Secondary key: register order, which is the
                           * engine's own split order. */
                          return xaccSplitOrder (left.split, right.split) < 0;
                      });

    CellTable table;
    gnc_numeric grand_total = gnc_numeric_zero ();
    gnc_numeric subtotal = gnc_numeric_zero ();
    std::string current_full_name;
    std::string current_name;
    bool in_group = false;

    auto add_total_row = [&table, currency] (const std::string& label,
                                             gnc_numeric amount)
    {
        std::vector<Cell> row;
        row.push_back ({});
        row.push_back ({label, "total-label-cell"});
        if (gnc_numeric_positive_p (amount))
        {
            row.push_back ({format_amount (amount, currency),
                            "total-number-cell"});
            row.push_back ({});
        }
        else
        {
            row.push_back ({});
            row.push_back ({format_amount (gnc_numeric_neg (amount), currency),
                            "total-number-cell"});
        }
        table.rows.push_back (std::move (row));
    };

    for (const SplitRow& entry : splits)
    {
        if (!in_group || entry.account_full_name != current_full_name)
        {
            if (in_group)
                add_total_row ("Total For " + current_name, subtotal);
            current_full_name = entry.account_full_name;
            current_name = entry.account_name;
            in_group = true;
            subtotal = gnc_numeric_zero ();
            table.rows.push_back ({{current_name, "total-label-cell"}, {}, {}});
        }

        Transaction* trans = xaccSplitGetParent (entry.split);
        const gnc_numeric amount = xaccSplitGetAmount (entry.split);

        std::vector<Cell> row;
        row.push_back ({});
        row.push_back ({split_date (entry.split), "date-cell"});
        row.push_back ({text_or_empty (xaccTransGetNum (trans)), "text-cell"});
        row.push_back ({text_or_empty (xaccTransGetDescription (trans)),
                        "text-cell"});
        std::string memo = text_or_empty (xaccSplitGetMemo (entry.split));
        if (memo.empty ())
            memo = text_or_empty (xaccTransGetNotes (trans));
        row.push_back ({memo, ""});
        row.push_back ({transfer_name (entry.split), ""});
        if (gnc_numeric_negative_p (amount))
        {
            row.push_back ({});
            row.push_back (amount_cell (gnc_numeric_neg (amount), currency));
        }
        else
        {
            row.push_back (amount_cell (amount, currency));
            row.push_back ({});
        }
        table.rows.push_back (std::move (row));

        subtotal = numeric_add (subtotal, amount);
        grand_total = numeric_add (grand_total, amount);
    }

    if (in_group)
        add_total_row ("Total For " + current_name, subtotal);
    if (!splits.empty ())
        add_total_row ("Grand Total", grand_total);

    return table;
}

/* --------------------------------------------------------- general ledger */

CellTable
general_ledger (Account* root, const std::vector<Account*>& accounts,
                std::optional<time64> start, std::optional<time64> end)
{
    gnc_commodity* currency = report_currency (root, nullptr);

    Query query;
    query.scope = root;
    query.start = start;
    query.end = end;
    query.currency = currency;
    query.skip_voided = true;
    query.skip_closing = true;

    std::vector<SplitRow> splits;
    collect_splits (query, accounts, splits);

    auto account_code = [] (const Account* account)
    {
        const char* code = xaccAccountGetCode (const_cast<Account*> (account));
        return code != nullptr ? std::string (code) : std::string ();
    };
    auto namestring = [&account_code] (const Account* account)
    {
        const std::string code = account_code (account);
        const std::string name = account_name (account);
        return code.empty () ? name : code + " " + name;
    };

    struct LedgerEntry
    {
        SplitRow row;
        std::string code;
    };
    std::vector<LedgerEntry> entries;
    entries.reserve (splits.size ());
    for (const SplitRow& row : splits)
        entries.push_back (
            {row, account_code (xaccSplitGetAccount (row.split))});

    /* The Scheme report's default primary key is the account code, so the
     * sort (and therefore the grouping) is by that string: accounts that
     * share a code share a group, which is visible on books that reuse
     * codes.  Within a group the secondary key is register order, i.e. the
     * posted date. */
    std::stable_sort (entries.begin (), entries.end (),
                      [] (const LedgerEntry& left, const LedgerEntry& right)
                      {
                          const int by_code = std::strcmp (
                              left.code.c_str (), right.code.c_str ());
                          if (by_code != 0)
                              return by_code < 0;
                          /* Secondary key: register order, the engine's own
                           * split order. */
                          return xaccSplitOrder (left.row.split,
                                                 right.row.split) < 0;
                      });

    CellTable table;
    table.headings = {"", "Date", "Description", "Debit", "Credit",
                      "Running Balance"};

    gnc_numeric subtotal = gnc_numeric_zero ();
    std::string current_code;
    std::string current_name;
    bool in_group = false;
    const time64 begin = start.value_or (0);

    auto add_subtotal = [&table, currency] (const std::string& label,
                                            gnc_numeric amount)
    {
        std::vector<Cell> row;
        row.push_back ({});
        row.push_back ({label, "total-label-cell"});
        if (gnc_numeric_positive_p (amount))
        {
            row.push_back ({format_amount (amount, currency),
                            "total-number-cell"});
            row.push_back ({});
        }
        else
        {
            row.push_back ({});
            row.push_back ({format_amount (gnc_numeric_neg (amount), currency),
                            "total-number-cell"});
        }
        row.push_back ({"", "total-number-cell"});
        table.rows.push_back (std::move (row));
    };

    for (const LedgerEntry& entry : entries)
    {
        Account* account = xaccSplitGetAccount (entry.row.split);
        if (!in_group || entry.code != current_code)
        {
            if (in_group)
                add_subtotal ("Total For " + current_name, subtotal);
            current_code = entry.code;
            current_name = namestring (account);
            in_group = true;
            subtotal = gnc_numeric_zero ();

            /* The heading carries the balance brought forward: the account's
             * balance before the report's first day. */
            gnc_numeric brought_forward =
                xaccAccountGetBalanceAsOfDate (account, begin);
            /* A credit-normal account's running balance is shown the way the
             * register shows it, sign reversed. */
            if (shows_reversed (account))
                brought_forward = gnc_numeric_neg (brought_forward);
            std::vector<Cell> heading;
            heading.push_back ({current_name + ": Balance b/f",
                                "total-label-cell"});
            heading.push_back ({});
            heading.push_back ({});
            heading.push_back ({format_amount (brought_forward, currency),
                                "number-cell"});
            table.rows.push_back (std::move (heading));
        }

        current_name = namestring (account);
        const gnc_numeric amount = xaccSplitGetAmount (entry.row.split);

        std::vector<Cell> row;
        row.push_back ({});
        row.push_back ({split_date (entry.row.split), "date-cell"});
        row.push_back ({text_or_empty (xaccTransGetDescription (
                           xaccSplitGetParent (entry.row.split))),
                        "text-cell"});
        if (gnc_numeric_negative_p (amount))
        {
            row.push_back ({});
            row.push_back ({format_amount (gnc_numeric_neg (amount), currency),
                            "number-cell"});
        }
        else
        {
            row.push_back ({format_amount (amount, currency), "number-cell"});
            row.push_back ({});
        }
        gnc_numeric running = xaccSplitGetBalance (entry.row.split);
        if (shows_reversed (account))
            running = gnc_numeric_neg (running);
        row.push_back ({format_amount (running, currency), "number-cell"});
        table.rows.push_back (std::move (row));
        subtotal = numeric_add (subtotal, amount);
    }

    if (in_group)
        add_subtotal ("Total For " + current_name, subtotal);

    return table;
}

Table
trial_balance (Account* root, std::optional<time64> end_date,
               gnc_commodity* requested_currency)
{
    gnc_commodity* currency = report_currency (root, requested_currency);

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

time64
default_start_date ()
{
    /* The period reports' start date defaults to the start of the accounting
     * period, i.e. the first day of the fiscal year the end date falls in. */
    time64 start = gnc_accounting_period_fiscal_start ();
    if (start <= 0)
        start = gnc_time64_get_day_start (default_end_date ());
    return start;
}

/* -------------------------------------------------------- balance sheet */

Table
balance_sheet (Account* root, std::optional<time64> as_of,
               gnc_commodity* requested_currency)
{
    gnc_commodity* currency = report_currency (root, requested_currency);

    Query query;
    query.scope = root;
    query.end = as_of;
    query.currency = currency;

    Table table;
    table.columns = {Column::AccountName, Column::Balance};
    table.currency = currency;

    std::vector<Account*> accounts;
    if (root != nullptr)
        collect_accounts (root, accounts);

    auto add_section = [&table] (const char* label)
    {
        Row row;
        row.type = RowType::Section;
        row.label = label;
        row.blank = true;
        table.rows.push_back (std::move (row));
    };
    auto add_account = [&table] (Account* account, gnc_numeric amount)
    {
        Row row;
        row.type = RowType::Account;
        row.label = account_name (account);
        row.guid = account_guid (account);
        row.amount = amount;
        table.rows.push_back (std::move (row));
    };
    auto add_total = [&table] (const char* label, gnc_numeric amount)
    {
        Row row;
        row.type = RowType::Total;
        row.label = label;
        row.amount = amount;
        table.rows.push_back (std::move (row));
    };

    auto bucket_rows = [&query, root] (Bucket bucket)
    {
        std::vector<BucketEntry> entries;
        GList* children = gnc_account_get_children_sorted (root);
        for (GList* node = children; node != nullptr; node = node->next)
            collect_bucket_rows (query, static_cast<Account*> (node->data), 0,
                                 bucket, entries);
        g_list_free (children);
        return entries;
    };

    gnc_numeric assets = gnc_numeric_zero ();
    gnc_numeric liabilities = gnc_numeric_zero ();
    gnc_numeric equity = gnc_numeric_zero ();
    gnc_numeric income_expense = gnc_numeric_zero ();

    for (Account* account : accounts)
    {
        switch (bucket_of (account))
        {
        case Bucket::Asset:
            assets = numeric_add (assets, query.sum (account));
            break;
        case Bucket::Liability:
            liabilities = numeric_add (liabilities, query.sum (account));
            break;
        case Bucket::Equity:
            equity = numeric_add (equity, query.sum (account));
            break;
        case Bucket::Income:
        case Bucket::Expense:
            income_expense = numeric_add (income_expense,
                                          query.sum (account));
            break;
        default:
            break;
        }
    }

    /* The Scheme single-column balance sheet runs the asset table, then the
     * liability table and the equity table, and prints one label plus one
     * total per section.  Liability and equity amounts are credit-normal, so
     * the report reverses them: a credit balance shows as a positive number
     * under its natural label. */
    add_section ("Assets");
    for (const BucketEntry& entry : bucket_rows (Bucket::Asset))
        add_account (entry.account, entry.amount);
    add_total ("Total Assets", assets);

    add_section ("Liabilities");
    for (const BucketEntry& entry : bucket_rows (Bucket::Liability))
        add_account (entry.account, gnc_numeric_neg (entry.amount));
    add_total ("Total Liabilities", gnc_numeric_neg (liabilities));

    add_section ("Equity");
    for (const BucketEntry& entry : bucket_rows (Bucket::Equity))
        add_account (entry.account, gnc_numeric_neg (entry.amount));

    /* Income and expense are not shown as rows; their balance is this
     * period's retained earnings, and a loss is labelled as one.  The Scheme
     * report omits the line entirely when it is zero. */
    const gnc_numeric retained = gnc_numeric_neg (income_expense);
    if (!gnc_numeric_zero_p (retained))
    {
        const bool loss = gnc_numeric_negative_p (retained);
        add_total (loss ? "Retained Losses" : "Retained Earnings",
                   loss ? gnc_numeric_neg (retained) : retained);
    }

    const gnc_numeric total_equity =
        numeric_add (gnc_numeric_neg (equity), retained);
    add_total ("Total Equity", total_equity);
    add_total ("Total Liabilities & Equity",
               numeric_add (gnc_numeric_neg (liabilities), total_equity));

    /* Stage 4's chart: the three section totals, drawn below the table.  The
     * values are the same numbers the Total rows print, in the same sign
     * convention, so the chart cannot disagree with the report. */
    Chart chart;
    chart.type = ChartType::Bar;
    chart.title = "Balance Sheet Totals";
    chart.labels = {"Assets", "Liabilities", "Equity"};
    chart.values = {assets, gnc_numeric_neg (liabilities), total_equity};
    table.chart = std::move (chart);

    return table;
}

/* ----------------------------------------------------- income statement */

Table
income_statement (Account* root, std::optional<time64> start,
                  std::optional<time64> end,
                  gnc_commodity* requested_currency)
{
    gnc_commodity* currency = report_currency (root, requested_currency);

    Query query;
    query.scope = root;
    query.start = start;
    query.end = end;
    query.currency = currency;
    /* The report's balance mode is 'pre-closing: closing entries are taken
     * out of the period's income and expense balances. */
    query.skip_closing = true;

    Table table;
    table.columns = {Column::AccountName, Column::Balance};
    table.currency = currency;

    std::vector<Account*> accounts;
    if (root != nullptr)
        collect_accounts (root, accounts);

    auto add_section = [&table] (const char* label)
    {
        Row row;
        row.type = RowType::Section;
        row.label = label;
        row.blank = true;
        table.rows.push_back (std::move (row));
    };
    auto add_account = [&table] (Account* account, gnc_numeric amount)
    {
        Row row;
        row.type = RowType::Account;
        row.label = account_name (account);
        row.guid = account_guid (account);
        row.amount = amount;
        table.rows.push_back (std::move (row));
    };
    auto add_total = [&table] (const char* label, gnc_numeric amount)
    {
        Row row;
        row.type = RowType::Total;
        row.label = label;
        row.amount = amount;
        table.rows.push_back (std::move (row));
    };

    gnc_numeric revenue = gnc_numeric_zero ();
    gnc_numeric expenses = gnc_numeric_zero ();

    for (Account* account : accounts)
    {
        switch (bucket_of (account))
        {
        case Bucket::Income:
            revenue = numeric_add (revenue, query.sum (account));
            break;
        case Bucket::Expense:
            expenses = numeric_add (expenses, query.sum (account));
            break;
        default:
            break;
        }
    }

    auto bucket_rows = [&query, root] (Bucket bucket)
    {
        std::vector<BucketEntry> entries;
        GList* children = gnc_account_get_children_sorted (root);
        for (GList* node = children; node != nullptr; node = node->next)
            collect_bucket_rows (query, static_cast<Account*> (node->data), 0,
                                 bucket, entries);
        g_list_free (children);
        return entries;
    };

    add_section ("Revenues");
    for (const BucketEntry& entry : bucket_rows (Bucket::Income))
        add_account (entry.account, gnc_numeric_neg (entry.amount));
    add_total ("Total Revenue", gnc_numeric_neg (revenue));

    add_section ("Expenses");
    for (const BucketEntry& entry : bucket_rows (Bucket::Expense))
        add_account (entry.account, entry.amount);
    add_total ("Total Expenses", expenses);

    /* Revenue minus expenses.  Like the Scheme report, a negative net is
     * shown with a loss label and its absolute value. */
    const gnc_numeric net = gnc_numeric_sub (gnc_numeric_neg (revenue),
                                             expenses, GNC_DENOM_AUTO,
                                             GNC_HOW_DENOM_LCD);
    const bool loss = gnc_numeric_negative_p (net);
    Row net_row;
    net_row.type = RowType::Account;
    net_row.label = loss ? "Net loss for Period" : "Net income for Period";
    net_row.amount = loss ? gnc_numeric_neg (net) : net;
    table.rows.push_back (std::move (net_row));
    return table;
}

/* ---------------------------------------------------------- aging reports */

namespace
{

/* The Scheme report's num-buckets.  Five buckets are filled by invoices and
 * the sixth is the "Pre-Payment" bucket, which only unapplied payments
 * reach. */
constexpr int aging_bucket_count = 6;
/* The bucket the overpayment of a payment transaction goes into. */
constexpr int aging_prepayment_bucket = aging_bucket_count - 1;

/* gnc:make-extended-interval-list() steps 30 days back from the report date
 * three times and then walks forward again with gnc:make-date-list(), so the
 * boundaries are the end of the day 90, 60 and 30 days before the report
 * date, and the end of the report day itself.  An invoice falls in bucket k
 * when its driving date is earlier than boundary k, which is why a due date
 * of exactly "report date - 90 days" counts as 91+ (its midnight is earlier
 * than that boundary's end of day).  The last bucket, "Current", is
 * everything not earlier than the report day; the Scheme list ends with
 * +inf.0 to close it. */
std::vector<time64>
aging_boundaries (time64 as_of)
{
    std::vector<time64> boundaries;
    GDate date = time64_to_gdate (as_of);
    g_date_subtract_days (&date, 90);
    for (int index = 0; index < 4; ++index)
    {
        boundaries.push_back (gnc_time64_get_day_end_gdate (&date));
        g_date_add_days (&date, 30);
    }
    return boundaries;
}

int
aging_bucket_for (const std::vector<time64>& boundaries, time64 when)
{
    for (size_t index = 0; index < boundaries.size (); ++index)
    {
        if (when < boundaries[index])
            return static_cast<int> (index);
    }
    /* Not earlier than the report day: the "Current" bucket. */
    return static_cast<int> (boundaries.size ());
}

/* gnc:payment-txn->payment-info()'s sign-equal?: both zero, or the same
 * sign. */
bool
sign_equal (gnc_numeric left, gnc_numeric right)
{
    if (gnc_numeric_zero_p (left) || gnc_numeric_zero_p (right))
        return gnc_numeric_zero_p (left) && gnc_numeric_zero_p (right);
    return gnc_numeric_positive_p (left) == gnc_numeric_positive_p (right);
}

/* The owner of a split, resolved the way gnc:make-split->owner does: the
 * lot's own owner when the lot carries one (that is how payment and
 * pre-payment lots are marked), otherwise the owner of the invoice posted in
 * that lot.  An undefined owner means the split cannot be attributed. */
GncOwner
split_owner (Split* split)
{
    GncOwner owner;
    owner.type = GNC_OWNER_NONE;
    owner.owner.undefined = nullptr;
    owner.qof_temp = nullptr;

    GNCLot* lot = xaccSplitGetLot (split);
    if (lot == nullptr)
        return owner;
    if (!gncOwnerGetOwnerFromLot (lot, &owner))
    {
        GncInvoice* invoice = gncInvoiceGetInvoiceFromLot (lot);
        if (invoice != nullptr)
            gncOwnerCopy (gncOwnerGetEndOwner (gncInvoiceGetOwner (invoice)),
                          &owner);
    }
    return owner;
}

/* The part of a payment that is not applied to an invoice, as
 * gnc:payment-txn->payment-info() computes it.  A split in the A/Payable or
 * A/Receivable account that sits in an invoice lot pays that invoice: the
 * invoice's own balance already accounts for it, so the two cancel out of
 * this sum.  Splits outside those accounts, and the part of a pre-payment lot
 * that this split does not consume, are what is left over. */
gnc_numeric
payment_overpayment (Transaction* txn)
{
    Split* apar_split = xaccTransGetFirstAPARAcctSplit (txn, TRUE);
    Account* apar_account =
        apar_split != nullptr ? xaccSplitGetAccount (apar_split) : nullptr;
    if (apar_account == nullptr)
        return gnc_numeric_zero ();

    gnc_numeric overpayment = gnc_numeric_zero ();
    for (GList* node = xaccTransGetSplitList (txn); node != nullptr;
         node = node->next)
    {
        auto* split = static_cast<Split*> (node->data);
        Account* account = xaccSplitGetAccount (split);
        if (account == nullptr)
            continue;

        const gnc_numeric amount = xaccSplitConvertAmount (split, apar_account);
        GNCLot* lot = xaccSplitGetLot (split);
        if (!xaccAccountIsAPARType (xaccAccountGetType (account)) ||
            lot == nullptr || gncInvoiceGetInvoiceFromLot (lot) != nullptr)
        {
            overpayment = numeric_add (overpayment, amount);
            continue;
        }

        /* An unapplied payment lot: what is left of the lot once this split
         * is set aside is the part of the payment still free. */
        gnc_numeric balance = gnc_lot_get_balance (lot);
        if (sign_equal (balance, amount))
            balance = gnc_numeric_zero ();
        gnc_numeric partial = gnc_numeric_neg (balance);
        for (GList* lot_node = gnc_lot_get_split_list (lot);
             lot_node != nullptr; lot_node = lot_node->next)
        {
            auto* lot_split = static_cast<Split*> (lot_node->data);
            if (lot_split != split)
                partial = numeric_add (
                    partial, xaccSplitConvertAmount (lot_split, apar_account));
        }
        overpayment = numeric_add (overpayment, partial);
    }
    return overpayment;
}

/* One owner's aging vector, in the Scheme report's bucket order:
 * 0 = 91+, 1 = 61-90, 2 = 31-60, 3 = 0-30, 4 = Current, 5 = Pre-Payment. */
std::vector<gnc_numeric>
owner_aging (const std::vector<Split*>& splits, const AgingOptions& options,
             const std::vector<time64>& boundaries, bool receivable)
{
    std::vector<gnc_numeric> buckets (aging_bucket_count, gnc_numeric_zero ());
    std::vector<Split*> remaining = splits;
    size_t index = 0;
    while (index < remaining.size ())
    {
        Split* split = remaining[index];
        Transaction* txn = xaccSplitGetParent (split);
        const char type = xaccTransGetTxnType (txn);

        if (type == TXN_TYPE_INVOICE)
        {
            GncInvoice* invoice = gncInvoiceGetInvoiceFromTxn (txn);
            if (invoice != nullptr)
            {
                GNCLot* lot = gncInvoiceGetPostedLot (invoice);
                Account* posted = gncInvoiceGetPostedAcc (invoice);
                gnc_numeric balance = gnc_numeric_zero ();
                if (lot != nullptr && posted != nullptr)
                {
                    /* The invoice's remaining balance: every split of its
                     * lot posted on or before the report date, which leaves
                     * out payments made later. */
                    for (GList* node = gnc_lot_get_split_list (lot);
                         node != nullptr; node = node->next)
                    {
                        auto* lot_split = static_cast<Split*> (node->data);
                        if (xaccTransGetDate (xaccSplitGetParent (lot_split)) <=
                            options.as_of)
                            balance = numeric_add (
                                balance,
                                xaccSplitConvertAmount (lot_split, posted));
                    }
                }
                if (!receivable)
                    balance = gnc_numeric_neg (balance);
                const time64 when =
                    options.date_driver == AgingDate::Posted
                        ? gncInvoiceGetDatePosted (invoice)
                        : gncInvoiceGetDateDue (invoice);
                const int bucket = aging_bucket_for (boundaries, when);
                buckets[bucket] = numeric_add (buckets[bucket], balance);
            }
            ++index;
        }
        else if (type == TXN_TYPE_PAYMENT)
        {
            gnc_numeric overpayment = payment_overpayment (txn);
            if (receivable)
                overpayment = gnc_numeric_neg (overpayment);
            buckets[aging_prepayment_bucket] = numeric_add (
                buckets[aging_prepayment_bucket], overpayment);
            /* The payment has been accounted for in full, so its transaction
             * leaves the list, exactly as the Scheme walk drops it. */
            std::vector<Split*> rest;
            rest.reserve (remaining.size ());
            for (size_t other = index + 1; other < remaining.size (); ++other)
                if (xaccSplitGetParent (remaining[other]) != txn)
                    rest.push_back (remaining[other]);
            remaining = std::move (rest);
            index = 0;
        }
        else
        {
            /* The Scheme report reports these as an invalid transaction type
             * below the table; they never contribute an amount. */
            ++index;
        }
    }
    return buckets;
}

bool
aging_account_type (Account* account, bool receivable)
{
    return xaccAccountGetType (account) ==
           (receivable ? ACCT_TYPE_RECEIVABLE : ACCT_TYPE_PAYABLE);
}

/* The Scheme report's default sort key is the owner name, compared with
 * gnc:string-locale<?, i.e. the engine's Unicode collation. */
int
owner_name_compare (const char* left, const char* right)
{
    return gnc_unicode_compare_accented_case_sensitive (
        left != nullptr ? left : "", right != nullptr ? right : "");
}

struct AgingOwnerRow
{
    GncOwner owner;
    std::vector<gnc_numeric> buckets;
    gnc_numeric total;
};

struct AgingAccountRows
{
    Account* account;
    std::vector<AgingOwnerRow> owners;
    /* The six bucket totals and the account total. */
    std::vector<gnc_numeric> totals;
};

/* The splits of one account the Scheme report's query selects: not voided,
 * posted on or before the report date, one per transaction (the query asks
 * for unique transactions), oldest posted date first. */
std::vector<Split*>
collect_aging_splits (Account* account, const AgingOptions& options)
{
    std::vector<Split*> splits;
    std::set<Transaction*> seen;
    for (GList* node = xaccAccountGetSplitList (account); node != nullptr;
         node = node->next)
    {
        auto* split = static_cast<Split*> (node->data);
        Transaction* txn = xaccSplitGetParent (split);
        if (xaccSplitGetReconcile (split) == VREC)
            continue;
        if (xaccTransGetDate (txn) > options.as_of)
            continue;
        if (!seen.insert (txn).second)
            continue;
        splits.push_back (split);
    }
    std::stable_sort (splits.begin (), splits.end (),
                      [] (Split* left, Split* right)
                      {
                          return xaccTransGetDate (xaccSplitGetParent (left)) <
                                 xaccTransGetDate (xaccSplitGetParent (right));
                      });
    return splits;
}

std::vector<AgingAccountRows>
collect_aging (Account* root, const AgingOptions& options, bool receivable,
               bool& have_accounts)
{
    std::vector<AgingAccountRows> accounts;
    have_accounts = false;
    if (root == nullptr)
        return accounts;

    const std::vector<time64> boundaries = aging_boundaries (options.as_of);

    std::vector<Account*> candidates;
    collect_accounts (root, candidates);

    for (Account* account : candidates)
    {
        if (!aging_account_type (account, receivable))
            continue;
        have_accounts = true;

        std::vector<Split*> splits = collect_aging_splits (account, options);
        if (splits.empty ())
            continue;

        std::map<Split*, GncOwner> owners_by_split;
        auto owner_of = [&owners_by_split] (Split* split) -> const GncOwner&
        {
            auto found = owners_by_split.find (split);
            if (found == owners_by_split.end ())
                found = owners_by_split
                            .emplace (split, split_owner (split))
                            .first;
            return found->second;
        };

        AgingAccountRows rows;
        rows.account = account;
        rows.totals.assign (aging_bucket_count + 1, gnc_numeric_zero ());

        std::vector<Split*> remaining = splits;
        size_t index = 0;
        while (index < remaining.size ())
        {
            Split* split = remaining[index];
            Transaction* txn = xaccSplitGetParent (split);
            const char type = xaccTransGetTxnType (txn);
            if (type != TXN_TYPE_INVOICE && type != TXN_TYPE_PAYMENT)
            {
                ++index;
                continue;
            }

            const GncOwner& owner = owner_of (split);
            if (!gncOwnerIsValid (&owner) ||
                (!options.show_inactive && !gncOwnerGetActive (&owner)))
            {
                ++index;
                continue;
            }

            /* Every other split of this account that belongs to the same
             * owner is part of this owner's aging, wherever it sits in the
             * date order. */
            std::vector<Split*> mine;
            std::vector<Split*> others;
            mine.reserve (remaining.size ());
            others.reserve (remaining.size ());
            for (size_t other = index; other < remaining.size (); ++other)
            {
                Split* candidate = remaining[other];
                if (gncOwnerEqual (&owner, &owner_of (candidate)))
                    mine.push_back (candidate);
                else
                    others.push_back (candidate);
            }

            std::vector<gnc_numeric> buckets =
                owner_aging (mine, options, boundaries, receivable);
            gnc_numeric total = gnc_numeric_zero ();
            bool any = false;
            for (const gnc_numeric& amount : buckets)
            {
                total = numeric_add (total, amount);
                any = any || !gnc_numeric_zero_p (amount);
            }
            if (any || options.show_zeros)
            {
                AgingOwnerRow row;
                row.owner = owner;
                row.buckets = std::move (buckets);
                row.total = total;
                for (int bucket = 0; bucket < aging_bucket_count; ++bucket)
                    rows.totals[bucket] = numeric_add (rows.totals[bucket],
                                                       row.buckets[bucket]);
                rows.totals[aging_bucket_count] = numeric_add (
                    rows.totals[aging_bucket_count], total);
                rows.owners.push_back (std::move (row));
            }

            remaining = std::move (others);
            index = 0;
        }

        if (rows.owners.empty ())
            continue;

        std::stable_sort (rows.owners.begin (), rows.owners.end (),
                          [] (const AgingOwnerRow& left,
                              const AgingOwnerRow& right)
                          {
                              return owner_name_compare (
                                         gncOwnerGetName (&left.owner),
                                         gncOwnerGetName (&right.owner)) < 0;
                          });
        accounts.push_back (std::move (rows));
    }
    return accounts;
}

AgingReport
aging_report (Account* root, const AgingOptions& options, bool receivable)
{
    AgingReport report;
    char date[32] = {0};
    qof_print_date_buff (date, sizeof date, options.as_of);
    report.title = std::string (receivable ? "Receivable Aging"
                                           : "Payable Aging") +
                   " - " + date;

    bool have_accounts = false;
    std::vector<AgingAccountRows> accounts =
        collect_aging (root, options, receivable, have_accounts);
    if (accounts.empty ())
    {
        /* The Scheme report's two explanations for an empty aging: no
         * A/Payable or A/Receivable account at all, or such accounts with
         * nothing in them to age. */
        report.warning = have_accounts
            ? "A/Payable or A/Receivable accounts exist but have no suitable "
              "transactions."
            : "No valid A/Payable or A/Receivable account found. Please "
              "ensure valid AP/AR account exists.";
        return report;
    }

    CellTable& table = report.table;
    table.headings.push_back ("Company");
    for (const std::string& heading : aging_bucket_headings ())
        table.headings.push_back (heading);
    table.headings.push_back ("Total");

    /* With more than one account of the type the Scheme report puts the
     * account's name on a heading row of its own and prefixes every owner
     * row with an empty cell, so the owner rows of one account stay together
     * under it. */
    const bool multiple = accounts.size () > 1;

    /* The aging vector is built oldest bucket first (91+ ... Pre-Payment),
     * but the table prints it the other way round. */
    auto display_order = [] (const std::vector<gnc_numeric>& buckets)
    {
        std::vector<gnc_numeric> ordered;
        ordered.reserve (buckets.size ());
        for (int bucket = aging_bucket_count - 1; bucket >= 0; --bucket)
            ordered.push_back (buckets[bucket]);
        return ordered;
    };
    /* Negative amounts carry the Scheme renderer's "neg" class, which the
     * stylesheet colours red. */
    auto amount_css = [] (gnc_numeric amount, const char* base)
    {
        return gnc_numeric_negative_p (amount) ? std::string (base) + " neg"
                                               : std::string (base);
    };

    for (const AgingAccountRows& account : accounts)
    {
        gnc_commodity* currency = xaccAccountGetCommodity (account.account);

        if (multiple)
            table.rows.push_back ({{account_name (account.account), "text-cell"}});

        for (const AgingOwnerRow& owner : account.owners)
        {
            std::vector<Cell> row;
            if (multiple)
                row.push_back ({});
            const char* name = gncOwnerGetName (&owner.owner);
            row.push_back ({name != nullptr ? name : "", ""});
            for (const gnc_numeric& amount : display_order (owner.buckets))
                row.push_back ({format_amount (amount, currency),
                                amount_css (amount, "number-cell")});
            row.push_back ({format_amount (owner.total, currency),
                            amount_css (owner.total, "number-cell")});
            table.rows.push_back (std::move (row));
        }

        std::vector<Cell> total;
        if (multiple)
            total.push_back ({});
        total.push_back ({"Total", "total-label-cell"});
        /* The account total row prints the six bucket totals and the account
         * total, in the same order as the columns above. */
        for (const gnc_numeric& amount : display_order (account.totals))
            total.push_back ({format_amount (amount, currency),
                              amount_css (amount, "total-number-cell")});
        const gnc_numeric account_total = account.totals[aging_bucket_count];
        total.push_back ({format_amount (account_total, currency),
                          amount_css (account_total, "total-number-cell")});
        table.rows.push_back (std::move (total));
    }
    return report;
}

} // namespace

std::vector<std::string>
aging_bucket_headings ()
{
    return {"Pre-Payment", "Current", "0-30 days", "31-60 days",
            "61-90 days", "91+ days"};
}

time64
default_aging_date ()
{
    /* The Scheme aging reports default their "To" option to the relative date
     * 'today', which gnc:time64-end-day-time() resolves to the end of it. */
    return gnc_time64_get_today_end ();
}

AgingReport
receivable_aging (Account* root, const AgingOptions& options)
{
    return aging_report (root, options, true);
}

AgingReport
payable_aging (Account* root, const AgingOptions& options)
{
    return aging_report (root, options, false);
}

std::string
render_aging (const AgingReport& report)
{
    if (report.warning.empty ())
        return render_cell_table (report.table, report.title);

    /* The Scheme report prints its explanation as document text, under the
     * same title, when there is nothing to age. */
    std::ostringstream out;
    out << "<!DOCTYPE html>\n<html><head><meta charset='utf-8'><title>"
        << html_escape (report.title) << "</title><style>"
        << report_stylesheet () << "</style></head><body><h3>"
        << html_escape (report.title) << "</h3>\n<p class='text-cell'>"
        << html_escape (report.warning) << "</p>\n</body></html>\n";
    return out.str ();
}

namespace
{

/* The chart's numbers, as JSON in the document.  Keeping the data in the
 * markup (rather than only inside the drawing call) means the chart can be
 * read back, tested and archived without running JavaScript. */
std::string
chart_json (const Chart& chart, gnc_commodity* currency)
{
    std::ostringstream out;
    out << "{\"type\":\"bar\",\"title\":\"" << json_escape (chart.title)
        << "\",\"labels\":[";
    for (size_t index = 0; index < chart.labels.size (); ++index)
    {
        if (index > 0)
            out << ",";
        out << "\"" << json_escape (chart.labels[index]) << "\"";
    }
    out << "],\"values\":[";
    for (size_t index = 0; index < chart.values.size (); ++index)
    {
        if (index > 0)
            out << ",";
        char number[G_ASCII_DTOSTR_BUF_SIZE] = {0};
        g_ascii_dtostr (number, sizeof number,
                        gnc_numeric_to_double (chart.values[index]));
        out << number;
    }
    out << "]";
    const char* mnemonic =
        currency != nullptr ? gnc_commodity_get_mnemonic (currency) : nullptr;
    if (mnemonic != nullptr && mnemonic[0] != '\0')
        out << ",\"currency\":\"" << json_escape (mnemonic) << "\"";
    out << "}";
    return out.str ();
}

/* A chart section: a canvas, the data block and a small initialiser.  The
 * Chart.js script itself is vendored and served by gnc-webd; gnc-webui: is a
 * placeholder the server rewrites to the token-prefixed route, so the same
 * markup works for /report/<name> and /report/spec/<id>. */
void
render_chart (std::ostringstream& out, const Chart& chart,
              gnc_commodity* currency)
{
    out << "<section class='report-chart' data-chart-type='bar'>\n"
        << "<h4 class='chart-title'>" << html_escape (chart.title)
        << "</h4>\n"
        << "<div class='chart-box'><canvas id='report-chart-canvas' "
           "width='640' height='320' role='img' aria-label='"
        << html_escape (chart.title) << "'></canvas></div>\n"
        << "<script type='application/json' id='report-chart-data'>"
        << chart_json (chart, currency) << "</script>\n"
        << "<script src='gnc-webui:vendor/chart.umd.js'></script>\n"
        << R"JS(<script>
(function () {
  var source = document.getElementById("report-chart-data");
  var canvas = document.getElementById("report-chart-canvas");
  if (!source || !canvas || typeof Chart === "undefined") return;
  var data = JSON.parse(source.textContent);
  var money = null;
  try {
    if (data.currency) money = new Intl.NumberFormat(undefined,
        {style: "currency", currency: data.currency});
  } catch (err) { money = null; }
  var palette = ["#1f5fa8", "#a83b1f", "#2f7d32", "#7a4b9c",
                 "#c08a00", "#3d7f8c"];
  var chart = new Chart(canvas.getContext("2d"), {
    type: "bar",
    data: {
      labels: data.labels,
      datasets: [{
        label: data.title,
        data: data.values,
        backgroundColor: data.values.map(function (_, index) {
          return palette[index % palette.length];
        }),
        borderWidth: 0
      }]
    },
    options: {
      animation: false,
      responsive: false,
      maintainAspectRatio: false,
      plugins: { legend: { display: false }, title: { display: false } },
      scales: {
        y: { beginAtZero: true, ticks: { callback: function (value) {
          return money ? money.format(value) : String(value);
        } } },
        x: { grid: { display: false } }
      }
    }
  });
  window.__reportCharts = (window.__reportCharts || []).concat([chart]);
}());
</script>)JS"
        << "\n</section>\n";
}

} // namespace

std::string
render_html (const Table& table, const std::string& title)
{
    std::ostringstream out;
    out << "<!DOCTYPE html>\n<html><head><meta charset='utf-8'><title>"
        << html_escape (title) << "</title><style>" << report_stylesheet ()
        << "</style></head><body><h3>" << html_escape (title) << "</h3>\n"
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
                if (row.blank)
                    out << "<td class='total-number-cell'> </td>";
                else if (is_account)
                    out << "<td class='number-cell'>"
                        << html_escape (format_amount (row.amount,
                                                       table.currency))
                        << "</td>";
                else
                    out << "<td class='total-number-cell'>"
                        << html_escape (format_amount (row.amount,
                                                       table.currency))
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

    out << "</tbody></table>\n";

    if (table.chart.has_value ())
        render_chart (out, *table.chart, table.currency);

    out << "</body></html>\n";
    return out.str ();
}

std::string
render_no_accounts (const std::string& title, bool with_title_heading)
{
    /* The Scheme engine's gnc:html-make-generic-warning for the transaction
     * report and the general ledger with their default (empty) account
     * selection.  The wording is the engine's; the "Edit report options"
     * link is not repeated because the native layer has no options UI yet
     * (stage 3 adds it).  The general ledger renderer also sets a document
     * title, which the Scheme page shows above the warning; the transaction
     * report does not. */
    std::ostringstream out;
    out << "<!DOCTYPE html>\n<html><head><meta charset='utf-8'><title>"
        << html_escape (title) << "</title><style>" << report_stylesheet ()
        << "</style></head><body>";
    if (with_title_heading)
        out << "<h3>" << html_escape (title) << "</h3>\n";
    out << "<h3>" << html_escape (title) << ":</h3>\n"
        << "<h3>No accounts selected</h3>\n"
        << "<p>This report requires accounts to be selected in the report "
           "options.</p>\n"
        << "</body></html>\n";
    return out.str ();
}

std::string
render_cell_table (const CellTable& table, const std::string& title)
{
    std::ostringstream out;
    out << "<!DOCTYPE html>\n<html><head><meta charset='utf-8'><title>"
        << html_escape (title) << "</title><style>" << report_stylesheet ()
        << "</style></head><body><h3>"
        << html_escape (title) << "</h3>\n";
    /* The Scheme transaction engine prints the date interval as a heading of
     * its own above the table; do the same so the period is visible even
     * though the document title stays the report name. */
    if (!table.subtitle.empty ())
        out << "<h3>" << html_escape (table.subtitle) << "</h3>\n";
    out << "<table cellspacing='0' cellpadding='4' border='0'><thead><tr>";
    for (const std::string& heading : table.headings)
        out << "<th>" << html_escape (heading) << "</th>";
    out << "</tr></thead><tbody>\n";

    for (const std::vector<Cell>& row : table.rows)
    {
        out << "<tr>";
        for (const Cell& cell : row)
        {
            out << "<td";
            if (!cell.css.empty ())
                out << " class='" << cell.css << "'";
            out << ">";
            out << (cell.text.empty () ? " " : html_escape (cell.text));
            out << "</td>";
        }
        out << "</tr>\n";
    }

    out << "</tbody></table></body></html>\n";
    return out.str ();
}

std::vector<std::string>
native_report_names ()
{
    return {"Trial Balance", "Balance Sheet", "Income Statement",
            "Profit & Loss", "Transaction Report", "General Ledger",
            "Receivable Aging", "Payable Aging"};
}

/* --------------------------------------------------------------- options */

std::vector<std::string>
period_presets ()
{
    return {"this-year", "last-year", "all"};
}

bool
resolve_period (const std::string& preset, DateRange& range)
{
    if (preset == "all")
    {
        range.start.reset ();
        range.end.reset ();
        return true;
    }
    if (preset != "this-year" && preset != "last-year")
        return false;

    /* The interval the reports' date options default to: the engine's
     * accounting period. */
    time64 start = gnc_accounting_period_fiscal_start ();
    time64 end = gnc_accounting_period_fiscal_end ();
    if (start <= 0)
        start = default_start_date ();
    if (end <= 0)
        end = default_end_date ();

    if (preset == "last-year")
    {
        /* The accounting-period helpers answer for the current period only,
         * so the previous period is the same interval one year earlier.  The
         * day-of-month is preserved and 29 February clamps to the 28th. */
        GDate start_date = time64_to_gdate (start);
        GDate end_date = time64_to_gdate (end);
        g_date_subtract_years (&start_date, 1);
        g_date_subtract_years (&end_date, 1);
        start = gnc_time64_get_day_start_gdate (&start_date);
        end = gnc_time64_get_day_end_gdate (&end_date);
    }

    range.start = start;
    range.end = end;
    return true;
}

namespace
{

/* The descriptor's default as the route accepts it: an ISO 8601 date with
 * the time (and the local date format) chopped off. */
std::string
iso_date (time64 when)
{
    char buffer[64] = {0};
    gnc_time64_to_iso8601_buff (when, buffer);
    std::string text (buffer);
    const size_t separator = text.find_first_of ("T ");
    if (separator != std::string::npos)
        text.resize (separator);
    return text;
}

std::string
default_currency_code ()
{
    gnc_commodity* currency = gnc_default_report_currency ();
    if (currency == nullptr)
        return {};
    const char* code = gnc_commodity_get_mnemonic (currency);
    return code != nullptr ? code : "";
}

OptionDescriptor
date_option (const char* name, const char* label, const char* description,
             time64 default_date)
{
    OptionDescriptor option;
    option.name = name;
    option.type = OptionType::Date;
    option.label = label;
    option.description = description;
    option.default_value = iso_date (default_date);
    return option;
}

OptionDescriptor
accounts_option ()
{
    OptionDescriptor option;
    option.name = "accounts";
    option.type = OptionType::AccountList;
    option.label = "Accounts";
    option.description =
        "Accounts to report on, as a comma-separated list of account GUIDs; "
        "each GUID selects that account's subtree.  \"all\" selects every "
        "account in the book.  Without the option the report's own default "
        "selection applies, which for this report is empty.";
    return option;
}

OptionDescriptor
currency_option ()
{
    OptionDescriptor option;
    option.name = "currency";
    option.type = OptionType::Currency;
    option.label = "Report's currency";
    option.description =
        "The commodity the report's amounts are converted into.";
    option.default_value = default_currency_code ();
    return option;
}

OptionDescriptor
period_option ()
{
    OptionDescriptor option;
    option.name = "period";
    option.type = OptionType::Choice;
    option.label = "Period";
    option.description =
        "Resolve the date range from the accounting period instead of "
        "taking literal start/end dates.";
    option.default_value = "this-year";
    option.choices = period_presets ();
    return option;
}

} // namespace

std::vector<OptionDescriptor>
report_options (const std::string& name)
{
    std::vector<OptionDescriptor> options;

    if (name == "Trial Balance")
    {
        options.push_back (date_option (
            "start", "Start of Adjusting/Closing",
            "The Scheme trial balance's work-sheet variants collect adjusting "
            "and closing entries from this date.  The native report implements "
            "the 'current' variant, whose account table is given only an end "
            "date, so this option is accepted but has no effect.",
            default_start_date ()));
        options.push_back (date_option (
            "end", "Date of Report",
            "The date the account balances are calculated at.",
            default_end_date ()));
        options.push_back (currency_option ());
    }
    else if (name == "Balance Sheet")
    {
        options.push_back (date_option (
            "start", "Start Date",
            "Accepted so a date range can be handed to any report; the "
            "balance sheet is drawn as of its end date and does not use a "
            "start date.",
            default_start_date ()));
        options.push_back (date_option (
            "end", "Balance Sheet Date",
            "The date the balance sheet is drawn at.",
            default_end_date ()));
        options.push_back (currency_option ());
    }
    else if (name == "Income Statement" || name == "Profit & Loss")
    {
        options.push_back (date_option (
            "start", "Start Date",
            "The first day of the period the report covers.",
            default_start_date ()));
        options.push_back (date_option (
            "end", "End Date",
            "The last day of the period the report covers.",
            default_end_date ()));
        options.push_back (currency_option ());
    }
    else if (name == "Transaction Report" || name == "General Ledger")
    {
        options.push_back (date_option (
            "start", "Start Date",
            "The first day of the period the report covers.",
            default_start_date ()));
        options.push_back (date_option (
            "end", "End Date",
            "The last day of the period the report covers.",
            default_end_date ()));
        options.push_back (accounts_option ());
    }
    else if (name == "Receivable Aging" || name == "Payable Aging")
    {
        /* The aging reports take one date: the Scheme report's "To" option,
         * which defaults to today rather than to the accounting period.  It
         * is the anchor the buckets are measured from, so it is the whole
         * option surface; the report always ages every A/Receivable (or
         * A/Payable) account in the book. */
        options.push_back (date_option (
            "end", "To",
            "The date the aging is calculated at.  The buckets run back from "
            "the end of this day: Current (not yet due), 0-30, 31-60, 61-90 "
            "and 91+ days.  It defaults to today.",
            default_aging_date ()));
        return options;
    }
    else
    {
        return {};
    }

    options.push_back (period_option ());
    return options;
}

bool
report_has_option (const std::string& report, const std::string& option)
{
    for (const OptionDescriptor& descriptor : report_options (report))
    {
        if (descriptor.name == option)
            return true;
    }
    return false;
}

bool
render_report (const std::string& name, Account* root,
               const ReportRequest& request, std::string& html)
{
    if (root == nullptr)
        return false;

    /* The aging reports are anchored at one date (the Scheme "To" option),
     * whose default is today -- not the accounting period the balance
     * reports default their end date to.  Period=all means "no date bound"
     * everywhere else; an aging report has to pick a day, so it stays
     * today. */
    if (name == "Receivable Aging" || name == "Payable Aging")
    {
        AgingOptions options;
        options.as_of = request.all_dates
                            ? default_aging_date ()
                            : request.end.value_or (default_aging_date ());
        AgingReport report = name == "Receivable Aging"
                                 ? receivable_aging (root, options)
                                 : payable_aging (root, options);
        html = render_aging (report);
        return true;
    }

    /* The reports' own defaults are a real period (the accounting period),
     * not the whole book; period=all is the one way to ask for no bounds at
     * all. */
    const bool unbounded = request.all_dates;
    const std::optional<time64> start =
        unbounded ? std::nullopt
                  : std::optional<time64> (
                        request.start.value_or (default_start_date ()));
    const std::optional<time64> end =
        unbounded ? std::nullopt
                  : std::optional<time64> (
                        request.end.value_or (default_end_date ()));

    auto date_text = [] (std::optional<time64> when)
    {
        if (!when.has_value ())
            return std::string ("All Dates");
        char date[32] = {0};
        qof_print_date_buff (date, sizeof date, *when);
        return std::string (date);
    };
    const std::string interval =
        unbounded ? std::string ("All Dates")
                  : date_text (start) + " to " + date_text (end);

    if (name == "Trial Balance")
    {
        html = render_html (trial_balance (root, end, request.currency),
                            "Trial Balance " + date_text (end));
        return true;
    }
    if (name == "Balance Sheet")
    {
        html = render_html (balance_sheet (root, end, request.currency),
                            "Balance Sheet " + date_text (end));
        return true;
    }
    if (name == "Income Statement" || name == "Profit & Loss")
    {
        std::string title = name;
        title += " For Period Covering ";
        title += interval;
        html = render_html (
            income_statement (root, start, end, request.currency), title);
        return true;
    }
    if (name == "Transaction Report")
    {
        /* The Scheme report's default account selection is empty, and an
         * empty selection produces its "No accounts selected" warning, not a
         * report over the whole book.  Same output here until the caller
         * chooses accounts. */
        if (request.accounts.empty ())
        {
            html = render_no_accounts ("Transaction Report", false);
            return true;
        }
        CellTable table =
            transaction_report (root, request.accounts, start, end);
        table.subtitle = interval;
        html = render_cell_table (table, "Transaction Report");
        return true;
    }
    if (name == "General Ledger")
    {
        if (request.accounts.empty ())
        {
            html = render_no_accounts ("General Ledger", true);
            return true;
        }
        CellTable table = general_ledger (root, request.accounts, start, end);
        table.subtitle = interval;
        html = render_cell_table (table, "General Ledger");
        return true;
    }
    return false;
}

bool
render_report (const std::string& name, Account* root, std::string& html)
{
    return render_report (name, root, ReportRequest {}, html);
}

} // namespace report
} // namespace gnc
