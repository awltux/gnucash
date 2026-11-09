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
#include <cmath>
#include <cstring>
#include <map>
#include <optional>
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
#include <gnc-budget.h>
#include <gnc-commodity.h>
#include <gnc-date.h>
#include <gnc-lot.h>
#include <gnc-numeric.h>
#include <gnc-pricedb.h>
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

    /* The route's dates are independent, and an end date earlier than the
     * report's default start (e.g. ?end=2009-12-31 when the accounting
     * period starts today) is an inverted range.  The engine's split
     * iterator is std::for_each(first, last), which is undefined behaviour
     * when first > last -- it segfaulted gnc-webd -- so an inverted range is
     * an empty range here. */
    if (start.has_value () && end.has_value () && *start > *end)
        return gnc_numeric_zero ();

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

std::vector<std::string>
account_type_vocabulary ()
{
    /* The persistent names, in the order the engine defines the types.  ROOT
     * is deliberately absent: a report never scopes to the hidden root. */
    return {"BANK",       "CASH",    "ASSET",     "LIABILITY", "STOCK",
            "MUTUAL",     "CURRENCY", "INCOME",   "EXPENSE",   "EQUITY",
            "RECEIVABLE", "PAYABLE", "TRADING"};
}

bool
account_type_from_name (const std::string& name, GNCAccountType& type)
{
    /* The names account_type_vocabulary() publishes and the names this
     * accepts have to be the same set; keep the two lists in step. */
    struct Entry
    {
        const char* name;
        GNCAccountType type;
    };
    static const Entry entries[] = {
        {"BANK", ACCT_TYPE_BANK},         {"CASH", ACCT_TYPE_CASH},
        {"CREDIT", ACCT_TYPE_CREDIT},     {"ASSET", ACCT_TYPE_ASSET},
        {"LIABILITY", ACCT_TYPE_LIABILITY},
        {"STOCK", ACCT_TYPE_STOCK},       {"MUTUAL", ACCT_TYPE_MUTUAL},
        {"CURRENCY", ACCT_TYPE_CURRENCY}, {"INCOME", ACCT_TYPE_INCOME},
        {"EXPENSE", ACCT_TYPE_EXPENSE},   {"EQUITY", ACCT_TYPE_EQUITY},
        {"RECEIVABLE", ACCT_TYPE_RECEIVABLE},
        {"PAYABLE", ACCT_TYPE_PAYABLE},   {"TRADING", ACCT_TYPE_TRADING},
    };
    for (const Entry& entry : entries)
    {
        if (name == entry.name)
        {
            type = entry.type;
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

/* The account scope a spec means.  An empty selection is the whole book; an
 * empty type list is every type.  The two restrictions intersect, so a GUID
 * selects a subtree and an account type narrows that subtree (or, on its own,
 * scopes the whole book by type). */
struct SpecScope
{
    std::set<std::string> guids;
    std::vector<GNCAccountType> types;

    bool accepts (const Account* account) const
    {
        if (!types.empty () &&
            std::find (types.begin (), types.end (),
                       xaccAccountGetType (account)) == types.end ())
            return false;
        return guids.empty () || guids.count (account_guid (account)) > 0;
    }
};

SpecScope
spec_scope (const std::vector<Account*>& accounts,
            const std::vector<GNCAccountType>& types)
{
    SpecScope scope;
    scope.types = types;
    for (Account* account : accounts)
    {
        scope.guids.insert (account_guid (account));
        GList* descendants = gnc_account_get_descendants_sorted (account);
        for (GList* node = descendants; node != nullptr; node = node->next)
            scope.guids.insert (
                account_guid (static_cast<Account*> (node->data)));
        g_list_free (descendants);
    }
    return scope;
}

/* Every account in scope, parents before children, in the engine's sorted
 * order.  This is the account list a period report sums. */
void
collect_scope_accounts (Account* root, const SpecScope& scope,
                        std::vector<Account*>& out)
{
    GList* children = gnc_account_get_children_sorted (root);
    for (GList* node = children; node != nullptr; node = node->next)
    {
        auto* child = static_cast<Account*> (node->data);
        if (scope.accepts (child))
            out.push_back (child);
        collect_scope_accounts (child, scope, out);
    }
    g_list_free (children);
}

void
spec_flat_rows (const Query& query, Account* account, const SpecScope& scope,
                Table& table)
{
    if (scope.accepts (account))
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
spec_tree_rows (const Query& query, Account* account, const SpecScope& scope,
                const Grouping& grouping, int depth, Table& table)
{
    const bool include = scope.accepts (account);
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
 * period.  The two sides are summed per account before they are netted, so a
 * period that contains both income and expenses keeps them apart: the Debit
 * column (and a chart's Debit series) is what the period debited, the Credit
 * column what it credited.  With `spec.cumulative` the row is instead the
 * balance as of the period's end. */
bool
spec_period_rows (const TableSpec& spec, const Query& base,
                  const std::vector<Account*>& accounts, Table& table,
                  std::string& error)
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
        if (spec.cumulative)
        {
            /* A balance at a date is not the period's activity: accumulate
             * from the beginning of the book up to the period's end. */
            query.start.reset ();
            query.end = gnc_time64_get_day_end_gdate (&period_last);
        }
        else
        {
            query.start = gnc_time64_get_day_start_gdate (&cursor);
            query.end = gnc_time64_get_day_end_gdate (&period_last);
        }

        gnc_numeric debit = gnc_numeric_zero ();
        gnc_numeric credit = gnc_numeric_zero ();
        for (Account* account : accounts)
        {
            const gnc_numeric amount = query.sum (account);
            if (gnc_numeric_positive_p (amount))
                debit = numeric_add (debit, amount);
            else if (gnc_numeric_negative_p (amount))
                credit = numeric_add (credit, gnc_numeric_neg (amount));
        }

        const std::string label = period_label (cursor, spec.period_unit);
        Row row;
        row.type = RowType::Account;
        row.label = label;
        row.split_amounts = true;
        row.debit = debit;
        row.credit = credit;
        row.amount = numeric_add (debit, gnc_numeric_neg (credit));
        table.rows.push_back (std::move (row));
        total = numeric_add (total, row.amount);

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

/* The date range a period spec with no bounds should print: the span the
 * scoped transactions cover, at day granularity.  A book with no transactions
 * in scope has no range to print, and saying so beats printing nothing. */
bool
scope_date_range (const std::vector<Account*>& accounts, time64& start,
                  time64& end)
{
    bool found = false;
    time64 first = 0;
    time64 last = 0;
    for (Account* account : accounts)
    {
        for (GList* node = xaccAccountGetSplitList (account);
             node != nullptr; node = node->next)
        {
            auto* split = static_cast<Split*> (node->data);
            const time64 when = xaccTransGetDate (xaccSplitGetParent (split));
            if (!found || when < first)
                first = when;
            if (!found || when > last)
                last = when;
            found = true;
        }
    }
    if (!found)
        return false;
    start = gnc_time64_get_day_start (first);
    end = gnc_time64_get_day_end (last);
    return true;
}

/* The chart a spec means: the amount columns it plots.  An empty
 * chart_columns means derive them from the table's own columns -- the two
 * sides when both are shown, else the single balance or one side. */
std::vector<Column>
chart_columns_for (const TableSpec& spec)
{
    if (!spec.chart_columns.empty ())
        return spec.chart_columns;

    bool debit = false;
    bool credit = false;
    bool balance = false;
    for (const Column column : spec.columns)
    {
        debit = debit || column == Column::Debit;
        credit = credit || column == Column::Credit;
        balance = balance || column == Column::Balance;
    }
    if (debit && credit)
        return {Column::Debit, Column::Credit};
    if (balance)
        return {Column::Balance};
    if (debit)
        return {Column::Debit};
    if (credit)
        return {Column::Credit};
    return {};
}

gnc_numeric
row_series_amount (const Row& row, Column column)
{
    if (column == Column::Balance)
        return row.amount;
    if (column != Column::Debit && column != Column::Credit)
        return gnc_numeric_zero ();
    if (row.split_amounts)
        return column == Column::Debit ? row.debit : row.credit;
    if (column == Column::Debit && gnc_numeric_positive_p (row.amount))
        return row.amount;
    if (column == Column::Credit && gnc_numeric_negative_p (row.amount))
        return gnc_numeric_neg (row.amount);
    return gnc_numeric_zero ();
}

} // namespace

bool
render_table_spec (const TableSpec& requested, Account* root, std::string& html,
                   std::string* error)
{
    if (root == nullptr)
        return false;
    if (requested.columns.empty ())
        return false;

    const SpecScope scope = spec_scope (requested.accounts,
                                        requested.account_types);
    std::vector<Account*> scoped_accounts;
    collect_scope_accounts (root, scope, scoped_accounts);

    /* A period report with no bounds prints the range the scoped
     * transactions cover, so a built-in spec needs no dates that only fit
     * one book. */
    TableSpec spec = requested;
    if (spec.grouping == GroupingMode::Period &&
        (!spec.start.has_value () || !spec.end.has_value ()))
    {
        time64 start = 0;
        time64 end = 0;
        if (!scope_date_range (scoped_accounts, start, end))
        {
            if (error != nullptr)
                *error = "the report's account scope has no transactions, so a "
                         "period-grouped report has no date range";
            return false;
        }
        spec.start = start;
        spec.end = end;
    }

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

    if (spec.grouping == GroupingMode::Period)
    {
        std::string reason;
        if (!spec_period_rows (spec, query, scoped_accounts, table, reason))
        {
            if (error != nullptr)
                *error = reason;
            return false;
        }
    }
    else
    {
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
        }
        else
        {
            GList* children = gnc_account_get_children_sorted (root);
            for (GList* node = children; node != nullptr; node = node->next)
                spec_flat_rows (query, static_cast<Account*> (node->data),
                                scope, table);
            g_list_free (children);
        }

        /* A flat account list sums one row per account, so its debit/credit
         * columns have the same totals the Trial Balance prints.  The tree
         * case deliberately has none (its subtotals are the totals). */
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

    if (spec.chart)
    {
        /* The chart rows are the table's own: one per period; the top-level
         * section subtotals of a tree; the accounts of a flat list.  A flat
         * list's zero rows carry no information and would make the chart
         * unreadable, so they are skipped, with a cap so a book with
         * hundreds of accounts stays legible. */
        std::vector<const Row*> chart_rows;
        switch (spec.grouping)
        {
        case GroupingMode::Period:
            for (const Row& row : table.rows)
                if (row.type == RowType::Account)
                    chart_rows.push_back (&row);
            break;
        case GroupingMode::AccountTree:
            for (const Row& row : table.rows)
                if (row.type == RowType::Subtotal && row.depth == 0)
                    chart_rows.push_back (&row);
            break;
        case GroupingMode::None:
            for (const Row& row : table.rows)
            {
                if (chart_rows.size () >= 12)
                    break;
                if (row.type == RowType::Account &&
                    !gnc_numeric_zero_p (row.amount))
                    chart_rows.push_back (&row);
            }
            break;
        }

        const std::vector<Column> series_columns = chart_columns_for (spec);
        if (!chart_rows.empty () && !series_columns.empty ())
        {
            Chart chart;
            chart.type = spec.chart_type;
            chart.title = spec.title.empty () ? "Report Totals" : spec.title;
            for (const Row* row : chart_rows)
                chart.labels.push_back (row->label);
            for (const Column column : series_columns)
            {
                ChartSeries series;
                series.label = column_heading (column);
                series.values.reserve (chart_rows.size ());
                for (const Row* row : chart_rows)
                    series.values.push_back (row_series_amount (*row, column));
                chart.series.push_back (std::move (series));
            }
            table.chart = std::move (chart);
        }
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
    ChartSeries series;
    series.label = "Total";
    series.values = {assets, gnc_numeric_neg (liabilities), total_equity};
    chart.series.push_back (std::move (series));
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

/* -------------------------------------------------------- investment lots */

/* The Scheme report measures a holding period with gnc:date-year-delta():
 * whole years plus the difference of the two dates' day-of-year over 366,
 * with February 29 normalised away so that a day after February is the same
 * distance from January 1 in a leap year and in a common year. */
double
date_year_delta (time64 from, time64 to)
{
    struct tm from_tm;
    struct tm to_tm;
    gnc_localtime_r (&from, &from_tm);
    gnc_localtime_r (&to, &to_tm);

    const int year1 = from_tm.tm_year + 1900;
    const int year2 = to_tm.tm_year + 1900;
    const int day1 = from_tm.tm_yday + 1;
    const int day2 = to_tm.tm_yday + 1;

    auto leap_year = [] (int year)
    {
        return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    };
    const int adjusted1 = (!leap_year (year1) && day1 >= 59) ? day1 + 1 : day1;
    const int adjusted2 = (!leap_year (year2) && day2 >= 59) ? day2 + 1 : day2;

    return (year2 - year1) + (adjusted2 - adjusted1) / 366.0;
}

/* calculate-cagr(): false when the ratio is undefined, else compound annual
 * growth as a fraction (0.1 is 10%). */
std::optional<double>
calculate_cagr (gnc_numeric basis, gnc_numeric end_value,
                std::optional<double> years)
{
    if (!years.has_value () || *years <= 0.0)
        return std::nullopt;
    if (gnc_numeric_zero_p (basis) || gnc_numeric_negative_p (basis) ||
        gnc_numeric_negative_p (end_value))
        return std::nullopt;
    const double ratio = gnc_numeric_to_double (end_value) /
                         gnc_numeric_to_double (basis);
    return std::pow (ratio, 1.0 / *years) - 1.0;
}

/* long-term?(): strictly more than the threshold, as the Scheme `>` does. */
bool
is_long_term (std::optional<time64> bought, time64 sold, double years)
{
    if (!bought.has_value ())
        return false;
    return date_year_delta (*bought, sold) > years;
}

struct PriceHit
{
    /* Owned by the caller: gnc_price_unref() when done with it. */
    GNCPrice* price = nullptr;
};

/* find-price(): among the prices for `commodity` nearest before the report
 * date, prefer one quoted in the report currency; a price quoted the other
 * way round is inverted, and with no match at all the first price is used.
 * That is the Scheme report's search order for its default "Last up through
 * report date" price source. */
PriceHit
find_price (GNCPriceDB* db, gnc_commodity* commodity, gnc_commodity* report_cur,
            time64 as_of)
{
    PriceHit hit;
    if (db == nullptr || commodity == nullptr || report_cur == nullptr)
        return hit;

    PriceList* prices = gnc_pricedb_lookup_nearest_before_any_currency_t64 (
        db, commodity, time64CanonicalDayTime (as_of));
    if (prices == nullptr)
        return hit;

    GNCPrice* direct = nullptr;
    GNCPrice* inverse = nullptr;
    for (GList* node = prices; node != nullptr; node = node->next)
    {
        auto* price = static_cast<GNCPrice*> (node->data);
        if (gnc_commodity_equiv (report_cur, gnc_price_get_currency (price)))
        {
            direct = price;
            break;
        }
        if (inverse == nullptr &&
            gnc_commodity_equiv (report_cur, gnc_price_get_commodity (price)))
            inverse = price;
    }

    if (direct != nullptr)
    {
        gnc_price_ref (direct);
        hit.price = direct;
    }
    else if (inverse != nullptr)
        hit.price = gnc_price_invert (inverse);
    else
    {
        auto* first = static_cast<GNCPrice*> (prices->data);
        gnc_price_ref (first);
        hit.price = first;
    }
    gnc_price_list_destroy (prices);
    return hit;
}

/* The Scheme report converts every amount with gnc:case-exchange-fn() at its
 * default pricedb-before source; the engine has the same lookup.  Equal
 * commodities are the identity, which is the whole of a single-currency
 * book. */
gnc_numeric
convert_to_report_currency (gnc_numeric amount, gnc_commodity* from,
                            gnc_commodity* report_cur, GNCPriceDB* db,
                            time64 as_of)
{
    if (from == nullptr || report_cur == nullptr ||
        gnc_commodity_equiv (from, report_cur))
        return amount;
    return gnc_pricedb_convert_balance_nearest_before_price_t64 (
        db, amount, from, report_cur, as_of);
}

/* One row's worth of numbers, and the same struct after merging a group of
 * rows into a total (an account's lots, or every account's).  The Scheme
 * report calls this a "lot stats collector"; keeping it a value rather than
 * a closure is the only difference. */
struct LotStats
{
    enum class Kind
    {
        Lot,
        Account,
        GrandTotal,
    };

    Kind kind = Kind::Lot;
    GNCLot* lot = nullptr;
    Account* account = nullptr;
    gnc_commodity* currency = nullptr;

    int bought_split_count = 0;
    gnc_numeric old_bought_amount;
    gnc_numeric old_bought_value;
    gnc_numeric bought_amount;
    gnc_numeric bought_value;
    int sold_split_count = 0;
    gnc_numeric st_sold_amount;
    gnc_numeric st_sold_basis;
    gnc_numeric st_sold_value;
    gnc_numeric st_realized_gain;
    gnc_numeric lt_sold_amount;
    gnc_numeric lt_sold_basis;
    gnc_numeric lt_sold_value;
    gnc_numeric lt_realized_gain;
    double weighted_realized_cagr = 0.0;
    gnc_numeric end_amount;
    gnc_numeric end_basis;
    gnc_numeric end_value;
    gnc_numeric unrealized_gain;
    gnc_numeric st_unrealized_gain;
    gnc_numeric lt_unrealized_gain;
    double weighted_unrealized_cagr = 0.0;
    bool has_warnings = false;
    bool active = false;
    gnc_numeric splits_realized_gain;

    std::optional<time64> earliest_bought_date;
    std::optional<time64> latest_bought_date;
    Split* earliest_bought_split = nullptr;
    Split* last_sold_split = nullptr;
    std::optional<time64> last_sold_date;
    bool has_negative = false;
    std::optional<time64> first_negative_date;
    std::optional<double> unrealized_cagr;
};

gnc_numeric
zero_in (gnc_commodity* commodity)
{
    const int fraction =
        commodity != nullptr ? gnc_commodity_get_fraction (commodity) : 100;
    return gnc_numeric_create (0, fraction);
}

LotStats
make_stats (LotStats::Kind kind, Account* account, gnc_commodity* report_cur)
{
    LotStats stats;
    stats.kind = kind;
    stats.account = account;
    stats.currency =
        account != nullptr ? xaccAccountGetCommodity (account) : nullptr;

    stats.old_bought_amount = zero_in (stats.currency);
    stats.bought_amount = zero_in (stats.currency);
    stats.st_sold_amount = zero_in (stats.currency);
    stats.lt_sold_amount = zero_in (stats.currency);
    stats.end_amount = zero_in (stats.currency);

    stats.old_bought_value = zero_in (report_cur);
    stats.bought_value = zero_in (report_cur);
    stats.st_sold_basis = zero_in (report_cur);
    stats.st_sold_value = zero_in (report_cur);
    stats.st_realized_gain = zero_in (report_cur);
    stats.lt_sold_basis = zero_in (report_cur);
    stats.lt_sold_value = zero_in (report_cur);
    stats.lt_realized_gain = zero_in (report_cur);
    stats.end_basis = zero_in (report_cur);
    stats.end_value = zero_in (report_cur);
    stats.unrealized_gain = zero_in (report_cur);
    stats.st_unrealized_gain = zero_in (report_cur);
    stats.lt_unrealized_gain = zero_in (report_cur);
    stats.splits_realized_gain = zero_in (report_cur);
    return stats;
}

/* Merge one collector into another.  Amounts (shares) are only summed into an
 * account's total: the grand total may span accounts with different
 * commodities, so there they are left out, exactly as the Scheme report's
 * merge-stats does with its include-amounts flag. */
void
merge_stats (LotStats& into, const LotStats& from)
{
    into.bought_split_count += from.bought_split_count;
    into.old_bought_value =
        gnc_numeric_add_fixed (into.old_bought_value, from.old_bought_value);
    into.bought_value =
        gnc_numeric_add_fixed (into.bought_value, from.bought_value);
    into.sold_split_count += from.sold_split_count;
    into.st_sold_basis =
        gnc_numeric_add_fixed (into.st_sold_basis, from.st_sold_basis);
    into.st_sold_value =
        gnc_numeric_add_fixed (into.st_sold_value, from.st_sold_value);
    into.st_realized_gain =
        gnc_numeric_add_fixed (into.st_realized_gain, from.st_realized_gain);
    into.lt_sold_basis =
        gnc_numeric_add_fixed (into.lt_sold_basis, from.lt_sold_basis);
    into.lt_sold_value =
        gnc_numeric_add_fixed (into.lt_sold_value, from.lt_sold_value);
    into.lt_realized_gain =
        gnc_numeric_add_fixed (into.lt_realized_gain, from.lt_realized_gain);
    into.weighted_realized_cagr += from.weighted_realized_cagr;
    into.end_basis = gnc_numeric_add_fixed (into.end_basis, from.end_basis);
    into.end_value = gnc_numeric_add_fixed (into.end_value, from.end_value);
    into.unrealized_gain =
        gnc_numeric_add_fixed (into.unrealized_gain, from.unrealized_gain);
    into.st_unrealized_gain = gnc_numeric_add_fixed (into.st_unrealized_gain,
                                                     from.st_unrealized_gain);
    into.lt_unrealized_gain = gnc_numeric_add_fixed (into.lt_unrealized_gain,
                                                     from.lt_unrealized_gain);
    into.has_warnings = into.has_warnings || from.has_warnings;
    into.active = into.active || from.active;

    if (into.kind != LotStats::Kind::GrandTotal)
    {
        into.old_bought_amount = gnc_numeric_add_fixed (into.old_bought_amount,
                                                        from.old_bought_amount);
        into.bought_amount =
            gnc_numeric_add_fixed (into.bought_amount, from.bought_amount);
        into.st_sold_amount =
            gnc_numeric_add_fixed (into.st_sold_amount, from.st_sold_amount);
        into.lt_sold_amount =
            gnc_numeric_add_fixed (into.lt_sold_amount, from.lt_sold_amount);
        into.end_amount =
            gnc_numeric_add_fixed (into.end_amount, from.end_amount);
        if (into.currency == nullptr)
            into.currency = from.currency;
    }

    /* The Scheme report weights a lot's unrealised CAGR by its end amount as
     * the lot's row is added, so only the lots a report shows contribute. */
    if (from.kind == LotStats::Kind::Lot && from.unrealized_cagr.has_value ())
        into.weighted_unrealized_cagr +=
            *from.unrealized_cagr * gnc_numeric_to_double (from.end_amount);
}

std::vector<Split*>
account_splits_until (Account* account, time64 end)
{
    std::vector<Split*> splits;
    for (GList* node = xaccAccountGetSplitList (account); node != nullptr;
         node = node->next)
    {
        auto* split = static_cast<Split*> (node->data);
        Transaction* txn = xaccSplitGetParent (split);
        if (xaccTransGetVoidStatus (txn))
            continue;
        if (xaccTransGetDate (txn) > end)
            continue;
        splits.push_back (split);
    }
    return splits;
}

/* The lot's splits up to the report date, in the Scheme report's order: by
 * transaction date, purchases before sales on the same date, and the engine's
 * transaction order to break a tie between two purchases. */
std::vector<Split*>
lot_splits_until (GNCLot* lot, time64 end)
{
    std::vector<Split*> splits;
    for (GList* node = gnc_lot_get_split_list (lot); node != nullptr;
         node = node->next)
    {
        auto* split = static_cast<Split*> (node->data);
        Transaction* txn = xaccSplitGetParent (split);
        if (xaccTransGetVoidStatus (txn))
            continue;
        if (xaccTransGetDate (txn) > end)
            continue;
        splits.push_back (split);
    }

    std::stable_sort (
        splits.begin (), splits.end (),
        [] (Split* left, Split* right)
        {
            Transaction* left_txn = xaccSplitGetParent (left);
            Transaction* right_txn = xaccSplitGetParent (right);
            const time64 left_date = xaccTransGetDate (left_txn);
            const time64 right_date = xaccTransGetDate (right_txn);
            if (left_date != right_date)
                return left_date < right_date;
            const bool left_purchase =
                gnc_numeric_positive_p (xaccSplitGetAmount (left));
            const bool right_purchase =
                gnc_numeric_positive_p (xaccSplitGetAmount (right));
            if (left_purchase && right_purchase)
                return xaccTransOrder (left_txn, right_txn) < 0;
            return left_purchase && !right_purchase;
        });
    return splits;
}

/* The Scheme report's "Start date" option is a relative date whose default is
 * the last entry of the option's start-date list, START_ACCOUNTING_PERIOD.
 * The accounting period comes from the accounting-summary preference, whose
 * default (start-period 5) is the start of the current calendar year.  The
 * native layer has no preference store, and the engine's period helpers need
 * a current session that the report process does not set, so resolve the
 * preference's own default period directly. */
time64
default_calendar_year_start ()
{
    GDate* date = gnc_accounting_period_start_gdate (
        GNC_ACCOUNTING_PERIOD_CYEAR, nullptr, nullptr);
    if (date == nullptr)
        return default_start_date ();
    const time64 start = gnc_time64_get_day_start_gdate (date);
    g_date_free (date);
    return start;
}

/* True for a stock-split event, which the Scheme report recognises by the
 * split's type or its action. */
bool
stock_split_event (Split* split)
{
    const char* type = xaccSplitGetType (split);
    const char* action = xaccSplitGetAction (split);
    if (type != nullptr && g_strcmp0 (type, "stock-split") == 0)
        return true;
    return action != nullptr && g_ascii_strcasecmp (action, "split") == 0;
}

/* Accumulate one lot.  This is merge-lot()/merge-split() from the Scheme
 * report, in the same order and with the same rounding. */
LotStats
compute_lot (GNCLot* lot, Account* account, const LotOptions& options,
             gnc_commodity* report_cur, GNCPriceDB* price_db, bool have_price,
             gnc_numeric price_value, double long_term_years)
{
    LotStats stats = make_stats (LotStats::Kind::Lot, account, report_cur);
    stats.lot = lot;

    const int report_fraction = gnc_commodity_get_fraction (report_cur);

    for (Split* split : lot_splits_until (lot, options.end))
    {
        Transaction* txn = xaccSplitGetParent (split);
        const time64 date = xaccTransGetDate (txn);
        const gnc_numeric amount = xaccSplitGetAmount (split);
        const gnc_numeric value = convert_to_report_currency (
            xaccSplitGetValue (split), xaccTransGetCurrency (txn), report_cur,
            price_db, options.end);

        if (stock_split_event (split) && gnc_numeric_negative_p (amount) &&
            gnc_numeric_zero_p (value))
        {
            /* A reverse stock split: shares go down, basis and gains do not. */
            stats.end_amount =
                gnc_numeric_sub_fixed (stats.end_amount,
                                       gnc_numeric_neg (amount));
            if (date >= options.start && !stats.has_negative &&
                gnc_numeric_negative_p (stats.end_amount))
            {
                stats.has_negative = true;
                stats.first_negative_date = date;
            }
        }
        else if (gnc_numeric_positive_p (amount))
        {
            if (!stats.earliest_bought_date.has_value () ||
                date < *stats.earliest_bought_date)
            {
                stats.earliest_bought_date = date;
                stats.earliest_bought_split = split;
            }
            if (!stats.latest_bought_date.has_value () ||
                date > *stats.latest_bought_date)
                stats.latest_bought_date = date;

            stats.end_basis = gnc_numeric_add_fixed (stats.end_basis, value);
            stats.end_amount = gnc_numeric_add_fixed (stats.end_amount, amount);

            if (date >= options.start)
            {
                stats.bought_amount =
                    gnc_numeric_add_fixed (stats.bought_amount, amount);
                stats.bought_value =
                    gnc_numeric_add_fixed (stats.bought_value, value);
            }
            else
            {
                stats.old_bought_amount =
                    gnc_numeric_add_fixed (stats.old_bought_amount, amount);
                stats.old_bought_value =
                    gnc_numeric_add_fixed (stats.old_bought_value, value);
            }
            ++stats.bought_split_count;
        }
        else if (gnc_numeric_negative_p (amount))
        {
            /* A sale: its basis is its share of the lot's running basis,
             * rounded to the report currency, and its gain is the proceeds
             * minus that basis. */
            const gnc_numeric sold_amount = gnc_numeric_neg (amount);
            const gnc_numeric sold_value = gnc_numeric_neg (value);
            const gnc_numeric sold_fraction = gnc_numeric_div (
                sold_amount, stats.end_amount, GNC_DENOM_AUTO,
                GNC_HOW_DENOM_REDUCE);
            const gnc_numeric basis = gnc_numeric_mul (
                sold_fraction, stats.end_basis, report_fraction,
                GNC_HOW_RND_ROUND);
            const gnc_numeric gain = gnc_numeric_sub (
                sold_value, basis, report_fraction, GNC_HOW_RND_ROUND);
            const bool long_term =
                is_long_term (stats.latest_bought_date, date, long_term_years);

            stats.end_basis = gnc_numeric_sub_fixed (stats.end_basis, basis);
            stats.end_amount =
                gnc_numeric_sub_fixed (stats.end_amount, sold_amount);

            if (date >= options.start)
            {
                if (!stats.has_negative &&
                    gnc_numeric_negative_p (stats.end_amount))
                {
                    stats.has_negative = true;
                    stats.first_negative_date = date;
                }

                std::optional<double> years;
                if (stats.latest_bought_date.has_value ())
                    years = date_year_delta (*stats.latest_bought_date, date);
                const std::optional<double> cagr =
                    calculate_cagr (basis, sold_value, years);
                /* A sale with no meaningful CAGR (bought and sold the same
                 * day, say) does not contribute to the weighted total. */
                if (cagr.has_value ())
                    stats.weighted_realized_cagr +=
                        *cagr * gnc_numeric_to_double (sold_amount);

                if (long_term)
                {
                    stats.lt_sold_amount =
                        gnc_numeric_add_fixed (stats.lt_sold_amount,
                                               sold_amount);
                    stats.lt_sold_basis =
                        gnc_numeric_add_fixed (stats.lt_sold_basis, basis);
                    stats.lt_sold_value =
                        gnc_numeric_add_fixed (stats.lt_sold_value, sold_value);
                    stats.lt_realized_gain =
                        gnc_numeric_add_fixed (stats.lt_realized_gain, gain);
                }
                else
                {
                    stats.st_sold_amount =
                        gnc_numeric_add_fixed (stats.st_sold_amount,
                                               sold_amount);
                    stats.st_sold_basis =
                        gnc_numeric_add_fixed (stats.st_sold_basis, basis);
                    stats.st_sold_value =
                        gnc_numeric_add_fixed (stats.st_sold_value, sold_value);
                    stats.st_realized_gain =
                        gnc_numeric_add_fixed (stats.st_realized_gain, gain);
                }

                ++stats.sold_split_count;
                stats.last_sold_split = split;
                stats.last_sold_date = date;
            }
        }
        else if (date >= options.start)
        {
            /* A zero-amount split in the lot is a "Realized Gain/Loss" split;
             * its value is what the report checks its own gains against. */
            stats.splits_realized_gain =
                gnc_numeric_add_fixed (stats.splits_realized_gain, value);
        }
    }

    stats.end_value =
        have_price ? gnc_numeric_mul (stats.end_amount, price_value,
                                      report_fraction, GNC_HOW_RND_ROUND)
                   : zero_in (report_cur);
    stats.unrealized_gain =
        gnc_numeric_sub_fixed (stats.end_value, stats.end_basis);

    const bool lot_long_term =
        is_long_term (stats.latest_bought_date, options.end, long_term_years);
    stats.st_unrealized_gain =
        lot_long_term ? zero_in (report_cur) : stats.unrealized_gain;
    stats.lt_unrealized_gain =
        lot_long_term ? stats.unrealized_gain : zero_in (report_cur);

    stats.active = stats.sold_split_count > 0 ||
                   gnc_numeric_positive_p (stats.end_amount);

    std::optional<double> years;
    if (stats.latest_bought_date.has_value ())
        years = date_year_delta (*stats.latest_bought_date, options.end);
    stats.unrealized_cagr = calculate_cagr (stats.end_basis, stats.end_value,
                                            years);
    return stats;
}

/* -------------------------------------------------- investment lots cells */

Cell
blank_cell ()
{
    return Cell {};
}

Cell
label_cell (const std::string& text, const char* css, int span = 1)
{
    Cell cell;
    cell.text = text;
    cell.css = css;
    cell.span = span;
    return cell;
}

/* to-cell() in the Scheme report: an amount is a blank cell when it is zero
 * and the row is not a total row, because "Show blanks instead of zeros"
 * defaults to on. */
Cell
value_cell (gnc_numeric value, gnc_commodity* currency, bool bold)
{
    if (!bold && gnc_numeric_zero_p (value))
        return blank_cell ();
    Cell cell;
    cell.text = format_amount (value, currency);
    cell.css = bold ? "total-number-cell" : "number-cell";
    if (gnc_numeric_negative_p (value))
        cell.css += " neg";
    return cell;
}

Cell
integer_cell (int value, bool bold)
{
    if (!bold && value == 0)
        return blank_cell ();
    Cell cell;
    cell.text = std::to_string (value);
    cell.css = bold ? "total-number-cell" : "number-cell";
    return cell;
}

Cell
percent_cell (std::optional<double> fraction, bool bold)
{
    if (!fraction.has_value ())
        return blank_cell ();
    if (!bold && *fraction == 0.0)
        return blank_cell ();
    char text[32] = {0};
    g_snprintf (text, sizeof text, "%.1f%%", *fraction * 100.0);
    Cell cell;
    cell.text = text;
    cell.css = bold ? "total-number-cell" : "number-cell";
    return cell;
}

/* value/amount (the report's "average price"), or zero when undefined. */
gnc_numeric
average_price (gnc_numeric amount, gnc_numeric value,
               gnc_commodity* report_cur)
{
    if (gnc_numeric_zero_p (amount))
        return zero_in (report_cur);
    return gnc_numeric_div (value, amount, GNC_DENOM_AUTO,
                            GNC_HOW_DENOM_REDUCE);
}

std::string
printed_date (time64 when)
{
    char date[32] = {0};
    qof_print_date_buff (date, sizeof date, when);
    return std::string (date);
}

/* get-lot-warnings(): the lot-level validation messages, with the options at
 * their defaults (multiple bought splits, negative balance, gains mismatch). */
std::vector<std::string>
lot_warnings (const LotStats& stats, gnc_commodity* report_cur)
{
    std::vector<std::string> warnings;

    if (stats.bought_split_count > 1)
        warnings.push_back (
            "Warning: Above lot has " +
            std::to_string (stats.bought_split_count) +
            " bought splits. Consider separating them into their own lots.");

    if (stats.has_negative && stats.first_negative_date.has_value ())
        warnings.push_back (
            "Warning: Above lot's balance is negative on " +
            printed_date (*stats.first_negative_date) +
            ". Consider removing the responsible sale split from the lot and "
            "then scrubbing.");

    if (stats.bought_split_count > 0)
    {
        const gnc_numeric computed = gnc_numeric_add_fixed (
            stats.st_realized_gain, stats.lt_realized_gain);
        const gnc_numeric discrepancy =
            gnc_numeric_sub_fixed (computed, stats.splits_realized_gain);
        if (!gnc_numeric_zero_p (discrepancy))
            warnings.push_back (
                "Warning: Above lot's computed gain " +
                format_amount (computed, report_cur) +
                " is not equal to the \"Realized Gain/Loss\" split(s) sum " +
                format_amount (stats.splits_realized_gain, report_cur) +
                ". Difference: " + format_amount (discrepancy, report_cur));
    }
    return warnings;
}

/* get-account-warnings(): splits outside any lot, and a lot total that does
 * not equal the account's own balance. */
std::vector<std::string>
account_warnings (const LotStats& stats, const std::vector<Split*>& unassigned,
                  time64 end, gnc_commodity* report_cur)
{
    std::vector<std::string> warnings;

    if (!unassigned.empty ())
        warnings.push_back (
            "Warning: " + std::to_string (unassigned.size ()) +
            " split(s) are not assigned to a lot. Do lots need to be "
            "scrubbed?");

    const gnc_numeric balance =
        xaccAccountGetBalanceAsOfDate (stats.account, end);
    const gnc_numeric discrepancy =
        gnc_numeric_sub_fixed (stats.end_amount, balance);
    if (!gnc_numeric_zero_p (discrepancy))
        warnings.push_back (
            "Warning: End amount " +
            format_amount (stats.end_amount, stats.currency) +
            " is not equal to actual account balance " +
            format_amount (balance, stats.currency) + ". Difference: " +
            format_amount (discrepancy, stats.currency) +
            ". Do lots need to be scrubbed?");
    return warnings;
}

void
append_warning_rows (CellTable& table, const std::vector<std::string>& warnings,
                     int column_count)
{
    for (const std::string& warning : warnings)
        table.rows.push_back (
            {label_cell (warning, "total-label-cell neg", column_count)});
}

/* One row of the lots table.  `grand_total` blanks the share amounts and the
 * two CAGR columns, because accounts with different commodities cannot be
 * added together (the Scheme report's get-column-header-list makes the same
 * distinction for its headings). */
void
append_stats_row (CellTable& table, const LotStats& stats, bool bold,
                  bool grand_total, gnc_commodity* report_cur)
{
    std::vector<Cell> row;

    std::string label;
    if (stats.kind == LotStats::Kind::Lot)
    {
        const char* title = stats.lot != nullptr ? gnc_lot_get_title (stats.lot)
                                                 : nullptr;
        label = title != nullptr ? title : "";
    }
    else if (stats.kind == LotStats::Kind::Account)
        label = "Account Lots Total";
    else
        label = "Grand Total";
    row.push_back (bold ? label_cell (label, "total-number-cell")
                        : label_cell (label, ""));

    /* Opened and Closed: only a lot has them.  A closed lot's close date is
     * the last sale that took its amount to zero. */
    if (stats.kind == LotStats::Kind::Lot &&
        stats.earliest_bought_date.has_value ())
        row.push_back (label_cell (printed_date (*stats.earliest_bought_date),
                                   "number-cell"));
    else
        row.push_back (blank_cell ());

    if (stats.kind == LotStats::Kind::Lot &&
        gnc_numeric_zero_p (stats.end_amount) &&
        stats.last_sold_date.has_value ())
        row.push_back (label_cell (printed_date (*stats.last_sold_date),
                                   "number-cell"));
    else
        row.push_back (blank_cell ());

    /* Bought. */
    if (grand_total)
        row.push_back (blank_cell ());
    else
        row.push_back (value_cell (stats.bought_amount, stats.currency, bold));
    row.push_back (value_cell (stats.bought_value, report_cur, bold));
    /* The average price needs the amount, and the grand total has none, so
     * its price cells are blank (the Scheme report passes #f there). */
    if (grand_total)
        row.push_back (blank_cell ());
    else
        row.push_back (value_cell (
            average_price (stats.bought_amount, stats.bought_value, report_cur),
            report_cur, bold));

    /* Sold: the split count, then the amount/basis/value and their average
     * price. */
    row.push_back (integer_cell (stats.sold_split_count, bold));
    const gnc_numeric sold_amount = gnc_numeric_add_fixed (
        stats.st_sold_amount, stats.lt_sold_amount);
    const gnc_numeric sold_basis = gnc_numeric_add_fixed (stats.st_sold_basis,
                                                          stats.lt_sold_basis);
    const gnc_numeric sold_value = gnc_numeric_add_fixed (stats.st_sold_value,
                                                          stats.lt_sold_value);
    if (grand_total)
        row.push_back (blank_cell ());
    else
        row.push_back (value_cell (sold_amount, stats.currency, bold));
    row.push_back (value_cell (sold_basis, report_cur, bold));
    row.push_back (value_cell (sold_value, report_cur, bold));
    if (grand_total)
        row.push_back (blank_cell ());
    else
        row.push_back (value_cell (
            average_price (sold_amount, sold_value, report_cur), report_cur,
            bold));

    /* End. */
    if (grand_total)
        row.push_back (blank_cell ());
    else
        row.push_back (value_cell (stats.end_amount, stats.currency, bold));
    row.push_back (value_cell (stats.end_basis, report_cur, bold));
    row.push_back (value_cell (stats.end_value, report_cur, bold));

    /* Realised gain: short and long term, then the weighted CAGR. */
    row.push_back (value_cell (stats.st_realized_gain, report_cur, bold));
    row.push_back (value_cell (stats.lt_realized_gain, report_cur, bold));
    if (grand_total)
        row.push_back (blank_cell ());
    else if (gnc_numeric_zero_p (sold_amount))
        row.push_back (integer_cell (0, bold));
    else
        row.push_back (percent_cell (stats.weighted_realized_cagr /
                                         gnc_numeric_to_double (sold_amount),
                                     bold));

    /* Unrealised gain, the same way; a lot's CAGR is its own, an account's is
     * weighted by the lots' end amounts. */
    row.push_back (value_cell (stats.st_unrealized_gain, report_cur, bold));
    row.push_back (value_cell (stats.lt_unrealized_gain, report_cur, bold));
    if (grand_total)
        row.push_back (blank_cell ());
    else if (stats.kind == LotStats::Kind::Lot)
        row.push_back (percent_cell (stats.unrealized_cagr, bold));
    else if (gnc_numeric_zero_p (stats.end_amount))
        row.push_back (integer_cell (0, bold));
    else
        row.push_back (percent_cell (stats.weighted_unrealized_cagr /
                                         gnc_numeric_to_double (stats.end_amount),
                                     bold));

    table.rows.push_back (std::move (row));
}

/* The 20 column headings, and the same list with only the columns a grand
 * total can add up (the amount columns are blank there). */
const char* const lot_headings[] = {
    "Lot Title",           "Opened",          "Closed",
    "Bought Amount",       "Bought Value (Basis)", "Bought Average Price",
    "Sold Splits",         "Sold Amount",     "Sold Basis",
    "Sold Value",          "Sold Average Price", "End Amount",
    "End Basis",           "End Value",       "ST Realized Gain",
    "LT Realized Gain",    "Realized CAGR",   "ST Unrealized Gain",
    "LT Unrealized Gain",  "Unrealized CAGR"};

const bool lot_heading_totals[] = {
    false, false, false, false, true,  false, true,  false, true,  true,
    false, false, true,  true,  true,  true,  false, true,  true,  false};

void
append_heading_row (CellTable& table, bool grand_total)
{
    std::vector<Cell> row;
    for (int index = 0; index < 20; ++index)
        row.push_back (label_cell (
            grand_total && !lot_heading_totals[index] ? "" : lot_headings[index],
            "column-heading-center"));
    table.rows.push_back (std::move (row));
}

void
append_padding_rows (CellTable& table, int count, int column_count)
{
    for (int index = 0; index < count; ++index)
        table.rows.push_back ({label_cell ("", "", column_count)});
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

LotReport
investment_lots (Account* root, const LotOptions& options,
                 gnc_commodity* requested)
{
    LotReport report;
    report.title = "Investment Lots";

    gchar* interval = gnc_date_interval_format (options.start, options.end);
    if (interval != nullptr)
    {
        report.title += ", ";
        report.title += interval;
        g_free (interval);
    }

    gnc_commodity* report_cur = report_currency (root, requested);
    if (root == nullptr || report_cur == nullptr)
        return report;

    GNCPriceDB* price_db =
        gnc_pricedb_get_db (qof_instance_get_book (QOF_INSTANCE (root)));

    /* The Scheme report's "Long term gains age (years)" default. */
    const double long_term_years = 1.0;
    const int column_count = 20;

    CellTable& table = report.table;
    LotStats grand =
        make_stats (LotStats::Kind::GrandTotal, nullptr, report_cur);

    std::vector<Account*> accounts;
    collect_accounts (root, accounts);

    bool have_account_selection = false;
    for (Account* account : accounts)
    {
        const GNCAccountType type = xaccAccountGetType (account);
        /* The Scheme report's account option defaults to every stock and
         * mutual fund account.  When the book has none, the option machinery
         * falls back to the first account of an allowed type (asset, bank,
         * stock or mutual) rather than to an empty selection, and the report
         * then prints an empty grand-total section instead of nothing at
         * all.  Only stock and mutual fund accounts can have lots worth
         * reporting, so the fallback account contributes no rows. */
        if (type == ACCT_TYPE_ASSET || type == ACCT_TYPE_BANK ||
            type == ACCT_TYPE_STOCK || type == ACCT_TYPE_MUTUAL)
            have_account_selection = true;
        if (type != ACCT_TYPE_STOCK && type != ACCT_TYPE_MUTUAL)
            continue;

        gnc_commodity* account_currency = xaccAccountGetCommodity (account);
        if (account_currency == nullptr)
            continue;

        PriceHit price =
            find_price (price_db, account_currency, report_cur, options.end);
        gnc_numeric price_value = zero_in (report_cur);
        if (price.price != nullptr)
            price_value = convert_to_report_currency (
                gnc_price_get_value (price.price),
                gnc_price_get_currency (price.price), report_cur, price_db,
                options.end);

        std::vector<Split*> splits =
            account_splits_until (account, options.end);
        std::vector<GNCLot*> lots;
        std::set<GNCLot*> seen;
        std::vector<Split*> unassigned;
        for (Split* split : splits)
        {
            GNCLot* lot = xaccSplitGetLot (split);
            if (lot == nullptr)
            {
                unassigned.push_back (split);
                continue;
            }
            if (seen.insert (lot).second)
                lots.push_back (lot);
        }

        /* "Include accounts with no lots" defaults to false. */
        if (lots.empty ())
        {
            if (price.price != nullptr)
                gnc_price_unref (price.price);
            continue;
        }

        /* One account's rows are built aside and only copied into the report
         * when the account has something to show, as the Scheme report's
         * temporary account table does. */
        CellTable section;

        gchar* full_name = gnc_account_get_full_name (account);
        section.rows.push_back (
            {label_cell (full_name != nullptr ? full_name
                                              : account_name (account),
                         "column-heading-left", column_count)});
        g_free (full_name);

        std::string price_line = "No price found";
        if (price.price != nullptr)
        {
            gnc_commodity* price_currency = gnc_price_get_currency (price.price);
            price_line = "End price: ";
            price_line +=
                format_amount (gnc_price_get_value (price.price), price_currency);
            if (!gnc_commodity_equiv (price_currency, report_cur))
            {
                price_line += " [";
                price_line += format_amount (price_value, report_cur);
                price_line += "]";
            }
            price_line += " on ";
            price_line += printed_date (gnc_price_get_time64 (price.price));
        }
        section.rows.push_back (
            {label_cell (price_line, "text-cell", column_count)});

        append_heading_row (section, false);

        LotStats account_stats =
            make_stats (LotStats::Kind::Account, account, report_cur);
        /* An account with splits outside any lot is shown even when its lots
         * are not, because those splits are a warning about the account. */
        account_stats.active = !unassigned.empty ();

        for (GNCLot* lot : lots)
        {
            LotStats stats = compute_lot (lot, account, options, report_cur,
                                          price_db, price.price != nullptr,
                                          price_value, long_term_years);
            /* "Include closed lots" defaults to true, so every lot active in
             * the window is shown. */
            if (!stats.active)
                continue;

            const std::vector<std::string> warnings =
                lot_warnings (stats, report_cur);
            stats.has_warnings = !warnings.empty ();
            append_stats_row (section, stats, false, false, report_cur);
            append_warning_rows (section, warnings, column_count);
            merge_stats (account_stats, stats);
        }

        append_stats_row (section, account_stats, true, false, report_cur);
        append_warning_rows (section,
                             account_warnings (account_stats, unassigned,
                                               options.end, report_cur),
                             column_count);

        if (account_stats.active)
        {
            append_padding_rows (table, 3, column_count);
            table.rows.insert (table.rows.end (), section.rows.begin (),
                               section.rows.end ());
            merge_stats (grand, account_stats);
        }

        if (price.price != nullptr)
            gnc_price_unref (price.price);
    }

    /* With an account selection at all the grand total section is always
     * printed, even when every lot was inactive: its totals are the report's
     * "nothing to show" statement. */
    if (have_account_selection)
    {
        append_padding_rows (table, 3, column_count);
        append_heading_row (table, true);
        append_stats_row (table, grand, true, true, report_cur);
    }
    return report;
}

/* ---------------------------------------------------------- budget report */

namespace
{

/* The Scheme report's "Account Display Depth" default: two displayed
 * levels, counted from the top-level accounts.  The Scheme account table
 * counts a *logical* depth rather than the tree depth -- an account's depth
 * grows only when its parent was itself displayed -- which is why an
 * explicitly selected deep account with no selected ancestors can still be
 * shown.  The traversal below mirrors that. */
constexpr int budget_display_depth = 2;

/* The Scheme report's default account selection: exactly these four types,
 * not the engine's debit/credit families.  A bank account (ACCT_TYPE_BANK) is
 * therefore outside the default scope, as it is in the Scheme report. */
bool
budget_default_account_type (const Account* account)
{
    switch (xaccAccountGetType (account))
    {
    case ACCT_TYPE_ASSET:
    case ACCT_TYPE_LIABILITY:
    case ACCT_TYPE_INCOME:
    case ACCT_TYPE_EXPENSE:
        return true;
    default:
        return false;
    }
}

/* gnc:get-account-period-rolledup-budget-value(): a value set on the account
 * wins (budget values are inclusive of sub-accounts, so it is not added to
 * the children's), otherwise the children's values are summed, converted into
 * this account's commodity at the period start. */
gnc_numeric
budget_rolled_up_value (const GncBudget* budget, Account* account,
                        guint period, GNCPriceDB* price_db)
{
    if (gnc_budget_is_account_period_value_set (budget, account, period))
        return gnc_budget_get_account_period_value (budget, account, period);

    GList* children = gnc_account_get_children_sorted (account);
    if (children == nullptr)
        return gnc_numeric_zero ();

    gnc_commodity* currency = xaccAccountGetCommodity (account);
    const time64 start = gnc_budget_get_period_start_date (budget, period);
    gnc_numeric total = gnc_numeric_zero ();
    for (GList* node = children; node != nullptr; node = node->next)
    {
        auto* child = static_cast<Account*> (node->data);
        gnc_numeric value = budget_rolled_up_value (budget, child, period,
                                                    price_db);
        if (currency != nullptr)
            value = gnc_pricedb_convert_balance_nearest_price_t64 (
                price_db, value, xaccAccountGetCommodity (child), currency,
                start);
        total = numeric_add (total, value);
    }
    g_list_free (children);
    return total;
}

void
collect_budget_rows (Account* parent, int logical_depth,
                     const std::set<Account*>& selected,
                     GncBudget* budget, guint period_count,
                     GNCPriceDB* price_db, CellTable& table)
{
    GList* children = gnc_account_get_children_sorted (parent);
    for (GList* node = children; node != nullptr; node = node->next)
    {
        auto* account = static_cast<Account*> (node->data);
        const bool displayed =
            selected.count (account) != 0 &&
            logical_depth < budget_display_depth;

        if (displayed)
        {
            const bool reverse = gnc_reverse_balance (account);
            gnc_commodity* commodity = xaccAccountGetCommodity (account);

            std::vector<Cell> row;
            row.push_back ({account_name (account), "text-cell", 1});
            for (guint period = 0; period < period_count; ++period)
            {
                gnc_numeric budget_value = budget_rolled_up_value (
                    budget, account, period, price_db);
                gnc_numeric actual_value =
                    gnc_budget_get_account_period_actual_value (budget, account,
                                                               period);
                if (reverse)
                {
                    budget_value = gnc_numeric_neg (budget_value);
                    actual_value = gnc_numeric_neg (actual_value);
                }

                /* The Scheme report prints "." for a zero budget, but a
                 * monetary zero for an actual that happens to be zero. */
                row.push_back ({gnc_numeric_zero_p (budget_value)
                                    ? "."
                                    : format_amount (budget_value, commodity),
                                "number-cell"});
                row.push_back (
                    {format_amount (actual_value, commodity), "number-cell"});
            }
            table.rows.push_back (std::move (row));
        }

        collect_budget_rows (account,
                             displayed ? logical_depth + 1 : logical_depth,
                             selected, budget, period_count, price_db, table);
    }
    g_list_free (children);
}

std::string
printed_short_date (time64 when)
{
    char date[32] = {0};
    qof_print_date_buff (date, sizeof date, when);
    return std::string (date);
}

/* The Budget Balance Sheet's default account selection is the Scheme report's
 * 13-type filter, not "every account": the Scheme list deliberately leaves
 * out CHECKING, SAVINGS, MONEYMRKT, CREDITLINE and TRADING. */
bool
budget_balance_sheet_account_type (const Account* account)
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
        return true;
    default:
        return false;
    }
}

bool
budget_income_statement_account_type (const Account* account)
{
    return bucket_of (account) == Bucket::Income ||
           bucket_of (account) == Bucket::Expense;
}

bool
budget_any_account_type (const Account* account)
{
    (void) account;
    return true;
}

/* The selection the report should use: the caller's already-expanded list, or
 * the report's own default filter over the sorted account tree.  Duplicates
 * are dropped in caller order so a GUID plus its own subtree is harmless. */
std::vector<Account*>
budget_selection (Account* root, const std::vector<Account*>& supplied,
                  bool (*default_filter) (const Account*))
{
    std::vector<Account*> result;
    std::set<Account*> seen;
    if (!supplied.empty ())
    {
        for (Account* account : supplied)
        {
            if (account != nullptr && seen.insert (account).second)
                result.push_back (account);
        }
        return result;
    }

    std::vector<Account*> all;
    collect_accounts (root, all);
    for (Account* account : all)
    {
        if (default_filter (account) && seen.insert (account).second)
            result.push_back (account);
    }
    return result;
}

/* A report table's visible accounts, with the Scheme account table's logical
 * depth rule: a selected account is displayed while its logical depth is
 * below the limit, and its children inherit depth+1 only when it was shown. */
void
collect_budget_display_accounts (Account* parent, int logical_depth,
                                 int max_depth,
                                 const std::set<Account*>& selected,
                                 std::vector<Account*>& out)
{
    GList* children = gnc_account_get_children_sorted (parent);
    for (GList* node = children; node != nullptr; node = node->next)
    {
        auto* account = static_cast<Account*> (node->data);
        const bool displayed =
            selected.count (account) != 0 && logical_depth < max_depth;
        if (displayed)
            out.push_back (account);
        collect_budget_display_accounts (account,
                                         displayed ? logical_depth + 1
                                                   : logical_depth,
                                         max_depth, selected, out);
    }
    g_list_free (children);
}

/* Sum of an account's own budget values over the whole budget.  The Scheme
 * statement reports use gnc:budget-account-get-net with #f/#f, which is this
 * loop; it never rolls children into a parent. */
gnc_numeric
budget_account_total (const GncBudget* budget, Account* account)
{
    const guint periods = gnc_budget_get_num_periods (budget);
    gnc_numeric total = gnc_numeric_zero ();
    for (guint period = 0; period < periods; ++period)
        total = numeric_add (total,
                             gnc_budget_get_account_period_value (budget,
                                                                  account,
                                                                  period));
    return total;
}

/* gnc:budget-account-get-initial-balance(): the account's balance immediately
 * before the budget's first period starts.  The date is exclusive in the
 * engine's as-of lookup, so a split on the first day is not "existing". */
gnc_numeric
budget_account_initial_balance (const GncBudget* budget, Account* account)
{
    return xaccAccountGetBalanceAsOfDate (
        account, gnc_budget_get_period_start_date (budget, 0));
}

gnc_numeric
budget_convert (gnc_numeric amount, gnc_commodity* from,
                gnc_commodity* report_cur, GNCPriceDB* price_db, time64 date)
{
    if (from == nullptr || report_cur == nullptr ||
        gnc_commodity_equiv (from, report_cur))
        return amount;
    return gnc_pricedb_convert_balance_nearest_before_price_t64 (
        price_db, amount, from, report_cur, date);
}

/* gnc:account-get-comm-value-at-date(account, date, #f): the sum of the
 * account's own split *values* before `date`, in the transaction currency,
 * converted to the report currency.  The balance sheet uses this only for its
 * unrealized-gain line. */
gnc_numeric
budget_account_value_before (Account* account, time64 date,
                             gnc_commodity* report_cur, GNCPriceDB* price_db)
{
    gnc_numeric total = gnc_numeric_zero ();
    for (GList* node = xaccAccountGetSplitList (account); node != nullptr;
         node = node->next)
    {
        auto* split = static_cast<Split*> (node->data);
        Transaction* transaction = xaccSplitGetParent (split);
        if (transaction == nullptr || xaccTransGetDate (transaction) >= date)
            continue;
        const gnc_numeric value = xaccSplitGetValue (split);
        total = numeric_add (
            total,
            budget_convert (value, xaccTransGetCurrency (transaction),
                            report_cur, price_db, date));
    }
    return total;
}

bool
budget_account_has_children (Account* account)
{
    GList* children = gnc_account_get_children (account);
    const bool has_children = children != nullptr;
    g_list_free (children);
    return has_children;
}

/* The names gnc:account-get-type-string-plural() uses for the six buckets
 * gnc:decompose-accountlist() produces, in its order. */
const char*
budget_bucket_name (Bucket bucket)
{
    switch (bucket)
    {
    case Bucket::Asset:     return "Assets";
    case Bucket::Liability: return "Liabilities";
    case Bucket::Equity:    return "Equities";
    case Bucket::Income:    return "Income";
    case Bucket::Expense:   return "Expenses";
    case Bucket::Trading:   return "Trading Accounts";
    default:                return "";
    }
}

} // namespace

CellTable
budget_report (Account* root, GncBudget* budget,
               const std::vector<Account*>& accounts)
{
    CellTable table;
    if (root == nullptr || budget == nullptr)
        return table;

    const guint period_count = gnc_budget_get_num_periods (budget);

    /* The headings flatten the Scheme report's two header rows (the period
     * date with a colspan, then Bgt/Act) into one label per column.  The
     * parity harness compares the amounts and the account labels, not the
     * header markup. */
    table.headings.push_back ("Account");
    for (guint period = 0; period < period_count; ++period)
    {
        const std::string date = printed_short_date (
            gnc_budget_get_period_start_date (budget, period));
        table.headings.push_back ("Bgt " + date);
        table.headings.push_back ("Act " + date);
    }

    std::set<Account*> selected;
    if (accounts.empty ())
    {
        std::vector<Account*> all;
        collect_accounts (root, all);
        for (Account* account : all)
        {
            if (budget_default_account_type (account))
                selected.insert (account);
        }
    }
    else
    {
        for (Account* account : accounts)
            selected.insert (account);
    }

    if (selected.empty ())
        return table;

    GNCPriceDB* price_db =
        gnc_pricedb_get_db (qof_instance_get_book (QOF_INSTANCE (root)));
    collect_budget_rows (root, 0, selected, budget, period_count, price_db,
                         table);
    return table;
}

CellTable
budget_income_statement (Account* root, GncBudget* budget,
                         const std::vector<Account*>& accounts,
                         gnc_commodity* requested_currency)
{
    CellTable table;
    if (root == nullptr || budget == nullptr)
        return table;

    gnc_commodity* currency = report_currency (root, requested_currency);
    GNCPriceDB* price_db =
        gnc_pricedb_get_db (qof_instance_get_book (QOF_INSTANCE (root)));
    const time64 date = gnc_budget_get_period_start_date (budget, 0);

    const std::vector<Account*> selected =
        budget_selection (root, accounts, budget_income_statement_account_type);
    if (selected.empty ())
        return table;

    std::set<Account*> selected_set (selected.begin (), selected.end ());
    std::vector<Account*> displayed;
    collect_budget_display_accounts (root, 0, 3, selected_set, displayed);

    auto converted_total = [&] (Account* account)
    {
        return budget_convert (budget_account_total (budget, account),
                               xaccAccountGetCommodity (account), currency,
                               price_db, date);
    };
    auto add_heading = [&table] (const char* label)
    {
        std::vector<Cell> row;
        row.push_back ({label, "total-label-cell"});
        table.rows.push_back (std::move (row));
    };
    auto add_total = [&table, currency] (const char* label, gnc_numeric amount)
    {
        std::vector<Cell> row;
        row.push_back ({label, "total-label-cell"});
        row.push_back ({format_amount (amount, currency), "total-number-cell"});
        table.rows.push_back (std::move (row));
    };

    auto add_section = [&] (const char* heading, Bucket bucket)
    {
        add_heading (heading);
        for (Account* account : displayed)
        {
            if (bucket_of (account) != bucket)
                continue;
            gnc_numeric amount = converted_total (account);
            if (shows_reversed (account))
                amount = gnc_numeric_neg (amount);
            std::vector<Cell> row;
            row.push_back ({account_name (account), "text-cell"});
            row.push_back ({format_amount (amount, currency), "number-cell"});
            table.rows.push_back (std::move (row));
        }
    };

    add_section ("Revenues", Bucket::Income);
    gnc_numeric revenue_total = gnc_numeric_zero ();
    for (Account* account : selected)
    {
        if (bucket_of (account) != Bucket::Income)
            continue;
        gnc_numeric amount = converted_total (account);
        if (shows_reversed (account))
            amount = gnc_numeric_neg (amount);
        revenue_total = numeric_add (revenue_total, amount);
    }
    add_total ("Total Revenue", revenue_total);

    add_section ("Expenses", Bucket::Expense);
    gnc_numeric expense_total = gnc_numeric_zero ();
    for (Account* account : selected)
    {
        if (bucket_of (account) != Bucket::Expense)
            continue;
        expense_total = numeric_add (expense_total, converted_total (account));
    }
    add_total ("Total Expenses", expense_total);

    /* The Scheme net line is the negation of the sum of both sections' raw
     * (not sign-reversed) budget values.  A negative result is printed as the
     * absolute "Net loss", exactly as report-line does. */
    gnc_numeric net = gnc_numeric_zero ();
    for (Account* account : selected)
    {
        const Bucket bucket = bucket_of (account);
        if (bucket == Bucket::Income || bucket == Bucket::Expense)
            net = numeric_add (net, converted_total (account));
    }
    net = gnc_numeric_neg (net);

    const gchar* name = gnc_budget_get_name (budget);
    const std::string budget_name =
        name != nullptr && name[0] != '\0' ? std::string (name)
                                           : std::string ("Budget");
    std::string label = "Net income for Budget " + budget_name;
    if (gnc_numeric_negative_p (net))
    {
        label = "Net loss for Budget " + budget_name;
        net = gnc_numeric_neg (net);
    }
    std::vector<Cell> net_row;
    net_row.push_back ({label, "text-cell"});
    net_row.push_back ({format_amount (net, currency), "number-cell"});
    table.rows.push_back (std::move (net_row));

    return table;
}

CellTable
budget_balance_sheet (Account* root, GncBudget* budget,
                      const std::vector<Account*>& accounts,
                      gnc_commodity* requested_currency)
{
    CellTable table;
    if (root == nullptr || budget == nullptr)
        return table;

    gnc_commodity* currency = report_currency (root, requested_currency);
    GNCPriceDB* price_db =
        gnc_pricedb_get_db (qof_instance_get_book (QOF_INSTANCE (root)));
    const time64 date = gnc_budget_get_period_start_date (budget, 0);

    const std::vector<Account*> selected =
        budget_selection (root, accounts, budget_balance_sheet_account_type);
    if (selected.empty ())
        return table;

    std::set<Account*> selected_set (selected.begin (), selected.end ());
    std::vector<Account*> displayed;
    collect_budget_display_accounts (root, 0, 3, selected_set, displayed);

    auto converted_initial = [&] (Account* account)
    {
        return budget_convert (budget_account_initial_balance (budget, account),
                               xaccAccountGetCommodity (account), currency,
                               price_db, date);
    };
    auto converted_budget = [&] (Account* account)
    {
        return budget_convert (budget_account_total (budget, account),
                               xaccAccountGetCommodity (account), currency,
                               price_db, date);
    };
    auto add_heading = [&table] (const char* label)
    {
        std::vector<Cell> row;
        row.push_back ({label, "total-label-cell"});
        table.rows.push_back (std::move (row));
    };
    auto add_total = [&table, currency] (const char* positive_label,
                                         const char* negative_label,
                                         gnc_numeric amount)
    {
        std::string label = positive_label;
        if (negative_label != nullptr && gnc_numeric_negative_p (amount))
        {
            label = negative_label;
            amount = gnc_numeric_neg (amount);
        }
        std::vector<Cell> row;
        row.push_back ({label, "total-label-cell"});
        row.push_back ({format_amount (amount, currency), "total-number-cell"});
        table.rows.push_back (std::move (row));
    };
    auto add_account_rows = [&] (Bucket bucket)
    {
        for (Account* account : displayed)
        {
            if (bucket_of (account) != bucket)
                continue;
            gnc_numeric amount =
                numeric_add (converted_initial (account),
                             converted_budget (account));
            if (shows_reversed (account))
                amount = gnc_numeric_neg (amount);
            std::vector<Cell> row;
            row.push_back ({account_name (account), "text-cell"});
            row.push_back ({format_amount (amount, currency), "number-cell"});
            table.rows.push_back (std::move (row));
        }
    };
    auto sum_initial = [&] (Bucket bucket)
    {
        gnc_numeric total = gnc_numeric_zero ();
        for (Account* account : selected)
        {
            if (bucket_of (account) == bucket)
                total = numeric_add (total, converted_initial (account));
        }
        return total;
    };
    auto sum_budget = [&] (Bucket bucket)
    {
        gnc_numeric total = gnc_numeric_zero ();
        for (Account* account : selected)
        {
            if (bucket_of (account) == bucket)
                total = numeric_add (total, converted_budget (account));
        }
        return total;
    };

    const gnc_numeric existing_assets = sum_initial (Bucket::Asset);
    const gnc_numeric allocated_assets = sum_budget (Bucket::Asset);
    const gnc_numeric existing_liabilities = sum_initial (Bucket::Liability);
    const gnc_numeric new_liabilities = sum_budget (Bucket::Liability);
    const gnc_numeric liability_balance =
        numeric_add (existing_liabilities, new_liabilities);

    /* Existing retained earnings are initial income minus initial expenses;
     * new retained earnings are budget income minus budget expenses.  The
     * Scheme collector- makes both of these the negation of the sum. */
    const gnc_numeric existing_retained = gnc_numeric_neg (
        numeric_add (sum_initial (Bucket::Income),
                     sum_initial (Bucket::Expense)));
    const gnc_numeric new_retained = gnc_numeric_neg (
        numeric_add (sum_budget (Bucket::Income),
                     sum_budget (Bucket::Expense)));
    const gnc_numeric retained =
        numeric_add (existing_retained, new_retained);

    gnc_numeric asset_basis = gnc_numeric_zero ();
    gnc_numeric liability_basis = gnc_numeric_zero ();
    for (Account* account : selected)
    {
        const Bucket bucket = bucket_of (account);
        if (bucket == Bucket::Asset)
            asset_basis = numeric_add (
                asset_basis,
                budget_account_value_before (account, date, currency,
                                             price_db));
        else if (bucket == Bucket::Liability)
            liability_basis = numeric_add (
                liability_basis,
                budget_account_value_before (account, date, currency,
                                             price_db));
    }
    const gnc_numeric unrealized_gain = numeric_add (
        numeric_add (existing_assets, gnc_numeric_neg (asset_basis)),
        gnc_numeric_neg (numeric_add (
            existing_liabilities, gnc_numeric_neg (liability_basis))));

    const gnc_numeric existing_equity = numeric_add (
        numeric_add (gnc_numeric_neg (sum_initial (Bucket::Equity)),
                     existing_retained),
        unrealized_gain);
    const gnc_numeric new_equity =
        numeric_add (sum_budget (Bucket::Equity), new_retained);
    const gnc_numeric equity_balance =
        numeric_add (existing_equity, new_equity);

    const gnc_numeric unallocated_assets = numeric_add (
        new_retained,
        gnc_numeric_neg (numeric_add (allocated_assets, new_liabilities)));
    const gnc_numeric asset_balance =
        numeric_add (numeric_add (existing_assets, allocated_assets),
                     unallocated_assets);
    const gnc_numeric liability_plus_equity = numeric_add (
        gnc_numeric_neg (liability_balance), equity_balance);

    add_heading ("Assets");
    add_account_rows (Bucket::Asset);
    add_total ("Existing Assets", nullptr, existing_assets);
    add_total ("Allocated Assets", nullptr, allocated_assets);
    if (!gnc_numeric_zero_p (unallocated_assets))
        add_total ("Unallocated Assets", nullptr, unallocated_assets);
    add_total ("Total Assets", nullptr, asset_balance);

    add_heading ("Liabilities");
    add_account_rows (Bucket::Liability);
    add_total ("Existing Liabilities", nullptr,
               gnc_numeric_neg (existing_liabilities));
    add_total ("New Liabilities", nullptr, gnc_numeric_neg (new_liabilities));
    add_total ("Total Liabilities", nullptr,
               gnc_numeric_neg (liability_balance));

    add_heading ("Equity");
    add_account_rows (Bucket::Equity);
    if (!gnc_numeric_zero_p (retained))
    {
        add_total ("Existing Retained Earnings", "Existing Retained Losses",
                   existing_retained);
        add_total ("New Retained Earnings", "New Retained Losses",
                   new_retained);
    }
    if (!gnc_numeric_zero_p (unrealized_gain))
        add_total ("Unrealized Gains", "Unrealized Losses", unrealized_gain);
    add_total ("Existing Equity", nullptr, existing_equity);
    add_total ("New Equity", nullptr, new_equity);
    add_total ("Total Equity", nullptr, equity_balance);
    add_total ("Total Liabilities & Equity", nullptr, liability_plus_equity);

    return table;
}

CellTable
budget_flow (Account* root, GncBudget* budget,
             const std::vector<Account*>& accounts,
             gnc_commodity* requested_currency)
{
    CellTable table;
    if (root == nullptr || budget == nullptr)
        return table;

    gnc_commodity* currency = report_currency (root, requested_currency);
    GNCPriceDB* price_db =
        gnc_pricedb_get_db (qof_instance_get_book (QOF_INSTANCE (root)));
    const guint period = 0;
    const time64 date = gnc_budget_get_period_end_date (budget, period);

    const std::vector<Account*> selected =
        budget_selection (root, accounts, budget_any_account_type);
    if (selected.empty ())
        return table;

    struct TypeTotal
    {
        Bucket bucket;
        gnc_numeric budget;
        gnc_numeric actual;
    };
    std::vector<TypeTotal> type_totals;

    const Bucket buckets[] = {Bucket::Asset, Bucket::Liability,
                              Bucket::Equity, Bucket::Income,
                              Bucket::Expense, Bucket::Trading};
    for (const Bucket bucket : buckets)
    {
        gnc_numeric budget_total = gnc_numeric_zero ();
        gnc_numeric actual_total = gnc_numeric_zero ();
        for (Account* account : selected)
        {
            if (bucket_of (account) != bucket ||
                budget_account_has_children (account))
                continue;

            gnc_commodity* commodity = xaccAccountGetCommodity (account);
            const gnc_numeric budget_value =
                gnc_budget_get_account_period_value (budget, account, period);
            const gnc_numeric actual_value =
                gnc_budget_get_account_period_actual_value (budget, account,
                                                            period);
            budget_total = numeric_add (
                budget_total,
                budget_convert (budget_value, commodity, currency, price_db,
                                date));
            actual_total = numeric_add (
                actual_total,
                budget_convert (actual_value, commodity, currency, price_db,
                                date));

            gchar* full_name = gnc_account_get_full_name (account);
            const std::string label =
                full_name != nullptr ? std::string (full_name)
                                     : account_name (account);
            std::vector<Cell> row;
            row.push_back ({label, "text-cell"});
            row.push_back ({format_amount (budget_value, commodity),
                            "number-cell"});
            row.push_back ({format_amount (actual_value, commodity),
                            "number-cell"});
            table.rows.push_back (std::move (row));
            g_free (full_name);
        }

        std::vector<Cell> total_row;
        total_row.push_back ({"Total:", "text-cell"});
        total_row.push_back ({format_amount (budget_total, currency),
                              "total-number-cell"});
        total_row.push_back ({format_amount (actual_total, currency),
                              "total-number-cell"});
        table.rows.push_back (std::move (total_row));
        type_totals.push_back ({bucket, budget_total, actual_total});
    }

    gnc_numeric budget_grand_total = gnc_numeric_zero ();
    gnc_numeric actual_grand_total = gnc_numeric_zero ();
    for (auto entry = type_totals.rbegin (); entry != type_totals.rend ();
         ++entry)
    {
        std::vector<Cell> row;
        row.push_back ({budget_bucket_name (entry->bucket), "text-cell"});
        row.push_back ({format_amount (entry->budget, currency), "number-cell"});
        row.push_back ({format_amount (entry->actual, currency), "number-cell"});
        table.rows.push_back (std::move (row));
        budget_grand_total = numeric_add (budget_grand_total, entry->budget);
        actual_grand_total = numeric_add (actual_grand_total, entry->actual);
    }

    std::vector<Cell> grand_total;
    grand_total.push_back ({"Total:", "text-cell"});
    grand_total.push_back ({format_amount (budget_grand_total, currency),
                            "total-number-cell"});
    grand_total.push_back ({format_amount (actual_grand_total, currency),
                            "total-number-cell"});
    table.rows.push_back (std::move (grand_total));

    return table;
}

std::string
render_no_budget (const std::string& title)
{
    std::ostringstream out;
    out << "<!DOCTYPE html>\n<html><head><meta charset='utf-8'><title>"
        << html_escape (title) << "</title><style>" << report_stylesheet ()
        << "</style></head><body><h3>" << html_escape (title)
        << ":</h3>\n<h3></h3>\n<p>No budgets exist. You must create at "
           "least one budget.</p>\n</body></html>\n";
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
    out << "{\"type\":\"" << (chart.type == ChartType::Line ? "line" : "bar")
        << "\",\"title\":\"" << json_escape (chart.title) << "\",\"labels\":[";
    for (size_t index = 0; index < chart.labels.size (); ++index)
    {
        if (index > 0)
            out << ",";
        out << "\"" << json_escape (chart.labels[index]) << "\"";
    }
    out << "],\"series\":[";
    for (size_t series_index = 0; series_index < chart.series.size ();
         ++series_index)
    {
        const ChartSeries& series = chart.series[series_index];
        if (series_index > 0)
            out << ",";
        out << "{\"label\":\"" << json_escape (series.label)
            << "\",\"values\":[";
        for (size_t index = 0; index < series.values.size (); ++index)
        {
            if (index > 0)
                out << ",";
            char number[G_ASCII_DTOSTR_BUF_SIZE] = {0};
            g_ascii_dtostr (number, sizeof number,
                            gnc_numeric_to_double (series.values[index]));
            out << number;
        }
        out << "]}";
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
    out << "<section class='report-chart' data-chart-type='"
        << (chart.type == ChartType::Line ? "line" : "bar") << "'>\n"
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
  var series = Array.isArray(data.series) ? data.series : [];
  var type = data.type === "line" ? "line" : "bar";
  var datasets = series.map(function (entry, seriesIndex) {
    var color = palette[seriesIndex % palette.length];
    var dataset = {
      label: entry.label,
      data: entry.values,
      borderColor: color,
      backgroundColor: color,
      borderWidth: type === "bar" ? 0 : 2
    };
    if (type === "bar" && series.length === 1) {
      dataset.backgroundColor = entry.values.map(function (_, index) {
        return palette[index % palette.length];
      });
    }
    if (type === "line") {
      dataset.fill = false;
      dataset.pointRadius = 3;
      dataset.tension = 0;
    }
    return dataset;
  });
  var chart = new Chart(canvas.getContext("2d"), {
    type: type,
    data: { labels: data.labels, datasets: datasets },
    options: {
      animation: false,
      responsive: false,
      maintainAspectRatio: false,
      plugins: {
        legend: { display: series.length > 1 },
        title: { display: false }
      },
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
                if (gnc_numeric_positive_p (
                        row_series_amount (row, Column::Debit)))
                    out << "<td class='number-cell'>"
                        << html_escape (format_amount (
                               row_series_amount (row, Column::Debit),
                               table.currency))
                        << "</td>";
                else
                    out << "<td align='right' valign='top'> </td>";
                break;

            case Column::Credit:
                if (gnc_numeric_positive_p (
                        row_series_amount (row, Column::Credit)))
                    out << "<td class='number-cell'>"
                        << html_escape (format_amount (
                               row_series_amount (row, Column::Credit),
                               table.currency))
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
    out << "<table cellspacing='0' cellpadding='4' border='0'>";
    /* A report may put its column headings in the body instead (the lots
     * report repeats a heading row per account), so the header is optional. */
    if (!table.headings.empty ())
    {
        out << "<thead><tr>";
        for (const std::string& heading : table.headings)
            out << "<th>" << html_escape (heading) << "</th>";
        out << "</tr></thead>";
    }
    out << "<tbody>\n";

    for (const std::vector<Cell>& row : table.rows)
    {
        out << "<tr>";
        for (const Cell& cell : row)
        {
            out << "<td";
            if (!cell.css.empty ())
                out << " class='" << cell.css << "'";
            if (cell.span > 1)
                out << " colspan='" << cell.span << "'";
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
            "Receivable Aging", "Payable Aging", "Investment Lots",
            "Budget Report", "Budget Income Statement",
            "Budget Profit & Loss", "Budget Balance Sheet", "Budget Flow",
            "Printable Invoice", "Receipt", "Tax Invoice",
            "Australian Tax Invoice"};
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
budget_accounts_option (const char* description)
{
    OptionDescriptor option;
    option.name = "accounts";
    option.type = OptionType::AccountList;
    option.label = "Accounts";
    option.description = description;
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

    if (name == "Printable Invoice")
    {
        /* The "Printable Invoice" layout, the base of the three Scheme
         * invoice variants.  The Scheme option pages this maps are the
         * invoice selector ("Invoice Number"), the custom title, the nine
         * display columns and the print switches.  The layout-row, CSS,
         * picture, payable-to/company-contact, extra-notes, minimum-entries,
         * detailed-tax-summary, references and job-details options are not
         * reproduced: the native document has one fixed layout, and the
         * model has no reference or job fields to print. */
        OptionDescriptor invoice;
        invoice.name = "invoice";
        invoice.type = OptionType::Invoice;
        invoice.label = "Invoice Number";
        invoice.description =
            "The invoice to print, by its document id (for example "
            "\"INV-1001\") or by its GUID.  Without it the report renders "
            "the engine's \"No valid invoice selected\" warning.";
        options.push_back (invoice);

        OptionDescriptor title;
        title.name = "title";
        title.type = OptionType::Text;
        title.label = "Custom Title";
        title.description =
            "A custom string to replace Invoice, Bill or Expense Voucher.";
        title.default_value = "";
        options.push_back (title);

        struct ColumnOption
        {
            const char* name;
            const char* label;
            const char* description;
            bool default_value;
        };
        static const ColumnOption columns[] = {
            {"show_date", "Date", "Display the date?", true},
            {"show_description", "Description", "Display the description?",
             true},
            {"show_action", "Action", "Display the action?", true},
            {"show_quantity", "Quantity", "Display the quantity of items?",
             true},
            {"show_price", "Price", "Display the price per item?", true},
            {"show_discount", "Discount", "Display the entry's discount?",
             true},
            {"show_taxable", "Taxable", "Display the entry's taxable status?",
             true},
            {"show_tax_amount", "Tax Amount",
             "Display each entry's total tax?", false},
            {"show_total", "Total", "Display the entry's value?", true},
        };
        for (const ColumnOption& column : columns)
        {
            OptionDescriptor option;
            option.name = column.name;
            option.type = OptionType::Bool;
            option.label = column.label;
            option.description = column.description;
            option.default_value = column.default_value ? "true" : "false";
            options.push_back (option);
        }

        static const ColumnOption displays[] = {
            {"show_due_date", "Due Date", "Display due date?", true},
            {"show_subtotal", "Subtotal", "Display the subtotals?", true},
            {"show_billing_id", "Billing ID", "Display the billing id?", true},
            {"show_owner_id", "Invoice owner ID",
             "Display the customer/vendor id?", false},
            {"show_notes", "Invoice Notes",
             "Display the invoice notes?  A document with no notes has "
             "nothing to show, so the option has no effect there.", false},
            {"show_terms", "Billing Terms",
             "Display the invoice billing terms?  A document with no terms "
             "has nothing to show, so the option has no effect there.", true},
            {"show_payments", "Payments",
             "Display the payments applied to this invoice?  A document with "
             "no payments has nothing to show, so the option has no effect "
             "there.", true},
        };
        for (const ColumnOption& display : displays)
        {
            OptionDescriptor option;
            option.name = display.name;
            option.type = OptionType::Bool;
            option.label = display.label;
            option.description = display.description;
            option.default_value = display.default_value ? "true" : "false";
            options.push_back (option);
        }
        return options;
    }

    if (name == "Receipt")
    {
        /* The Scheme receipt's own option page is the invoice selector, a
         * set of heading strings (which the native layout keeps fixed) and
         * the template/CSS/font/logo files.  It has no display boolean at
         * all: the layout prints the tax and total rows whenever the
         * document has them. */
        OptionDescriptor invoice;
        invoice.name = "invoice";
        invoice.type = OptionType::Invoice;
        invoice.label = "Invoice Number";
        invoice.description =
            "The customer invoice to print as a receipt, by its document id "
            "(for example \"INV-1001\") or by its GUID.  Without it the "
            "report renders the Scheme \"No invoice selected\" explanation.";
        options.push_back (invoice);
        return options;
    }

    if (name == "Tax Invoice" || name == "Australian Tax Invoice")
    {
        /* The Scheme taxinvoice.scm "Elements" page, plus the Report Title.
         * Both registered reports share this renderer; the Australian one
         * changes only the default title and the heading texts the renderer
         * derives from its locale flag. */
        const bool australian = name == "Australian Tax Invoice";
        OptionDescriptor invoice;
        invoice.name = "invoice";
        invoice.type = OptionType::Invoice;
        invoice.label = "Invoice Number";
        invoice.description =
            "The invoice or bill to print, by its document id (for example "
            "\"INV-1001\") or by its GUID.  Without it the report renders "
            "the Scheme \"No invoice has been selected\" explanation.";
        options.push_back (invoice);

        OptionDescriptor title;
        title.name = "title";
        title.type = OptionType::Text;
        title.label = "Report Title";
        title.description =
            "The heading printed above the document.  The Scheme default is "
            "\"Invoice\" (\"Tax Invoice\" for the Australian variant).";
        title.default_value = australian ? "Tax Invoice" : "Invoice";
        options.push_back (title);

        struct Switch
        {
            const char* name;
            const char* label;
            const char* description;
            bool default_value;
        };
        static const Switch switches[] = {
            {"show_date", "column: Date", "Display the date?", true},
            {"show_tax_rate", "column: Tax Rate",
             "Display the Tax Rate column?  A document with no tax table has "
             "no tax columns at all, so the option has no effect there.",
             true},
            {"show_units", "column: Units",
             "Display the Units column?  A document whose entries have no "
             "action has no Units column to show.", true},
            {"show_contact", "row: Contact",
             "Display the company contact row?  A book with no Company "
             "Contact Person has nothing to show, so the option has no "
             "effect there.", true},
            {"show_address", "row: Address",
             "Display the company address?  A book with no Company Address "
             "has nothing to show, so the option has no effect there.", true},
            {"show_invoice_number", "row: Invoice Number",
             "Display the invoice number row?", true},
            {"show_company_name", "row: Company Name",
             "Display the customer or vendor name above the address?", true},
            {"invoice_number_next_to_title", "Invoice Number next to title",
             "Print the invoice number next to the title as well as in the "
             "details table?", false},
            {"show_job_name", "Show Job name",
             "Display the job name?  An invoice whose owner is not a job has "
             "no job name to show, so the option has no effect there.", true},
            {"show_job_number", "Show Job number",
             "Display the job number?  An invoice whose owner is not a job "
             "has no job number to show, so the option has no effect there.",
             false},
            {"show_net_price", "Show net price",
             "Print the unit price net of included tax?  An untaxed document "
             "has the same net and gross price, so the option has no effect "
             "there.", false},
            {"border_collapse", "table-border-collapse",
             "Use border-collapse on the document's tables?", false},
        };
        for (const Switch& item : switches)
        {
            OptionDescriptor option;
            option.name = item.name;
            option.type = OptionType::Bool;
            option.label = item.label;
            option.description = item.description;
            option.default_value = item.default_value ? "true" : "false";
            options.push_back (option);
        }
        return options;
    }

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
    else if (name == "Investment Lots")
    {
        options.push_back (date_option (
            "start", "Start date",
            "The first day of the period the lots are reported for.  Purchases "
            "before it still count towards a lot's amount and basis, but are "
            "not shown in the bought columns; sales before it are not counted "
            "as realised.  It defaults to the start of the current calendar "
            "year, which is what the accounting-period preference the Scheme "
            "option resolves to defaults to.",
            default_calendar_year_start ()));
        options.push_back (date_option (
            "end", "End date",
            "The last day of the period the lots are reported for.  It is also "
            "the date the end price and the short/long-term test are taken at, "
            "and it defaults to today, as the Scheme report's does.",
            default_aging_date ()));
        options.push_back (currency_option ());
        /* No "period" preset: unlike the statements, this report's end date
         * is also the valuation date, so "all dates" cannot mean an unbounded
         * end.  A preset accepted and then ignored is worse than a 400, and
         * the route checks report_has_option() before resolving one. */
        return options;
    }
    else if (name == "Budget Report")
    {
        /* Only the account selection.  The Scheme report's dates are budget
         * period selectors ("Range start"/"Range end" over the budget's own
         * periods), not calendar dates, so the route's start/end options
         * cannot stand in for them; the budget itself defaults to the book's
         * default budget, and the option surface has no budget type to select
         * another one.  This is why the report is registered natively but not
         * exposed to the designer, whose specs are queries over splits rather
         * than over a budget object. */
        OptionDescriptor accounts = accounts_option ();
        accounts.description =
            "Accounts to report on, as a comma-separated list of account "
            "GUIDs; each GUID selects that account's subtree.  \"all\" selects "
            "every account in the book.  Without the option the report's own "
            "default selection applies: every ASSET, LIABILITY, INCOME and "
            "EXPENSE account, two display levels deep, which is the Scheme "
            "budget report's default.";
        options.push_back (accounts);
        return options;
    }
    else if (name == "Budget Income Statement" ||
             name == "Budget Profit & Loss")
    {
        /* The statement has no date option in the Scheme report: it is always
         * the whole budget, and its period range is a pair of period numbers
         * the route cannot express.  The account list and the report currency
         * are real options; the depth, zero-balance, parent-subtotal and
         * two-column switches stay at their Scheme defaults. */
        options.push_back (budget_accounts_option (
            "Accounts to report on, as a comma-separated list of account "
            "GUIDs; each GUID selects that account's subtree.  \"all\" selects "
            "every account in the book.  Without the option the Scheme "
            "report's default applies: every INCOME and EXPENSE account, three "
            "display levels deep.  Income amounts are sign-reversed, expenses "
            "are not."));
        options.push_back (currency_option ());
        return options;
    }
    else if (name == "Budget Balance Sheet")
    {
        /* Single-column form at the Scheme defaults.  The account list and
         * the report currency are honoured; the budget selection, display
         * switches and multi-column form are not expressible. */
        options.push_back (budget_accounts_option (
            "Accounts to report on, as a comma-separated list of account "
            "GUIDs; each GUID selects that account's subtree.  \"all\" selects "
            "every account in the book.  Without the option the Scheme "
            "report's default type filter applies, three display levels deep.  "
            "Asset rows are positive; liability and equity rows are "
            "credit-normal and sign-reversed."));
        options.push_back (currency_option ());
        return options;
    }
    else if (name == "Budget Flow")
    {
        /* The Scheme period option is a period *number*, so this report
         * renders period 1 (its default) and exposes no date option. */
        options.push_back (budget_accounts_option (
            "Accounts to report on, as a comma-separated list of account "
            "GUIDs; each GUID selects that account's subtree.  \"all\" selects "
            "every account in the book, which is the Scheme report's default.  "
            "Only leaf accounts are shown, in the Scheme's account-type "
            "sections."));
        options.push_back (currency_option ());
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

    if (name == "Budget Report")
    {
        /* The Scheme report's budget option defaults to the book's default
         * budget, so that is the one this report uses too.  With no budget in
         * the book the Scheme engine prints its "No budgets exist" warning;
         * with the default account selection empty it prints the "No accounts
         * selected" warning instead.  Both are the report's correct output,
         * not errors. */
        QofBook* book = gnc_account_get_book (root);
        GncBudget* budget =
            book != nullptr ? gnc_budget_get_default (book) : nullptr;
        if (budget == nullptr)
        {
            html = render_no_budget ("Budget Report");
            return true;
        }
        if (request.accounts.empty ())
        {
            bool any_account = false;
            std::vector<Account*> all;
            collect_accounts (root, all);
            for (Account* account : all)
            {
                if (budget_default_account_type (account))
                {
                    any_account = true;
                    break;
                }
            }
            if (!any_account)
            {
                html = render_no_accounts ("Budget Report", false);
                return true;
            }
        }

        const char* budget_name = gnc_budget_get_name (budget);
        std::string title = "Budget Report";
        if (budget_name != nullptr && budget_name[0] != '\0')
            title += std::string (": ") + budget_name;
        html = render_cell_table (
            budget_report (root, budget, request.accounts), title);
        return true;
    }

    if (name == "Budget Income Statement" || name == "Budget Profit & Loss")
    {
        QofBook* book = gnc_account_get_book (root);
        GncBudget* budget =
            book != nullptr ? gnc_budget_get_default (book) : nullptr;
        if (request.accounts.empty () &&
            budget_selection (root, request.accounts,
                              budget_income_statement_account_type).empty ())
        {
            html = render_no_accounts (name, false);
            return true;
        }
        if (budget == nullptr)
        {
            html = render_no_budget (name);
            return true;
        }
        const gchar* budget_name = gnc_budget_get_name (budget);
        std::string title = name + " for Budget ";
        title += budget_name != nullptr && budget_name[0] != '\0'
                     ? budget_name
                     : "Budget";
        html = render_cell_table (
            budget_income_statement (root, budget, request.accounts,
                                     request.currency),
            title);
        return true;
    }

    if (name == "Budget Balance Sheet")
    {
        QofBook* book = gnc_account_get_book (root);
        GncBudget* budget =
            book != nullptr ? gnc_budget_get_default (book) : nullptr;
        if (request.accounts.empty () &&
            budget_selection (root, request.accounts,
                              budget_balance_sheet_account_type).empty ())
        {
            html = render_no_accounts (name, false);
            return true;
        }
        if (budget == nullptr)
        {
            html = render_no_budget (name);
            return true;
        }
        const gchar* budget_name = gnc_budget_get_name (budget);
        std::string title = "Budget Balance Sheet ";
        title += budget_name != nullptr && budget_name[0] != '\0'
                     ? budget_name
                     : "Budget";
        html = render_cell_table (
            budget_balance_sheet (root, budget, request.accounts,
                                  request.currency),
            title);
        return true;
    }

    if (name == "Budget Flow")
    {
        QofBook* book = gnc_account_get_book (root);
        GncBudget* budget =
            book != nullptr ? gnc_budget_get_default (book) : nullptr;
        if (request.accounts.empty () &&
            budget_selection (root, request.accounts,
                              budget_any_account_type).empty ())
        {
            html = render_no_accounts (name, false);
            return true;
        }
        if (budget == nullptr)
        {
            html = render_no_budget (name);
            return true;
        }
        const gchar* budget_name = gnc_budget_get_name (budget);
        std::string title = "Budget Flow: ";
        title += budget_name != nullptr && budget_name[0] != '\0'
                     ? budget_name
                     : "Budget";
        title += " - ";
        title += printed_short_date (
            gnc_budget_get_period_start_date (budget, 0));
        html = render_cell_table (budget_flow (root, budget, request.accounts,
                                               request.currency),
                                  title);
        return true;
    }

    if (name == "Printable Invoice")
    {
        /* The Scheme report prints its warning when the "Invoice Number"
         * option is empty, and there is no invoice to print.  A selected
         * invoice that the book no longer has is reported as an error by the
         * route's option validator before this point; the warning here keeps
         * the native renderer total. */
        if (request.invoice.empty ())
        {
            html = render_invoice_not_selected ();
            return true;
        }
        InvoiceDocument document;
        std::string error;
        if (!find_invoice_document (root, request.invoice, document, &error))
        {
            html = render_invoice_not_selected ();
            return true;
        }
        html = render_printable_invoice (document, request.invoice_options);
        return true;
    }

    if (name == "Receipt")
    {
        if (request.invoice.empty ())
        {
            html = render_receipt_not_selected ();
            return true;
        }
        InvoiceDocument document;
        std::string error;
        if (!find_invoice_document (root, request.invoice, document, &error))
        {
            html = render_receipt_not_selected ();
            return true;
        }
        /* receipt.scm accepts a customer invoice only: it checks the end
         * owner's type, not the document type, so a customer credit note is
         * printable as a receipt as well. */
        html = document.owner.kind == OwnerKind::Customer
                   ? render_receipt (document)
                   : render_receipt_not_customer ();
        return true;
    }

    if (name == "Tax Invoice" || name == "Australian Tax Invoice")
    {
        if (request.invoice.empty ())
        {
            html = render_tax_invoice_not_selected ();
            return true;
        }
        InvoiceDocument document;
        std::string error;
        if (!find_invoice_document (root, request.invoice, document, &error))
        {
            html = render_tax_invoice_not_selected ();
            return true;
        }
        html = render_tax_invoice (document, request.tax_invoice_options,
                                   name == "Australian Tax Invoice");
        return true;
    }

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
    if (name == "Investment Lots")
    {
        /* The lots report has a real period, unlike the as-of reports.  Its
         * own defaults are the start of the current calendar year and today,
         * matching the Scheme report's options; a caller may pin either. */
        LotOptions options;
        options.start = request.start.value_or (default_calendar_year_start ());
        options.end = request.end.value_or (default_aging_date ());
        LotReport report = investment_lots (root, options, request.currency);
        html = render_cell_table (report.table, report.title);
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
