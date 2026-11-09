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
#include "gnc-owner.hpp"

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
    if (debit && credit && balance &&
        spec.grouping == GroupingMode::Period)
        return {Column::Debit, Column::Credit, Column::Balance};
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

/* -------------------------------------------------------- entry listings */

namespace
{

/* Every non-void transaction of the book, in the order the Scheme query
 * returns them.  The query's primary sort key is the transaction date and its
 * secondary key is the engine's default split order, which is what
 * xaccTransOrder() implements (date, closing flag, number, entered date,
 * description, GUID).  Duplicates are dropped because a transaction has
 * several splits. */
std::vector<Transaction*>
all_transactions (Account* root)
{
    std::vector<Account*> accounts;
    collect_accounts (root, accounts);

    std::vector<Transaction*> transactions;
    std::set<Transaction*> seen;
    for (Account* account : accounts)
    {
        for (GList* node = xaccAccountGetSplitList (account); node != nullptr;
             node = node->next)
        {
            auto* split = static_cast<Split*> (node->data);
            Transaction* trans = xaccSplitGetParent (split);
            if (trans == nullptr || xaccTransGetVoidStatus (trans))
                continue;
            if (seen.insert (trans).second)
                transactions.push_back (trans);
        }
    }

    std::stable_sort (transactions.begin (), transactions.end (),
                      [] (const Transaction* left, const Transaction* right)
                      {
                          return xaccTransOrder (left, right) < 0;
                      });
    return transactions;
}

/* The Scheme report's "Num" column for a transaction: the register's
 * gnc-get-num-action(trans, split), i.e. the transaction number when the book
 * does not use the split action for it.  The fixtures' books use the
 * transaction number. */
std::string
transaction_num (Transaction* trans)
{
    return text_or_empty (xaccTransGetNum (trans));
}

} // namespace

CellTable
general_journal (Account* root, const GeneralJournalOptions& options)
{
    gnc_commodity* currency = report_currency (root, nullptr);

    CellTable table;
    std::vector<std::string> headings;
    if (options.show_date)
        headings.push_back ("Date");
    if (options.show_num)
        headings.push_back ("Num");
    if (options.show_description || options.show_memo)
        headings.push_back ("Description");
    if (options.show_account)
        headings.push_back ("Account");
    headings.push_back ("Debit");
    headings.push_back ("Credit");
    if (options.show_running_balance)
        headings.push_back ("Balance");
    table.headings = std::move (headings);
    const size_t width = table.headings.size ();

    gnc_numeric grand_debit = gnc_numeric_zero ();
    gnc_numeric grand_credit = gnc_numeric_zero ();

    for (Transaction* trans : all_transactions (root))
    {
        const std::vector<Split*> splits = [trans]
        {
            std::vector<Split*> out;
            for (GList* node = xaccTransGetSplitList (trans); node != nullptr;
                 node = node->next)
                out.push_back (static_cast<Split*> (node->data));
            return out;
        } ();
        if (splits.empty ())
            continue;

        /* The register's Balance column on a transaction heading prints the
         * first split's running balance (not the transaction net, which is
         * zero for a balanced transaction).  The first split is the one the
         * query returns: the split order minimum, which is also what
         * xaccSplitOrder compares. */
        Split* leader = nullptr;
        for (Split* split : splits)
            if (leader == nullptr || xaccSplitOrder (split, leader) < 0)
                leader = split;
        gnc_numeric transaction_balance = gnc_numeric_zero ();
        if (leader != nullptr)
        {
            transaction_balance = xaccSplitGetBalance (leader);
            if (shows_reversed (xaccSplitGetAccount (leader)))
                transaction_balance = gnc_numeric_neg (transaction_balance);
        }

        /* The transaction heading line: date, number, description and empty
         * amount cells, then (with a running balance) the first split's
         * running balance.  The register prints the heading as an ordinary
         * row followed by a spacer row. */
        std::vector<Cell> heading;
        if (options.show_date)
            heading.push_back ({split_date (splits.front ()), "date-cell"});
        if (options.show_num)
            heading.push_back ({transaction_num (trans), "text-cell"});
        if (options.show_description || options.show_memo)
            heading.push_back (
                {options.show_description
                     ? text_or_empty (xaccTransGetDescription (trans))
                     : std::string (),
                 "text-cell"});
        if (options.show_account)
            heading.push_back ({{}, "text-cell"});
        heading.push_back ({});
        heading.push_back ({});
        if (options.show_running_balance)
            heading.push_back ({format_amount (transaction_balance, currency),
                                "number-cell"});
        table.rows.push_back (std::move (heading));

        std::vector<Cell> spacer;
        spacer.push_back ({});
        spacer.push_back ({{}, {}, width > 0 ? static_cast<int> (width) - 1 : 1});
        table.rows.push_back (std::move (spacer));

        for (Split* split : splits)
        {
            Account* account = xaccSplitGetAccount (split);
            const gnc_numeric amount = xaccSplitGetAmount (split);
            if (gnc_numeric_positive_p (amount))
                grand_debit = numeric_add (grand_debit, amount);
            else if (gnc_numeric_negative_p (amount))
                grand_credit = numeric_add (grand_credit,
                                            gnc_numeric_neg (amount));

            std::vector<Cell> row;
            if (options.show_date)
                row.push_back ({});
            if (options.show_num)
                row.push_back ({text_or_empty (xaccSplitGetAction (split)),
                                "text-cell"});
            if (options.show_description || options.show_memo)
                row.push_back ({options.show_memo
                                    ? text_or_empty (xaccSplitGetMemo (split))
                                    : std::string (),
                                "text-cell"});
            if (options.show_account)
                row.push_back ({account_full_name (account), "text-cell"});
            if (gnc_numeric_negative_p (amount))
            {
                row.push_back ({});
                row.push_back ({format_amount (gnc_numeric_neg (amount),
                                               currency),
                                "number-cell"});
            }
            else
            {
                row.push_back ({format_amount (amount, currency),
                                "number-cell"});
                row.push_back ({});
            }
            if (options.show_running_balance)
                row.push_back ({});
            table.rows.push_back (std::move (row));
        }
    }

    if (!table.rows.empty ())
    {
        std::vector<Cell> debits;
        debits.push_back ({});
        debits.push_back ({"Total Debits", "total-label-cell"});
        debits.push_back ({format_amount (grand_debit, currency),
                           "total-number-cell",
                           width > 1 ? static_cast<int> (width) - 1 : 1});
        table.rows.push_back (std::move (debits));

        std::vector<Cell> credits;
        credits.push_back ({});
        credits.push_back ({"Total Credits", "total-label-cell"});
        credits.push_back ({format_amount (grand_credit, currency),
                            "total-number-cell",
                            width > 1 ? static_cast<int> (width) - 1 : 1});
        table.rows.push_back (std::move (credits));
    }

    return table;
}

/* --------------------------------------------------- transaction breakdown */

namespace
{

/* The Scheme report's account<? : the engine's own account order. */
int
account_order (const Account* left, const Account* right)
{
    return xaccAccountOrder (const_cast<Account*> (left),
                             const_cast<Account*> (right));
}

/* The Scheme report's account-type<? : assets, liabilities, equity, expenses,
 * income, with an account type outside that list sorting last. */
int
breakdown_type_index (Account* account)
{
    const GNCAccountType fundamental =
        xaccAccountTypeGetFundamental (xaccAccountGetType (account));
    switch (fundamental)
    {
    case ACCT_TYPE_ASSET:     return 0;
    case ACCT_TYPE_LIABILITY: return 1;
    case ACCT_TYPE_EQUITY:    return 2;
    case ACCT_TYPE_EXPENSE:   return 3;
    case ACCT_TYPE_INCOME:    return 4;
    default:                  return 5;
    }
}

/* The account's full name as a list of names below the book root, i.e.
 * ("Assets" "Current Assets" "Checking Account"). */
std::vector<std::string>
account_name_parts (Account* account)
{
    QofBook* book = gnc_account_get_book (account);
    Account* root = book != nullptr ? gnc_book_get_root_account (book)
                                    : nullptr;
    std::vector<std::string> parts;
    for (Account* current = account; current != nullptr && current != root;
         current = gnc_account_get_parent (current))
        parts.insert (parts.begin (), account_name (current));
    return parts;
}

/* A cell's text for an amount in `currency`. */
std::string
breakdown_amount (gnc_numeric value, gnc_numeric amount,
                  gnc_commodity* transaction_currency,
                  gnc_commodity* account_currency)
{
    if (gnc_commodity_equal (account_currency, transaction_currency))
        return format_amount (value, transaction_currency);
    return "[" + format_amount (value, transaction_currency) + "] " +
           format_amount (amount, account_currency);
}

} // namespace

BreakdownReport
transaction_breakdown (Account* root, const BreakdownOptions& options)
{
    /* The query: the selected account's splits, non-void, in the period, and
     * then the transactions whose description contains the filter. */
    std::vector<Transaction*> transactions;
    std::set<Transaction*> seen;
    if (options.account != nullptr)
    {
        for (GList* node = xaccAccountGetSplitList (options.account);
             node != nullptr; node = node->next)
        {
            auto* split = static_cast<Split*> (node->data);
            Transaction* trans = xaccSplitGetParent (split);
            if (trans == nullptr)
                continue;
            if (xaccTransGetVoidStatus (trans))
                continue;
            const time64 date = xaccTransGetDate (trans);
            if (options.start.has_value () && date < *options.start)
                continue;
            if (options.end.has_value () && date > *options.end)
                continue;
            const std::string description =
                text_or_empty (xaccTransGetDescription (trans));
            if (!options.description_filter.empty () &&
                description.find (options.description_filter) ==
                    std::string::npos)
                continue;
            if (seen.insert (trans).second)
                transactions.push_back (trans);
        }
    }

    BreakdownReport report;
    if (transactions.empty ())
    {
        report.no_data = true;
        return report;
    }

    CellTable& table = report.table;

    /* Every account any of the transactions touches, ordered by account type
     * then account order, exactly as the Scheme report collects and sorts
     * them. */
    std::vector<Account*> accounts;
    for (Transaction* trans : transactions)
    {
        for (GList* node = xaccTransGetSplitList (trans); node != nullptr;
             node = node->next)
        {
            auto* split = static_cast<Split*> (node->data);
            Account* account = xaccSplitGetAccount (split);
            if (account == nullptr)
                continue;
            if (xaccAccountGetType (account) == ACCT_TYPE_TRADING)
                continue;
            if (std::find (accounts.begin (), accounts.end (), account) !=
                accounts.end ())
                continue;
            accounts.push_back (account);
        }
    }
    std::stable_sort (accounts.begin (), accounts.end (),
                      [] (Account* left, Account* right)
                      {
                          return account_order (left, right) < 0;
                      });
    std::stable_sort (accounts.begin (), accounts.end (),
                      [] (Account* left, Account* right)
                      {
                          return breakdown_type_index (left) <
                                 breakdown_type_index (right);
                      });

    if (static_cast<int> (accounts.size ()) > options.max_columns)
    {
        report.too_many_accounts = true;
        return report;
    }

    const size_t max_depth = [accounts]
    {
        size_t depth = 0;
        for (Account* account : accounts)
            depth = std::max<size_t> (
                depth, gnc_account_get_current_depth (account));
        return depth;
    } ();

    /* The header: Date, Num and Desc padded to the depth of the account
     * column tree, then each account's name parts padded the same way. */
    std::vector<std::vector<std::string>> header_text (
        max_depth, std::vector<std::string> (3 + accounts.size ()));
    const char* fixed[] = {"Date", "Num", "Desc"};
    for (size_t row = 0; row < max_depth; ++row)
        for (size_t column = 0; column < 3; ++column)
            if (row == 0)
                header_text[row][column] = fixed[column];
    for (size_t index = 0; index < accounts.size (); ++index)
    {
        const std::vector<std::string> parts =
            account_name_parts (accounts[index]);
        for (size_t row = 0; row < max_depth; ++row)
            if (row < parts.size ())
                header_text[row][3 + index] = parts[row];
    }
    for (const std::vector<std::string>& text_row : header_text)
    {
        std::vector<Cell> cells;
        for (const std::string& text : text_row)
            cells.push_back ({text, {}});
        table.header_rows.push_back (std::move (cells));
    }

    std::vector<gnc_numeric> totals (accounts.size (), gnc_numeric_zero ());

    for (Transaction* trans : transactions)
    {
        gnc_commodity* transaction_currency = xaccTransGetCurrency (trans);
        std::vector<Cell> row;
        char date[32] = {0};
        qof_print_date_buff (date, sizeof date, xaccTransGetDate (trans));
        row.push_back ({date, {}});
        row.push_back ({text_or_empty (xaccTransGetNum (trans)), {}});
        row.push_back ({text_or_empty (xaccTransGetDescription (trans)), {}});

        for (size_t index = 0; index < accounts.size (); ++index)
        {
            Account* account = accounts[index];
            gnc_numeric value = gnc_numeric_zero ();
            gnc_numeric amount = gnc_numeric_zero ();
            bool found = false;
            for (GList* node = xaccTransGetSplitList (trans); node != nullptr;
                 node = node->next)
            {
                auto* split = static_cast<Split*> (node->data);
                if (xaccSplitGetAccount (split) != account)
                    continue;
                found = true;
                value = numeric_add (value, xaccSplitGetValue (split));
                amount = numeric_add (amount, xaccSplitGetAmount (split));
            }
            if (!found)
            {
                row.push_back ({});
                continue;
            }
            if (shows_reversed (account))
            {
                value = gnc_numeric_neg (value);
                amount = gnc_numeric_neg (amount);
            }
            row.push_back ({breakdown_amount (
                                value, amount, transaction_currency,
                                xaccAccountGetCommodity (account)),
                            "number-cell"});
            totals[index] = numeric_add (totals[index], amount);
        }
        table.rows.push_back (std::move (row));
    }

    gchar* interval = gnc_date_interval_format (
        options.start.value_or (0), options.end.value_or (
            options.start.has_value () ? *options.start
                                       : gnc_time64_get_today_end ()));
    std::vector<Cell> total_row;
    total_row.push_back ({"Total For " + std::string (interval != nullptr
                                                          ? interval
                                                          : ""),
                          "total-label-cell", 3});
    if (interval != nullptr)
        g_free (interval);
    for (size_t index = 0; index < accounts.size (); ++index)
        total_row.push_back (
            {format_amount (totals[index],
                            xaccAccountGetCommodity (accounts[index])),
             "total-number-cell"});
    table.rows.push_back (std::move (total_row));

    return report;
}

/* ------------------------------------------------------- reconciliation */

namespace
{

int
reconcile_index (char status)
{
    switch (status)
    {
    case 'n': return 0;
    case 'c': return 1;
    case 'y': return 2;
    case 'f': return 3;
    default:  return 4;
    }
}

std::string
reconcile_label (char status)
{
    switch (status)
    {
    case 'n': return "Unreconciled";
    case 'c': return "Cleared";
    case 'y': return "Reconciled";
    case 'f': return "Frozen";
    default:  return "Voided";
    }
}

} // namespace

CellTable
reconciliation_report (Account* root, const ReconciliationOptions& options)
{
    gnc_commodity* currency = report_currency (root, nullptr);

    /* The trep renderer's date rule for this report: a reconciled split is
     * in the period when its reconciled date is, and every other split is
     * always included. */
    Query query;
    query.scope = root;
    query.currency = currency;
    query.skip_voided = true;
    query.skip_closing = true;

    std::vector<Split*> splits;
    for (Account* account : options.accounts)
    {
        for (GList* node = xaccAccountGetSplitList (account); node != nullptr;
             node = node->next)
        {
            auto* split = static_cast<Split*> (node->data);
            if (query.skip_voided &&
                xaccTransGetVoidStatus (xaccSplitGetParent (split)))
                continue;
            if (closing_entry_p (query, split))
                continue;
            if (xaccSplitGetReconcile (split) == YREC)
            {
                const time64 reconciled = xaccSplitGetDateReconciled (split);
                if (options.start.has_value () && reconciled < *options.start)
                    continue;
                if (options.end.has_value () && reconciled > *options.end)
                    continue;
            }
            splits.push_back (split);
        }
    }

    /* The Scheme query's default order first, then the report's own sort:
     * reconciled status (its primary key) and date (its secondary key). */
    std::stable_sort (splits.begin (), splits.end (),
                      [] (const Split* left, const Split* right)
                      {
                          return xaccSplitOrder (left, right) < 0;
                      });
    std::stable_sort (splits.begin (), splits.end (),
                      [] (const Split* left, const Split* right)
                      {
                          const int by_status =
                              reconcile_index (xaccSplitGetReconcile (left)) -
                              reconcile_index (xaccSplitGetReconcile (right));
                          if (by_status != 0)
                              return by_status < 0;
                          const time64 left_date = xaccTransGetDate (
                              xaccSplitGetParent (left));
                          const time64 right_date = xaccTransGetDate (
                              xaccSplitGetParent (right));
                          return left_date < right_date;
                      });

    CellTable table;
    table.headings = {"", "Date", "Reconciled Date", "Num", "Description",
                      "Transfer from/to", "Funds In", "Funds Out"};
    gchar* interval = gnc_date_interval_format (
        options.start.value_or (0), options.end.value_or (
            options.start.has_value () ? *options.start
                                       : gnc_time64_get_today_end ()));
    if (interval != nullptr)
    {
        table.subtitle = interval;
        g_free (interval);
    }

    auto add_total = [&table, currency] (const std::string& label,
                                         gnc_numeric funds_in,
                                         gnc_numeric funds_out)
    {
        std::vector<Cell> row;
        row.push_back ({});
        row.push_back ({label, "total-label-cell", 5});
        row.push_back ({format_amount (funds_in, currency),
                        "total-number-cell"});
        row.push_back ({format_amount (funds_out, currency),
                        "total-number-cell"});
        table.rows.push_back (std::move (row));
    };

    char group_status = '\0';
    bool in_group = false;
    gnc_numeric group_in = gnc_numeric_zero ();
    gnc_numeric group_out = gnc_numeric_zero ();
    gnc_numeric grand_in = gnc_numeric_zero ();
    gnc_numeric grand_out = gnc_numeric_zero ();

    for (Split* split : splits)
    {
        const char status = xaccSplitGetReconcile (split);
        if (!in_group || status != group_status)
        {
            if (in_group)
                add_total ("Total For " + reconcile_label (group_status),
                           group_in, group_out);
            group_status = status;
            in_group = true;
            group_in = gnc_numeric_zero ();
            group_out = gnc_numeric_zero ();
            table.rows.push_back (
                {{reconcile_label (status), "total-label-cell", 6}, {}, {}});
        }

        Transaction* trans = xaccSplitGetParent (split);
        const gnc_numeric amount = xaccSplitGetAmount (split);
        const bool funds_in = gnc_numeric_positive_p (amount);
        const gnc_numeric shown = funds_in ? amount : gnc_numeric_neg (amount);
        if (funds_in)
        {
            group_in = numeric_add (group_in, amount);
            grand_in = numeric_add (grand_in, amount);
        }
        else
        {
            group_out = numeric_add (group_out, shown);
            grand_out = numeric_add (grand_out, shown);
        }

        std::vector<Cell> row;
        row.push_back ({});
        row.push_back ({split_date (split), "date-cell"});
        if (options.show_reconciled_date)
        {
            if (status == YREC && xaccSplitGetDateReconciled (split) != 0)
            {
                char date[32] = {0};
                qof_print_date_buff (date, sizeof date,
                                     xaccSplitGetDateReconciled (split));
                row.push_back ({date, "date-cell"});
            }
            else
                row.push_back ({});
        }
        row.push_back ({transaction_num (trans), "text-cell"});
        row.push_back ({text_or_empty (xaccTransGetDescription (trans)),
                        "text-cell"});
        row.push_back ({transfer_name (split), {}});
        row.push_back (funds_in
                           ? Cell{format_amount (shown, currency), "number-cell"}
                           : Cell{});
        row.push_back (!funds_in
                           ? Cell{format_amount (shown, currency), "number-cell"}
                           : Cell{});
        table.rows.push_back (std::move (row));
    }

    if (in_group)
        add_total ("Total For " + reconcile_label (group_status), group_in,
                   group_out);

    if (!splits.empty ())
    {
        /* The Scheme grand-total row starts with the label (it has no
         * indenting cell), unlike the per-group total rows. */
        std::vector<Cell> row;
        row.push_back ({"Grand Total", "total-label-cell", 6});
        row.push_back ({format_amount (grand_in, currency),
                        "total-number-cell"});
        row.push_back ({format_amount (grand_out, currency),
                        "total-number-cell"});
        table.rows.push_back (std::move (row));
    }

    return table;
}

std::string
render_reconciliation_no_accounts ()
{
    std::ostringstream out;
    out << "<!DOCTYPE html>\n<html><head><meta charset='utf-8'><title>"
        << "Reconciliation Report</title><style>" << report_stylesheet ()
        << "</style></head><body><h3>Reconciliation Report:</h3>\n"
        << "<h3>No accounts selected</h3>\n"
        << "<p>This report requires accounts to be selected in the report "
           "options.</p>\n"
        << "<p>The reconcile report is designed to be similar to the formal "
           "reconciliation tool.  Please select the account from Report "
           "Options.  Please note the dates specified in the options will "
           "apply to the Reconciliation Date.</p>\n"
        << "</body></html>\n";
    return out.str ();
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

/* ------------------------------------------------ multicolumn statements */

const char*
column_period_name (ColumnPeriod period)
{
    switch (period)
    {
    case ColumnPeriod::Disabled:  return "disabled";
    case ColumnPeriod::Year:      return "year";
    case ColumnPeriod::HalfYear:  return "half-year";
    case ColumnPeriod::Quarter:   return "quarter";
    case ColumnPeriod::Month:     return "month";
    case ColumnPeriod::TwoWeeks:  return "two-weeks";
    case ColumnPeriod::Week:      return "week";
    }
    return "";
}

bool
column_period_from_name (const std::string& name, ColumnPeriod& period)
{
    for (const ColumnPeriod candidate :
         {ColumnPeriod::Disabled, ColumnPeriod::Year, ColumnPeriod::HalfYear,
          ColumnPeriod::Quarter, ColumnPeriod::Month, ColumnPeriod::TwoWeeks,
          ColumnPeriod::Week})
    {
        if (name == column_period_name (candidate))
        {
            period = candidate;
            return true;
        }
    }
    return false;
}

namespace
{

/* ---------------------------------------------- date axis of the columns */

/* gnc:end-month?(): adding one day changes the month. */
bool
date_is_month_end (const GDate& date)
{
    GDate next = date;
    g_date_add_days (&next, 1);
    return g_date_get_month (&next) != g_date_get_month (&date);
}

/* incdate-months from the report engine's date utilities: add whole months,
 * clamp the day to the target month, and keep a month-end date at the month
 * end (31 January + one month is 28 or 29 February, never 3 March). */
GDate
add_months (const GDate& date, int months)
{
    const bool month_end = date_is_month_end (date);
    GDate result = date;
    g_date_add_months (&result, months);
    if (month_end)
    {
        while (!date_is_month_end (result))
            g_date_add_days (&result, 1);
    }
    return result;
}

/* gnc:make-date-list(): `start`, `start + n*step`, ... while the result is
 * before `end`, then `end` itself.  The step keeps the start date's time of
 * day, which is what makes a balance-sheet column the end of its day and a
 * profit & loss column the start of its day. */
std::vector<time64>
make_date_list (time64 start, time64 end, ColumnPeriod period)
{
    std::vector<time64> dates;
    if (end < start)
        std::swap (start, end);
    if (start >= end || period == ColumnPeriod::Disabled)
    {
        dates.push_back (end);
        return dates;
    }

    GDate start_date = time64_to_gdate (start);
    /* The start date's time of day: the Scheme report steps with incdate(),
     * which keeps the wall-clock time, so a balance-sheet column stays the
     * end of its day and a profit & loss column the start of its day. */
    struct tm clock;
    gnc_localtime_r (&start, &clock);

    auto at = [&clock] (GDate date)
    {
        struct tm target = clock;
        target.tm_year = g_date_get_year (&date) - 1900;
        target.tm_mon = g_date_get_month (&date) - 1;
        target.tm_mday = g_date_get_day (&date);
        target.tm_isdst = -1;
        return gnc_mktime (&target);
    };

    int months = 0;
    int days = 0;
    switch (period)
    {
    case ColumnPeriod::Year:     months = 12; break;
    case ColumnPeriod::HalfYear: months = 6;  break;
    case ColumnPeriod::Quarter:  months = 3;  break;
    case ColumnPeriod::Month:    months = 1;  break;
    case ColumnPeriod::TwoWeeks: days = 14;   break;
    case ColumnPeriod::Week:     days = 7;    break;
    case ColumnPeriod::Disabled: break;
    }

    for (int index = 0; ; ++index)
    {
        GDate date = months > 0 ? add_months (start_date, months * index)
                                : start_date;
        if (days > 0)
            g_date_add_days (&date, days * index);
        const time64 when = at (date);
        if (when >= end)
            break;
        dates.push_back (when);
    }
    dates.push_back (end);
    return dates;
}

/* qof-print-date(): the heading the Scheme report's column headers print. */
std::string
print_date (time64 when)
{
    char date[32] = {0};
    qof_print_date_buff (date, sizeof date, when);
    return std::string (date);
}

/* "12/01/18 to 12/31/18": the Scheme profit & loss column heading. */
std::string
print_interval (time64 from, time64 to)
{
    return print_date (from) + " to " + print_date (to);
}

/* ------------------------------------------------------- amount series */

/* One amount per column, in display order, for one account. */
using Series = std::vector<gnc_numeric>;

std::vector<gnc_numeric>
zero_series (size_t columns)
{
    return std::vector<gnc_numeric> (columns, gnc_numeric_zero ());
}

bool
series_all_zero (const Series& series)
{
    for (const gnc_numeric value : series)
    {
        if (!gnc_numeric_zero_p (value))
            return false;
    }
    return true;
}

/* The Scheme report's gnc:account-accumulate-at-dates() reads the running
 * balance of the last split posted on or before the report date, which is
 * the sum of every split up to and including that date.  Closing entries are
 * part of a balance-sheet balance (xaccSplitGetBalance) and are left out of
 * a profit & loss balance (xaccSplitGetNoclosingBalance). */
gnc_numeric
raw_balance_at (Account* account, time64 when, bool with_closing)
{
    gnc_numeric total = gnc_numeric_zero ();
    gnc_account_foreach_split_between_dates (
        account, std::nullopt, when, false,
        [&] (Split* split)
        {
            if (!with_closing &&
                xaccTransGetIsClosingTxn (xaccSplitGetParent (split)))
                return;
            total = numeric_add (total, xaccSplitGetAmount (split));
        });
    return total;
}

/* Convert an amount into the report currency at the price date the Scheme
 * report uses for the column.  A null currency, or an account already in it,
 * leaves the amount alone (the report's "Common Currency" default). */
gnc_numeric
converted_at (Account* account, gnc_numeric amount,
              gnc_commodity* currency, time64 when)
{
    gnc_commodity* account_currency = xaccAccountGetCommodity (account);
    if (currency == nullptr || account_currency == nullptr ||
        account_currency == currency)
        return amount;
    return xaccAccountConvertBalanceToCurrencyAsOfDate (
        account, amount, account_currency, currency, when);
}

/* gnc:decompose-accountlist(): the report's account list split into the
 * buckets the statements print, in the list's own order. */
struct BucketAccounts
{
    std::vector<Account*> assets;
    std::vector<Account*> liabilities;
    std::vector<Account*> equity;
    std::vector<Account*> income;
    std::vector<Account*> expenses;
};

BucketAccounts
split_accounts (const std::vector<Account*>& accounts)
{
    BucketAccounts split;
    for (Account* account : accounts)
    {
        switch (bucket_of (account))
        {
        case Bucket::Asset:     split.assets.push_back (account); break;
        case Bucket::Liability: split.liabilities.push_back (account); break;
        case Bucket::Equity:    split.equity.push_back (account); break;
        case Bucket::Income:    split.income.push_back (account); break;
        case Bucket::Expense:   split.expenses.push_back (account); break;
        default: break;
        }
    }
    return split;
}

std::vector<Account*>
income_expense_accounts (const BucketAccounts& split)
{
    std::vector<Account*> accounts;
    accounts.insert (accounts.end (), split.income.begin (), split.income.end ());
    accounts.insert (accounts.end (), split.expenses.begin (),
                     split.expenses.end ());
    return accounts;
}

/* The account's descendants that are themselves in the report's account list,
 * which is what the Scheme report's (account-and-descendants) is. */
void
collect_descendants_in (Account* account, const std::vector<Account*>& list,
                        std::vector<Account*>& out)
{
    GList* children = gnc_account_get_children_sorted (account);
    for (GList* node = children; node != nullptr; node = node->next)
    {
        auto* child = static_cast<Account*> (node->data);
        if (std::find (list.begin (), list.end (), child) != list.end ())
            out.push_back (child);
        collect_descendants_in (child, list, out);
    }
    g_list_free (children);
}

std::vector<Account*>
descendants_in (Account* account, const std::vector<Account*>& list)
{
    std::vector<Account*> out;
    collect_descendants_in (account, list, out);
    return out;
}

/* Row label: the account's own name, or its full name when the report's
 * "Account full name instead of indenting" option asks for it. */
std::string
statement_account_name (const Account* account, bool full_name)
{
    return full_name ? account_full_name (account) : account_name (account);
}

} // namespace

Table
balance_sheet_multicolumn (Account* root, const MulticolumnOptions& options)
{
    gnc_commodity* currency = report_currency (root, options.currency);
    const time64 start = options.start.value_or (default_start_date ());
    const time64 end = options.end.value_or (default_end_date ());

    /* The balance sheet's columns are dates at the end of their day, and the
     * single-column form has one: the report date. */
    std::vector<time64> dates;
    if (options.period == ColumnPeriod::Disabled)
        dates.push_back (gnc_time64_get_day_end (end));
    else
        dates = make_date_list (gnc_time64_get_day_end (start),
                                gnc_time64_get_day_end (end), options.period);
    if (options.reverse_chrono)
        std::reverse (dates.begin (), dates.end ());

    Table table;
    table.columns = {Column::AccountName};
    table.currency = currency;
    const bool grouped = options.period != ColumnPeriod::Disabled;
    if (grouped)
    {
        table.row_heading = "Date";
        for (const time64 date : dates)
            table.column_headings.push_back (print_date (date));
    }
    else
        table.columns.push_back (Column::Balance);

    std::vector<Account*> accounts;
    if (root != nullptr)
        collect_accounts (root, accounts);
    const BucketAccounts split = split_accounts (accounts);

    /* Each account's own balance at every column date. */
    std::map<Account*, Series> balances;
    for (Account* account : accounts)
    {
        Series series;
        for (const time64 date : dates)
            series.push_back (converted_at (
                account, raw_balance_at (account, date, true), currency, date));
        balances[account] = std::move (series);
    }

    const std::vector<Account*> income_expense = income_expense_accounts (split);
    /* The income and expense balances print as one "Retained Earnings" row
     * inside equity, which is why the row is a virtual account. */
    Series retained = zero_series (dates.size ());
    for (size_t column = 0; column < dates.size (); ++column)
    {
        for (Account* account : income_expense)
            retained[column] = numeric_add (retained[column],
                                            balances[account][column]);
    }

    const size_t columns = dates.size ();
    auto series_sum = [&balances, columns] (const std::vector<Account*>& list)
    {
        Series total = zero_series (columns);
        for (Account* account : list)
        {
            const auto found = balances.find (account);
            if (found == balances.end ())
                continue;
            for (size_t column = 0; column < columns; ++column)
                total[column] = numeric_add (total[column],
                                             found->second[column]);
        }
        return total;
    };

    auto add_row = [&table, grouped] (const std::string& label, RowType type,
                                      const Series& values, bool negate)
    {
        Row row;
        row.type = type;
        row.label = label;
        if (type == RowType::Section)
            row.blank = true;
        for (const gnc_numeric value : values)
            row.values.push_back (negate ? gnc_numeric_neg (value) : value);
        if (!grouped && !row.values.empty ())
            row.amount = row.values.front ();
        table.rows.push_back (std::move (row));
    };

    /* One section: its title, one row per account (a parent's row is its
     * subtree, and its own balance follows on a second row when it has one),
     * and its total. */
    auto add_section = [&] (const std::string& title,
                            const std::vector<Account*>& list, bool negate)
    {
        if (list.empty ())
            return;
        if (options.label_sections)
            add_row (title, RowType::Section, zero_series (columns), false);
        for (Account* account : list)
        {
            const std::vector<Account*> subtree = descendants_in (account, list);
            if (!subtree.empty ())
            {
                const Series total = series_sum (subtree);
                if (options.show_zero_accounts || !series_all_zero (total))
                    add_row (statement_account_name (account,
                                                     options.account_full_name),
                             RowType::Account, total, negate);
                if (!series_all_zero (balances[account]))
                    add_row (statement_account_name (account,
                                                     options.account_full_name),
                             RowType::Account, balances[account], negate);
            }
            else if (options.show_zero_accounts ||
                     !series_all_zero (balances[account]))
                add_row (statement_account_name (account,
                                                 options.account_full_name),
                         RowType::Account, balances[account], negate);
        }
        if (options.total_sections)
            add_row ("Total For " + title, RowType::Total, series_sum (list),
                     negate);
    };

    add_section ("Asset", split.assets, false);
    add_section ("Liability", split.liabilities, true);

    /* Equity, with the retained-earnings row appended. */
    if (!split.equity.empty () || !income_expense.empty ())
    {
        if (options.label_sections)
            add_row ("Equity", RowType::Section, zero_series (columns), false);
        for (Account* account : split.equity)
        {
            const std::vector<Account*> subtree =
                descendants_in (account, split.equity);
            if (!subtree.empty ())
            {
                const Series total = series_sum (subtree);
                if (options.show_zero_accounts || !series_all_zero (total))
                    add_row (statement_account_name (account,
                                                     options.account_full_name),
                             RowType::Account, total, true);
                if (!series_all_zero (balances[account]))
                    add_row (statement_account_name (account,
                                                     options.account_full_name),
                             RowType::Account, balances[account], true);
            }
            else if (options.show_zero_accounts ||
                     !series_all_zero (balances[account]))
                add_row (statement_account_name (account,
                                                 options.account_full_name),
                         RowType::Account, balances[account], true);
        }
        if (!income_expense.empty ())
            add_row ("Retained Earnings", RowType::Account, retained, true);
        if (options.total_sections)
        {
            Series total = series_sum (split.equity);
            for (size_t column = 0; column < columns; ++column)
                total[column] = numeric_add (total[column], retained[column]);
            add_row ("Total For Equity", RowType::Total, total, true);
        }
    }

    if (options.total_sections)
    {
        Series total = series_sum (split.liabilities);
        for (Account* account : split.equity)
        {
            for (size_t column = 0; column < columns; ++column)
                total[column] = numeric_add (total[column],
                                             balances[account][column]);
        }
        for (size_t column = 0; column < columns; ++column)
            total[column] = numeric_add (total[column], retained[column]);
        add_row ("Total For Liability and Equity", RowType::Total, total, true);
    }

    return table;
}

Table
income_statement_multicolumn (Account* root, const MulticolumnOptions& options)
{
    gnc_commodity* currency = report_currency (root, options.currency);
    const time64 start =
        gnc_time64_get_day_start (options.start.value_or (default_start_date ()));
    const time64 end =
        gnc_time64_get_day_end (options.end.value_or (default_end_date ()));

    /* The profit & loss columns are periods: the report's dates are the
     * period boundaries, and a column is the activity between two of them. */
    std::vector<time64> dates;
    if (options.period == ColumnPeriod::Disabled)
    {
        dates.push_back (start);
        dates.push_back (end);
    }
    else
        dates = make_date_list (start, end, options.period);

    struct Interval
    {
        time64 from;
        time64 to;
        /* The price date the Scheme report converts this column at. */
        time64 price_date;
        std::string heading;
    };
    std::vector<Interval> intervals;
    for (size_t index = 0; index + 1 < dates.size (); ++index)
    {
        /* The Scheme report's col-idx->datepair(): the last interval ends on
         * the report date, every earlier one on the day before the next
         * period starts. */
        const bool last = index + 2 >= dates.size ();
        const time64 printed_end =
            last ? dates.back () : dates[index + 1] - 86400;
        intervals.push_back ({dates[index], dates[index + 1], printed_end,
                              print_interval (dates[index], printed_end)});
    }
    if (options.include_overall_period && dates.size () > 2)
    {
        /* The overall column's heading reads to the report date itself, not
         * to the day before the next period, which is the Scheme report's
         * special case for this column. */
        intervals.push_back ({dates.front (), dates.back (), dates.back (),
                              print_interval (dates.front (), dates.back ())});
    }
    if (options.reverse_chrono)
        std::reverse (intervals.begin (), intervals.end ());

    Table table;
    table.columns = {Column::AccountName};
    table.currency = currency;
    const bool grouped = options.period != ColumnPeriod::Disabled;
    if (grouped)
    {
        table.row_heading = "Period";
        for (const Interval& interval : intervals)
            table.column_headings.push_back (interval.heading);
    }
    else
        table.columns.push_back (Column::Balance);

    std::vector<Account*> accounts;
    if (root != nullptr)
        collect_accounts (root, accounts);
    const BucketAccounts split = split_accounts (accounts);

    /* Each account's own activity per column: the balance at the end of the
     * period minus the balance at its start, closing entries left out, which
     * is the Scheme report's no-closing balance difference. */
    std::map<Account*, Series> activity;
    for (Account* account : accounts)
    {
        Series series;
        for (const Interval& interval : intervals)
        {
            const gnc_numeric opening =
                raw_balance_at (account, interval.from, false);
            const gnc_numeric closing =
                raw_balance_at (account, interval.to, false);
            series.push_back (converted_at (
                account,
                gnc_numeric_sub (closing, opening, GNC_DENOM_AUTO,
                                 GNC_HOW_DENOM_LCD),
                currency, interval.price_date));
        }
        activity[account] = std::move (series);
    }

    const size_t columns = intervals.size ();
    auto series_sum = [&activity, columns] (const std::vector<Account*>& list)
    {
        Series total = zero_series (columns);
        for (Account* account : list)
        {
            const auto found = activity.find (account);
            if (found == activity.end ())
                continue;
            for (size_t column = 0; column < columns; ++column)
                total[column] = numeric_add (total[column],
                                             found->second[column]);
        }
        return total;
    };

    auto add_row = [&table, grouped] (const std::string& label, RowType type,
                                      const Series& values, bool negate)
    {
        Row row;
        row.type = type;
        row.label = label;
        if (type == RowType::Section)
            row.blank = true;
        for (const gnc_numeric value : values)
            row.values.push_back (negate ? gnc_numeric_neg (value) : value);
        if (!grouped && !row.values.empty ())
            row.amount = row.values.front ();
        table.rows.push_back (std::move (row));
    };

    auto add_section = [&] (const std::string& title,
                            const std::vector<Account*>& list, bool negate)
    {
        if (list.empty ())
            return;
        if (options.label_sections)
            add_row (title, RowType::Section, zero_series (columns), false);
        for (Account* account : list)
        {
            const std::vector<Account*> subtree = descendants_in (account, list);
            if (!subtree.empty ())
            {
                const Series total = series_sum (subtree);
                if (options.show_zero_accounts || !series_all_zero (total))
                    add_row (statement_account_name (account,
                                                     options.account_full_name),
                             RowType::Account, total, negate);
                if (!series_all_zero (activity[account]))
                    add_row (statement_account_name (account,
                                                     options.account_full_name),
                             RowType::Account, activity[account], negate);
            }
            else if (options.show_zero_accounts ||
                     !series_all_zero (activity[account]))
                add_row (statement_account_name (account,
                                                 options.account_full_name),
                         RowType::Account, activity[account], negate);
        }
        if (options.total_sections)
            add_row ("Total For " + title, RowType::Total, series_sum (list),
                     negate);
    };

    const bool income_and_expense =
        !split.income.empty () && !split.expenses.empty ();
    /* The Scheme report puts the Net Income block in its left panel and the
     * Expense section in its right one, and only splits the report into those
     * panels in the single-column form; a period-column run prints Income,
     * Expense and then Net Income.  The row order follows the panels, so the
     * two forms differ here. */
    const bool panels = options.period == ColumnPeriod::Disabled;

    auto add_net_income = [&]
    {
        if (options.label_sections)
            add_row ("Net Income", RowType::Section, zero_series (columns),
                     false);
        add_row ("Total For Net Income", RowType::Total,
                 series_sum (income_expense_accounts (split)), true);
    };

    add_section ("Income", split.income, true);
    if (panels)
        add_net_income ();
    add_section ("Expense", split.expenses, false);
    if (income_and_expense && !panels)
        add_net_income ();

    return table;
}

Table
equity_statement (Account* root, const EquityStatementOptions& options)
{
    gnc_commodity* currency = report_currency (root, options.currency);
    const time64 start_printable =
        options.start.value_or (default_start_date ());
    const time64 end =
        gnc_time64_get_day_end (options.end.value_or (default_end_date ()));

    /* The report's opening balance is taken at the end of the day before the
     * date it prints in the "Capital, ..." heading. */
    GDate opening = time64_to_gdate (start_printable);
    g_date_subtract_days (&opening, 1);
    const time64 start = gnc_time64_get_day_end_gdate (&opening);

    Table table;
    table.columns = {Column::AccountName, Column::Balance};
    table.currency = currency;

    std::vector<Account*> accounts;
    if (root != nullptr)
        collect_accounts (root, accounts);
    const BucketAccounts split = split_accounts (accounts);

    /* The Scheme report's closing-entry pattern: a case-insensitive substring
     * of the description, or the transaction's closing flag. */
    auto closing_entry = [] (Split* item)
    {
        if (xaccTransGetIsClosingTxn (xaccSplitGetParent (item)))
            return true;
        const char* description =
            xaccTransGetDescription (xaccSplitGetParent (item));
        if (description == nullptr || description[0] == '\0')
            return false;
        gchar* haystack = g_ascii_strdown (description, -1);
        const bool found = std::strstr (haystack, "closing entries") != nullptr;
        g_free (haystack);
        return found;
    };

    /* A balance at a date over an account list, voided splits left out (the
     * Scheme report's query matches on the cleared state) and converted into
     * the report currency at that date. */
    auto balances_at = [&currency] (const std::vector<Account*>& list,
                                    time64 when)
    {
        gnc_numeric total = gnc_numeric_zero ();
        for (Account* account : list)
        {
            gnc_numeric balance = gnc_numeric_zero ();
            gnc_account_foreach_split_between_dates (
                account, std::nullopt, when, false,
                [&] (Split* item)
                {
                    if (xaccTransGetVoidStatus (xaccSplitGetParent (item)))
                        return;
                    balance = numeric_add (balance,
                                           xaccSplitGetAmount (item));
                });
            total = numeric_add (total,
                                 converted_at (account, balance, currency,
                                               when));
        }
        return total;
    };

    /* The splits of a period, optionally only those the report calls closing
     * entries, summed per account and converted at the period end.  `what`
     * selects which splits count: all of them, the closing ones, or the
     * positive (withdrawal) ones. */
    enum class Flow
    {
        All,
        Closing,
        Positive,
    };
    auto flow_at = [&closing_entry, &currency] (
        const std::vector<Account*>& list, time64 from, time64 to, Flow what)
    {
        gnc_numeric total = gnc_numeric_zero ();
        for (Account* account : list)
        {
            gnc_numeric sum = gnc_numeric_zero ();
            gnc_account_foreach_split_between_dates (
                account, std::optional<time64> (from), to, false,
                [&] (Split* item)
                {
                    if (xaccTransGetVoidStatus (xaccSplitGetParent (item)))
                        return;
                    const gnc_numeric value = xaccSplitGetAmount (item);
                    switch (what)
                    {
                    case Flow::All:
                        break;
                    case Flow::Closing:
                        if (!closing_entry (item))
                            return;
                        break;
                    case Flow::Positive:
                        if (!gnc_numeric_positive_p (value))
                            return;
                        break;
                    }
                    sum = numeric_add (sum, value);
                });
            total = numeric_add (total,
                                 converted_at (account, sum, currency, to));
        }
        return total;
    };

    const std::vector<Account*> income_expense = income_expense_accounts (split);

    /* The opening capital: equity plus the retained earnings the income and
     * expense accounts hold at the opening date, sign-reversed. */
    const gnc_numeric opening_equity = balances_at (split.equity, start);
    const gnc_numeric opening_retained = balances_at (income_expense, start);
    const gnc_numeric opening_capital =
        gnc_numeric_sub (gnc_numeric_neg (opening_equity), opening_retained,
                         GNC_DENOM_AUTO, GNC_HOW_DENOM_LCD);

    /* Net income: the period's income and expense activity, closing entries
     * taken back out, sign-reversed. */
    const gnc_numeric nonclosing = flow_at (income_expense, start, end,
                                            Flow::All);
    const gnc_numeric closing = flow_at (income_expense, start, end,
                                         Flow::Closing);
    const gnc_numeric net_income =
        gnc_numeric_sub (closing, nonclosing, GNC_DENOM_AUTO,
                         GNC_HOW_DENOM_LCD);

    /* Investments and withdrawals: the change in the equity accounts the
     * withdrawals do not explain.  The Scheme report's "neg-" variables hold
     * the raw balances (gnc:accounts-get-comm-total-assets() does not reverse
     * them), so the arithmetic below is the Scheme report's own. */
    const gnc_numeric ending_equity = balances_at (split.equity, end);
    const gnc_numeric equity_closing = flow_at (split.equity, start, end,
                                                Flow::Closing);
    const gnc_numeric pre_closing_equity =
        gnc_numeric_sub (ending_equity, equity_closing, GNC_DENOM_AUTO,
                         GNC_HOW_DENOM_LCD);
    const gnc_numeric net_investment =
        gnc_numeric_sub (opening_equity, pre_closing_equity, GNC_DENOM_AUTO,
                         GNC_HOW_DENOM_LCD);
    const gnc_numeric withdrawals = flow_at (split.equity, start, end,
                                             Flow::Positive);
    /* The Scheme report's capital-increase line is the period's net income
     * plus the investments it shows, less the withdrawals, which cancels the
     * withdrawals the investments line added: it is the net income plus the
     * net investment.  Transcribed rather than simplified so the two lines
     * read as the Scheme report's own arithmetic. */
    const gnc_numeric investments = numeric_add (net_investment, withdrawals);
    const gnc_numeric capital_increase = numeric_add (net_income, investments);
    const gnc_numeric capital_increase_net =
        gnc_numeric_sub (capital_increase, withdrawals, GNC_DENOM_AUTO,
                         GNC_HOW_DENOM_LCD);

    /* The closing capital is the opening one plus the increase. */
    const gnc_numeric ending_capital =
        numeric_add (opening_capital, capital_increase_net);

    auto add_amount = [&table] (const std::string& positive,
                                const char* negative, gnc_numeric amount,
                                bool subheading)
    {
        Row row;
        row.type = subheading ? RowType::Total : RowType::Account;
        const bool flip = negative != nullptr &&
                          gnc_numeric_negative_p (amount);
        row.label = flip ? negative : positive;
        row.amount = flip ? gnc_numeric_neg (amount) : amount;
        table.rows.push_back (std::move (row));
    };

    add_amount ("Capital, " + print_date (start_printable), nullptr,
                opening_capital, true);
    add_amount ("Net income for Period", "Net loss for Period", net_income,
                false);
    add_amount ("Investments for Period", nullptr, investments, false);
    add_amount ("Withdrawals for Period", nullptr, withdrawals, false);
    add_amount ("Increase in capital", "Decrease in capital",
                capital_increase_net, false);
    add_amount ("Capital, " + print_date (end), nullptr, ending_capital, true);

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

/* ------------------------------------------------------ portfolio listings */

namespace
{

/* The Scheme reports print units with gnc-share-print-info-places(), i.e. a
 * fixed number of decimals and no commodity: "130.00". */
std::string
format_shares (gnc_numeric amount, int places)
{
    return xaccPrintAmount (amount, gnc_share_print_info_places (places));
}

/* The price of one unit of `commodity` in `report_cur`, and the price row the
 * report prints.  The two are not the same lookup in the Scheme reports: the
 * Price column prints a price in whatever currency the price database has it,
 * while Value converts the units at the same source.  A book whose prices are
 * already in the report currency makes them agree, and that is the only case
 * the parity claim covers; for a price quoted the other way round the Scheme
 * report inverts it and so does this. */
struct PriceQuote
{
    /* Owned by the caller: gnc_price_unref() when done. */
    GNCPrice* price = nullptr;
    /* The price's value in its own currency (inverted when the lookup found
     * it quoted the other way round). */
    gnc_numeric value = gnc_numeric_zero ();
};

PriceQuote
quote_price (GNCPriceDB* db, gnc_commodity* commodity, gnc_commodity* report_cur,
             PriceSource source, time64 when)
{
    PriceQuote quote;
    if (db == nullptr || commodity == nullptr)
        return quote;

    GList* prices = nullptr;
    switch (source)
    {
    case PriceSource::Latest:
        prices = gnc_pricedb_lookup_latest_any_currency (db, commodity);
        break;
    case PriceSource::Before:
        prices = gnc_pricedb_lookup_nearest_before_any_currency_t64 (
            db, commodity, time64CanonicalDayTime (when));
        break;
    case PriceSource::Nearest:
        prices = gnc_pricedb_lookup_nearest_in_time_any_currency_t64 (
            db, commodity, time64CanonicalDayTime (when));
        break;
    }

    if (prices == nullptr)
        return quote;

    /* portfolio.scm takes the first price of the list, and inverts it only
     * when the report currency is its commodity.  Preferring a price quoted
     * in the report currency is what find-price() in the advanced report
     * does; the two agree whenever the database holds only one quote. */
    GNCPrice* first = static_cast<GNCPrice*> (prices->data);
    GNCPrice* chosen = nullptr;
    for (GList* node = prices; node != nullptr; node = node->next)
    {
        auto* price = static_cast<GNCPrice*> (node->data);
        if (report_cur != nullptr &&
            gnc_commodity_equiv (report_cur,
                                 gnc_price_get_currency (price)))
        {
            chosen = price;
            break;
        }
    }
    if (chosen == nullptr)
        chosen = first;

    if (gnc_commodity_equiv (commodity, gnc_price_get_commodity (chosen)))
    {
        gnc_price_ref (chosen);
        quote.price = chosen;
        quote.value = gnc_price_get_value (chosen);
    }
    else
    {
        quote.price = gnc_price_invert (chosen);
        quote.value = gnc_price_get_value (quote.price);
    }
    gnc_price_list_destroy (prices);
    return quote;
}

/* Convert `amount` of `from` into `report_cur` at the price source, which is
 * gnc:case-exchange-fn()'s own choice of engine helper. */
gnc_numeric
convert_at_source (gnc_numeric amount, gnc_commodity* from,
                   gnc_commodity* report_cur, GNCPriceDB* db, PriceSource source,
                   time64 when)
{
    if (from == nullptr || report_cur == nullptr ||
        gnc_commodity_equiv (from, report_cur))
        return amount;
    switch (source)
    {
    case PriceSource::Latest:
        return gnc_pricedb_convert_balance_latest_price (db, amount, from,
                                                        report_cur);
    case PriceSource::Before:
        return gnc_pricedb_convert_balance_nearest_before_price_t64 (
            db, amount, from, report_cur, time64CanonicalDayTime (when));
    case PriceSource::Nearest:
        return gnc_pricedb_convert_balance_nearest_price_t64 (
            db, amount, from, report_cur, time64CanonicalDayTime (when));
    }
    return amount;
}

/* lot-viewer.scm's txn-type-alist: the four names the report prints. */
const char*
transaction_type_name (Transaction* txn)
{
    switch (xaccTransGetTxnType (txn))
    {
    case TXN_TYPE_INVOICE: return "Inv";
    case TXN_TYPE_PAYMENT: return "Pmt";
    case TXN_TYPE_LINK:    return "Link";
    default:               return "None";
    }
}

/* One lot column's cell for a transaction: the amounts of the splits that
 * belong to the lot, or ":" when none do.  The Scheme report prints each
 * amount in the *account's* commodity, which is why the other accounts'
 * splits in the same transaction are printed as if they were shares.  With no
 * account chosen the commodity is null, and the engine prints a null-commodity
 * monetary with the default print info ("$20.00"), so the same fallback is
 * used here. */
Cell
lot_cell (const std::vector<Split*>& splits, gnc_commodity* currency)
{
    if (splits.empty ())
        return {":", "number-cell"};
    std::string text;
    for (Split* split : splits)
    {
        if (!text.empty ())
            text += " ";
        if (currency != nullptr)
            text += format_amount (xaccSplitGetAmount (split), currency);
        else
            text += xaccPrintAmount (xaccSplitGetAmount (split),
                                     gnc_default_print_info (TRUE));
    }
    return {text, "number-cell"};
}

/* The reports' default account selection: every stock and mutual fund account
 * below the root, in the engine's sorted order. */std::vector<Account*>
default_portfolio_accounts (Account* root)
{
    std::vector<Account*> all;
    collect_accounts (root, all);
    std::vector<Account*> selected;
    for (Account* account : all)
    {
        const GNCAccountType type = xaccAccountGetType (account);
        if (type == ACCT_TYPE_STOCK || type == ACCT_TYPE_MUTUAL)
            selected.push_back (account);
    }
    return selected;
}

/* The first account of a named selection: a report that publishes more than
 * one account list reads the ones after "accounts" from here. */
Account*
first_account_option (const ReportRequest& request, const std::string& name)
{
    const auto found = request.account_lists.find (name);
    if (found == request.account_lists.end () || found->second.empty ())
        return nullptr;
    return found->second.front ();
}

/* An unset account-selection option's value, which is what the engine's
 * GncOptionAccountSelValue::get_default_value() resolves: the first account
 * below the root, in the engine's sorted order, whose type is one of the
 * option's allowed types.  The IFRS report's five selections all have an
 * empty default and an allowed-type set, so this is how its "no options at
 * all" run decides which accounts are its stock, proceeds, dividend, capital
 * gains and fee accounts. */
Account*
first_account_of_types (Account* root,
                        std::initializer_list<GNCAccountType> types)
{
    if (root == nullptr)
        return nullptr;
    GList* descendants = gnc_account_get_descendants_sorted (root);
    Account* found = nullptr;
    for (GList* node = descendants; node != nullptr && found == nullptr;
         node = node->next)
    {
        Account* account = static_cast<Account*> (node->data);
        for (GNCAccountType type : types)
            if (xaccAccountGetType (account) == type)
            {
                found = account;
                break;
            }
    }
    g_list_free (descendants);
    return found;
}

} // namespace

PortfolioReport
investment_portfolio (Account* root, const PortfolioOptions& options,
                      gnc_commodity* requested)
{
    PortfolioReport report;
    report.title = "Investment Portfolio";

    gnc_commodity* report_cur = report_currency (root, requested);
    if (root == nullptr || report_cur == nullptr)
        return report;

    gchar* printed = nullptr;
    printed = gnc_date_interval_format (options.date, options.date);
    if (printed != nullptr)
    {
        report.title += " ";
        report.title += printed;
        g_free (printed);
    }

    GNCPriceDB* db =
        gnc_pricedb_get_db (qof_instance_get_book (QOF_INSTANCE (root)));

    std::vector<Account*> accounts = options.accounts;
    if (accounts.empty ())
        accounts = default_portfolio_accounts (root);

    report.table.headings = {"Account", "Symbol", "Listing", "Units", "Price",
                             "Value"};

    gnc_numeric total = zero_in (report_cur);
    bool any_row = false;
    for (Account* account : accounts)
    {
        gnc_commodity* commodity = xaccAccountGetCommodity (account);
        if (commodity == nullptr)
            continue;

        const gnc_numeric units =
            xaccAccountGetBalanceAsOfDate (account, options.date);
        if (!options.include_zero && gnc_numeric_zero_p (units))
            continue;

        PriceQuote quote = quote_price (db, commodity, report_cur,
                                        options.price_source, options.date);
        gnc_numeric value = convert_at_source (units, commodity, report_cur, db,
                                               options.price_source,
                                               options.date);
        total = gnc_numeric_add_fixed (total, value);

        std::vector<Cell> row;
        row.push_back ({account_name (account), "anchor-cell"});
        row.push_back ({gnc_commodity_get_mnemonic (commodity), "text-cell"});
        row.push_back ({gnc_commodity_get_namespace (commodity), "text-cell"});
        row.push_back ({format_shares (units, options.share_places),
                        "number-cell"});
        if (quote.price != nullptr)
            row.push_back (
                {format_amount (quote.value, gnc_price_get_currency (quote.price)),
                 "number-cell"});
        else
            row.push_back ({format_amount (gnc_numeric_zero (), report_cur),
                            "number-cell"});
        row.push_back ({format_amount (value, report_cur), "number-cell"});
        report.table.rows.push_back (std::move (row));
        any_row = true;

        if (quote.price != nullptr)
            gnc_price_unref (quote.price);
    }

    /* The Scheme report prints the rule row and the total whenever an account
     * selection exists, even when every account has been skipped. */
    if (any_row)
    {
        report.table.rows.push_back ({label_cell ("", "", 6)});
        std::vector<Cell> total_row;
        total_row.push_back (label_cell ("Total", "total-label-cell"));
        total_row.push_back (label_cell (format_amount (total, report_cur),
                                         "total-number-cell", 5));
        report.table.rows.push_back (std::move (total_row));
    }
    return report;
}

/* ------------------------------------------------------------- lot viewer */

LotViewerReport
lot_viewer (const LotViewerOptions& options)
{
    LotViewerReport report;
    report.title = "Lot Viewer";

    /* The Scheme option's unset value is a null account, and
     * xaccQueryAddSingleAccountMatch() with a null account adds no term at
     * all: the query then matches every non-voided split in the book and the
     * report lists every lot it can reach, with the amounts printed in the
     * default currency (xaccAccountGetCommodity(null) is null, and
     * gnc_commodity_print_info(null) is the default print info).  That is the
     * report's default page, so the native report reproduces it rather than
     * requiring an account. */
    Account* account = options.account;
    gnc_commodity* currency =
        account != nullptr ? xaccAccountGetCommodity (account) : nullptr;

    /* xaccQueryAddClearedMatch (CLEARED-ALL & ~CLEARED-VOIDED) plus the date
     * match: every non-voided split inside the period, of the account or --
     * with no account -- of the whole book. */
    const time64 start = gnc_time64_get_day_start (options.start);
    const time64 end = gnc_time64_get_day_end (options.end);

    std::vector<Split*> splits;
    std::vector<Account*> sources;
    if (account != nullptr)
        sources.push_back (account);
    else if (options.root != nullptr)
    {
        /* The whole book, so the null-account query's split set is
         * reproduced: the root account and every account below it. */
        sources.push_back (options.root);
        collect_accounts (options.root, sources);
    }

    for (Account* source : sources)
    {
        for (GList* node = xaccAccountGetSplitList (source); node != nullptr;
             node = node->next)
        {
            auto* split = static_cast<Split*> (node->data);
            Transaction* txn = xaccSplitGetParent (split);
            if (xaccTransGetVoidStatus (txn))
                continue;
            const time64 date = xaccTransGetDate (txn);
            if (date < start || date > end)
                continue;
            if (!options.description_filter.empty ())
            {
                const char* description = xaccTransGetDescription (txn);
                if (description == nullptr ||
                    strstr (description,
                            options.description_filter.c_str ()) == nullptr)
                    continue;
            }
            splits.push_back (split);
        }
    }

    /* qof-query-run() answers in date order, and the report takes its lot
     * columns in the order the lots first appear in that list. */
    std::stable_sort (splits.begin (), splits.end (),
                      [] (Split* left, Split* right)
                      {
                          Transaction* left_txn = xaccSplitGetParent (left);
                          Transaction* right_txn = xaccSplitGetParent (right);
                          const time64 left_date = xaccTransGetDate (left_txn);
                          const time64 right_date = xaccTransGetDate (right_txn);
                          if (left_date != right_date)
                              return left_date < right_date;
                          return xaccTransOrder (left_txn, right_txn) < 0;
                      });

    if (splits.empty ())
    {
        report.warning = "no-data";
        return report;
    }

    std::vector<GNCLot*> lots;
    std::vector<std::vector<Split*>> lot_splits;
    std::set<GNCLot*> seen;
    for (Split* split : splits)
    {
        GNCLot* lot = xaccSplitGetLot (split);
        if (lot == nullptr || !seen.insert (lot).second)
            continue;
        lots.push_back (lot);
        std::vector<Split*> members;
        for (GList* node = gnc_lot_get_split_list (lot); node != nullptr;
             node = node->next)
            members.push_back (static_cast<Split*> (node->data));
        lot_splits.push_back (std::move (members));
    }

    /* The transactions are the splits' parents, unique, in the engine's
     * transaction order. */
    std::vector<Transaction*> transactions;
    for (Split* split : splits)
    {
        Transaction* txn = xaccSplitGetParent (split);
        if (std::find (transactions.begin (), transactions.end (), txn) ==
            transactions.end ())
            transactions.push_back (txn);
    }
    std::stable_sort (transactions.begin (), transactions.end (),
                      [] (Transaction* left, Transaction* right)
                      { return xaccTransOrder (left, right) < 0; });

    CellTable& table = report.table;

    /* The two heading rows: the lot titles and their short GUIDs. */
    std::vector<Cell> titles;
    titles.push_back (label_cell ("Date", ""));
    titles.push_back (label_cell ("Desc", ""));
    titles.push_back (label_cell ("Type", ""));
    for (GNCLot* lot : lots)
    {
        const char* title = gnc_lot_get_title (lot);
        titles.push_back (
            label_cell (title != nullptr && title[0] != '\0' ? title : "None",
                        ""));
    }
    titles.push_back (label_cell ("Other Account", ""));
    table.rows.push_back (std::move (titles));

    std::vector<Cell> guids;
    guids.push_back (label_cell ("", ""));
    guids.push_back (label_cell ("", ""));
    guids.push_back (label_cell ("", ""));
    for (GNCLot* lot : lots)
    {
        gchar* guid = guid_to_string (gnc_lot_get_guid (lot));
        std::string text = guid != nullptr ? guid : "";
        g_free (guid);
        if (text.size () > 8)
            text.resize (8);
        guids.push_back (label_cell (text, ""));
    }
    guids.push_back (label_cell ("", ""));
    table.rows.push_back (std::move (guids));

    /* The document row: the invoice a lot came from, or nothing. */
    std::vector<Cell> documents;
    documents.push_back (label_cell ("", ""));
    documents.push_back (label_cell ("Document", ""));
    documents.push_back (label_cell ("", ""));
    for (GNCLot* lot : lots)
    {
        GncInvoice* invoice = gncInvoiceGetInvoiceFromLot (lot);
        documents.push_back (
            invoice != nullptr ? label_cell (gncInvoiceGetID (invoice), "")
                               : label_cell ("", ""));
    }
    documents.push_back (label_cell ("", ""));
    table.rows.push_back (std::move (documents));

    /* One row per transaction.  Every lot column holds the transaction's
     * splits that belong to that lot, and the last column the rest. */
    for (Transaction* txn : transactions)
    {
        std::vector<Cell> row;
        row.push_back (label_cell (printed_date (xaccTransGetDate (txn)), ""));
        const char* description = xaccTransGetDescription (txn);
        row.push_back (label_cell (description != nullptr ? description : "",
                                   ""));
        row.push_back (label_cell (transaction_type_name (txn), ""));

        std::vector<Split*> remaining;
        for (GList* node = xaccTransGetSplitList (txn); node != nullptr;
             node = node->next)
            remaining.push_back (static_cast<Split*> (node->data));

        for (size_t index = 0; index < lots.size (); ++index)
        {
            std::vector<Split*> in_lot;
            std::vector<Split*> rest;
            for (Split* split : remaining)
            {
                if (std::find (lot_splits[index].begin (),
                               lot_splits[index].end (),
                               split) != lot_splits[index].end ())
                    in_lot.push_back (split);
                else
                    rest.push_back (split);
            }
            row.push_back (lot_cell (in_lot, currency));
            remaining = std::move (rest);
        }
        row.push_back (lot_cell (remaining, currency));
        table.rows.push_back (std::move (row));
    }

    /* The balance row: each lot's balance, in the account's commodity. */
    std::vector<Cell> balances;
    balances.push_back (label_cell ("", ""));
    balances.push_back (label_cell ("Balance", ""));
    balances.push_back (label_cell ("", ""));
    for (GNCLot* lot : lots)
    {
        const gnc_numeric balance = gnc_lot_get_balance (lot);
        balances.push_back (
            {currency != nullptr
                 ? format_amount (balance, currency)
                 : xaccPrintAmount (balance, gnc_default_print_info (TRUE)),
             "number-cell"});
    }
    balances.push_back (label_cell ("", ""));
    table.rows.push_back (std::move (balances));
    return report;
}

/* ------------------------------------------------- advanced portfolio basis */

namespace
{

/* To avoid overflows the Scheme report computes prices and unit counts at a
 * fixed denominator of 1e8; these are its price-denom and units-denom. */
constexpr int price_denom = 100000000;
constexpr int units_denom = 100000000;

struct BasisParcel
{
    gnc_numeric units;
    gnc_numeric price;
};

using BasisList = std::vector<BasisParcel>;

/* sum-basis(): the parcels' units times their price, in the report currency. */
gnc_numeric
sum_basis (const BasisList& list, int currency_frac)
{
    gnc_numeric total = gnc_numeric_zero ();
    for (auto parcel = list.rbegin (); parcel != list.rend (); ++parcel)
        total = gnc_numeric_add (
            gnc_numeric_mul (parcel->units, parcel->price, currency_frac,
                             GNC_HOW_RND_ROUND),
            total, currency_frac, GNC_HOW_RND_ROUND);
    return total;
}

gnc_numeric
units_basis (const BasisList& list)
{
    gnc_numeric total = gnc_numeric_zero ();
    for (auto parcel = list.rbegin (); parcel != list.rend (); ++parcel)
        total = gnc_numeric_add (parcel->units, total, units_denom,
                                 GNC_HOW_RND_ROUND);
    return total;
}

/* apply-basis-ratio(): a stock split or a spin-off rescales the parcels. */
BasisList
apply_basis_ratio (const BasisList& list, gnc_numeric units_ratio,
                   gnc_numeric value_ratio)
{
    BasisList out;
    out.reserve (list.size ());
    for (const BasisParcel& parcel : list)
        out.push_back ({gnc_numeric_mul (units_ratio, parcel.units, units_denom,
                                        GNC_HOW_RND_ROUND),
                        gnc_numeric_mul (value_ratio, parcel.price, price_denom,
                                        GNC_HOW_RND_ROUND)});
    return out;
}

/* basis-builder(): the Scheme report's own accumulation, case for case.  A
 * purchase with a value adds units, a sale with a value removes them from the
 * head (FIFO) or the tail (LIFO) or from the running average, a units-only
 * change is a split and a value-only change is a spin-off. */
BasisList
basis_builder (const BasisList& list, gnc_numeric b_units, gnc_numeric b_value,
               const std::string& method, int currency_frac)
{
    const bool has_value = !gnc_numeric_zero_p (b_value);

    if (has_value && gnc_numeric_positive_p (b_units))
    {
        if (method == "average-basis" && !list.empty ())
        {
            const gnc_numeric units = gnc_numeric_add (
                b_units, list.front ().units, units_denom, GNC_HOW_RND_ROUND);
            const gnc_numeric value = gnc_numeric_add (
                b_value,
                gnc_numeric_mul (list.front ().units, list.front ().price,
                                 GNC_DENOM_AUTO, GNC_HOW_DENOM_REDUCE),
                GNC_DENOM_AUTO, GNC_HOW_DENOM_REDUCE);
            if (gnc_numeric_zero_p (units))
                return list;
            return {{units, gnc_numeric_div (value, units, price_denom,
                                             GNC_HOW_RND_ROUND)}};
        }
        BasisList out = list;
        out.push_back ({b_units, gnc_numeric_div (b_value, b_units, price_denom,
                                                 GNC_HOW_RND_ROUND)});
        return out;
    }

    if (has_value && gnc_numeric_negative_p (b_units))
    {
        if (list.empty ())
            return {};
        if (method == "fifo-basis")
        {
            const int compare = gnc_numeric_compare (gnc_numeric_abs (b_units),
                                                     list.front ().units);
            if (compare < 0)
            {
                BasisList out = list;
                out.front ().units = gnc_numeric_add (
                    b_units, list.front ().units, units_denom,
                    GNC_HOW_RND_ROUND);
                return out;
            }
            if (compare == 0)
                return BasisList (list.begin () + 1, list.end ());
            const BasisList rest (list.begin () + 1, list.end ());
            return basis_builder (
                rest,
                gnc_numeric_add (b_units, list.front ().units, units_denom,
                                 GNC_HOW_RND_ROUND),
                b_value, method, currency_frac);
        }
        if (method == "filo-basis")
        {
            BasisList reversed (list.rbegin (), list.rend ());
            const int compare = gnc_numeric_compare (gnc_numeric_abs (b_units),
                                                     reversed.front ().units);
            if (compare < 0)
            {
                reversed.front ().units = gnc_numeric_add (
                    b_units, reversed.front ().units, units_denom,
                    GNC_HOW_RND_ROUND);
                std::reverse (reversed.begin (), reversed.end ());
                return reversed;
            }
            if (compare == 0)
            {
                reversed.erase (reversed.begin ());
                std::reverse (reversed.begin (), reversed.end ());
                return reversed;
            }
            const BasisList rest (reversed.begin () + 1, reversed.end ());
            BasisList out = basis_builder (
                rest,
                gnc_numeric_add (b_units, reversed.front ().units, units_denom,
                                 GNC_HOW_RND_ROUND),
                b_value, method, currency_frac);
            std::reverse (out.begin (), out.end ());
            return out;
        }
        /* average-basis: the sale takes units out at the running average. */
        BasisList out = list;
        out.front ().units = gnc_numeric_add (list.front ().units, b_units,
                                              units_denom, GNC_HOW_RND_ROUND);
        return out;
    }

    if (!has_value && !gnc_numeric_zero_p (b_units))
    {
        /* A split or a merger: rescale the units, and the value the other way
         * round so the basis does not move. */
        const gnc_numeric current = units_basis (list);
        gnc_numeric units_ratio = gnc_numeric_zero ();
        if (!gnc_numeric_zero_p (current))
            units_ratio = gnc_numeric_div (
                gnc_numeric_add (b_units, current, GNC_DENOM_AUTO,
                                 GNC_HOW_DENOM_REDUCE),
                current, GNC_DENOM_AUTO, GNC_HOW_DENOM_REDUCE);
        gnc_numeric value_ratio = gnc_numeric_zero ();
        if (!gnc_numeric_zero_p (units_ratio))
            value_ratio = gnc_numeric_div (gnc_numeric_create (1, 1), units_ratio,
                                          GNC_DENOM_AUTO,
                                          GNC_HOW_DENOM_REDUCE);
        return apply_basis_ratio (list, units_ratio, value_ratio);
    }

    if (gnc_numeric_zero_p (b_units) && has_value)
    {
        /* A spin-off: rescale the values, leave the units alone. */
        const gnc_numeric current = sum_basis (list, currency_frac);
        if (gnc_numeric_zero_p (current))
            return list;
        const gnc_numeric value_ratio = gnc_numeric_div (
            gnc_numeric_add (b_value, current, GNC_DENOM_AUTO,
                             GNC_HOW_DENOM_REDUCE),
            current, GNC_DENOM_AUTO, GNC_HOW_DENOM_REDUCE);
        return apply_basis_ratio (list, gnc_numeric_create (1, 1), value_ratio);
    }

    return list;
}

/* spin-off?(): a no-units split whose transaction has exactly one other split,
 * which is not in an income or expense account. */
bool
is_spin_off (Split* split, Account* current)
{
    Split* other = xaccSplitGetOtherSplit (split);
    if (other == nullptr)
        return false;
    if (!gnc_numeric_zero_p (xaccSplitGetAmount (split)))
        return false;
    if (xaccSplitGetAccount (split) != current)
        return false;
    const GNCAccountType type = xaccAccountGetType (xaccSplitGetAccount (other));
    return type != ACCT_TYPE_EXPENSE && type != ACCT_TYPE_INCOME;
}

} // namespace

PortfolioReport
advanced_portfolio (Account* root, const PortfolioOptions& options,
                    gnc_commodity* requested)
{
    PortfolioReport report;
    report.title = "Advanced Portfolio";

    gnc_commodity* report_cur = report_currency (root, requested);
    if (root == nullptr || report_cur == nullptr)
        return report;

    gchar* printed = gnc_date_interval_format (options.date, options.date);
    if (printed != nullptr)
    {
        report.title += " ";
        report.title += printed;
        g_free (printed);
    }

    GNCPriceDB* db =
        gnc_pricedb_get_db (qof_instance_get_book (QOF_INSTANCE (root)));
    const int currency_frac = gnc_commodity_get_fraction (report_cur);

    std::vector<Account*> accounts = options.accounts;
    if (accounts.empty ())
        accounts = default_portfolio_accounts (root);

    const bool show_brokerage = options.brokerage_fees != "ignore-brokerage";

    std::vector<std::string> headings = {"Account"};
    if (options.show_symbol)
        headings.push_back ("Symbol");
    if (options.show_listing)
        headings.push_back ("Listing");
    if (options.show_shares)
        headings.push_back ("Shares");
    if (options.show_price)
        headings.push_back ("Price");
    headings.push_back (" ");
    for (const char* heading :
         {"Basis", "Value", "Money In", "Money Out", "Realized Gain",
          "Unrealized Gain", "Total Gain", "Rate of Gain", "Income"})
        headings.push_back (heading);
    if (show_brokerage)
        headings.push_back ("Brokerage Fees");
    headings.push_back ("Total Return");
    headings.push_back ("Rate of Return");
    report.table.headings = headings;

    /* The totals the Scheme report accumulates in commodity collectors, which
     * for a single-currency book are plain sums in the report currency. */
    gnc_numeric total_basis = zero_in (report_cur);
    gnc_numeric total_value = zero_in (report_cur);
    gnc_numeric total_money_in = zero_in (report_cur);
    gnc_numeric total_money_out = zero_in (report_cur);
    gnc_numeric total_gain = zero_in (report_cur);
    gnc_numeric total_ugain = zero_in (report_cur);
    gnc_numeric total_income = zero_in (report_cur);
    gnc_numeric total_brokerage = zero_in (report_cur);

    auto rate_cell = [] (gnc_numeric gain, gnc_numeric money_in)
    {
        if (gnc_numeric_zero_p (money_in))
            return Cell {"", "number-cell"};
        const double rate = 100.0 * gnc_numeric_to_double (gain) /
                            gnc_numeric_to_double (money_in);
        char text[32] = {0};
        g_snprintf (text, sizeof text, "%.2f%%", rate);
        return Cell {text, "number-cell"};
    };

    for (Account* account : accounts)
    {
        gnc_commodity* commodity = xaccAccountGetCommodity (account);
        if (commodity == nullptr)
            continue;

        const gnc_numeric shares =
            xaccAccountGetBalanceAsOfDate (account, options.date);
        if (!options.include_zero && gnc_numeric_zero_p (shares))
            continue;

        /* The price at the report date.  With no price the Scheme report
         * falls back to a transaction's share price (and prints "*"), and
         * then to 1 (and prints "**"); the native report does not read
         * transaction prices, so it uses 1 and says so. */
        PriceQuote quote = quote_price (db, commodity, report_cur,
                                        options.price_source, options.date);
        bool have_price = quote.price != nullptr;
        gnc_numeric price_value =
            have_price ? quote.value : gnc_numeric_create (1, 1);
        gnc_numeric value;
        if (have_price &&
            gnc_commodity_equiv (gnc_price_get_currency (quote.price),
                                 report_cur))
            value = gnc_numeric_mul (shares, price_value, currency_frac,
                                     GNC_HOW_RND_ROUND);
        else if (have_price)
            value = convert_to_report_currency (
                gnc_numeric_mul (shares, price_value, currency_frac,
                                 GNC_HOW_RND_ROUND),
                gnc_price_get_currency (quote.price), report_cur, db,
                options.date);
        else
            value = gnc_numeric_mul (shares, price_value, currency_frac,
                                     GNC_HOW_RND_ROUND);

        BasisList basis_list;
        gnc_numeric money_in = zero_in (report_cur);
        gnc_numeric money_out = zero_in (report_cur);
        gnc_numeric gain = zero_in (report_cur);
        gnc_numeric income = zero_in (report_cur);
        gnc_numeric brokerage = zero_in (report_cur);

        std::set<Transaction*> seen;
        for (GList* node = xaccAccountGetSplitList (account); node != nullptr;
             node = node->next)
        {
            auto* split = static_cast<Split*> (node->data);
            Transaction* txn = xaccSplitGetParent (split);
            if (xaccTransGetDate (txn) > options.date)
                continue;
            if (!seen.insert (txn).second)
                continue;

            gnc_commodity* txn_currency = xaccTransGetCurrency (txn);
            const int txn_frac = gnc_commodity_get_fraction (txn_currency);

            gnc_numeric trans_income = gnc_numeric_zero ();
            gnc_numeric trans_brokerage = gnc_numeric_zero ();
            gnc_numeric trans_shares = gnc_numeric_zero ();
            gnc_numeric shares_bought = gnc_numeric_zero ();
            gnc_numeric shares_sold = gnc_numeric_zero ();
            gnc_numeric trans_sold = gnc_numeric_zero ();
            gnc_numeric trans_bought = gnc_numeric_zero ();
            gnc_numeric trans_spinoff = gnc_numeric_zero ();

            /* Pass 1: what the transaction does to the account's income, its
             * fees and its share count. */
            for (GList* item = xaccTransGetSplitList (txn); item != nullptr;
                 item = item->next)
            {
                auto* other = static_cast<Split*> (item->data);
                const gnc_numeric split_units = xaccSplitGetAmount (other);
                const gnc_numeric split_value = xaccSplitGetValue (other);
                const GNCAccountType type =
                    xaccAccountGetType (xaccSplitGetAccount (other));

                if (type == ACCT_TYPE_EXPENSE)
                {
                    /* A commission, unless this is a stock donation: a
                     * two-split transaction whose other split is in this
                     * account. */
                    Split* counterpart = xaccSplitGetOtherSplit (other);
                    if (counterpart == nullptr ||
                        xaccSplitGetAccount (counterpart) != account)
                        trans_brokerage = gnc_numeric_add (
                            trans_brokerage, split_value, txn_frac,
                            GNC_HOW_RND_ROUND);
                }
                else if (type == ACCT_TYPE_INCOME)
                    trans_income = gnc_numeric_sub (trans_income, split_value,
                                                    txn_frac, GNC_HOW_RND_ROUND);
                else if (xaccSplitGetAccount (other) == account)
                {
                    trans_shares = gnc_numeric_add (
                        trans_shares, gnc_numeric_abs (split_units),
                        units_denom, GNC_HOW_RND_ROUND);
                    if (gnc_numeric_zero_p (split_units))
                    {
                        if (is_spin_off (other, account))
                        {
                            if (gnc_numeric_negative_p (split_value))
                                trans_spinoff = gnc_numeric_sub (
                                    trans_spinoff, split_value, txn_frac,
                                    GNC_HOW_RND_ROUND);
                        }
                        else if (!gnc_numeric_zero_p (split_value))
                        {
                            /* A zero-share split is the register's realised
                             * gain, whose matching income split has already
                             * been counted: take it out again. */
                            trans_income = gnc_numeric_sub (
                                trans_income, split_value, txn_frac,
                                GNC_HOW_RND_ROUND);
                        }
                    }
                    else if (gnc_numeric_positive_p (split_value))
                    {
                        trans_bought = gnc_numeric_add (
                            trans_bought, split_value, txn_frac,
                            GNC_HOW_RND_ROUND);
                        shares_bought = gnc_numeric_add (shares_bought,
                                                         split_units,
                                                         units_denom,
                                                         GNC_HOW_RND_ROUND);
                    }
                    else
                    {
                        trans_sold = gnc_numeric_sub (trans_sold, split_value,
                                                      txn_frac,
                                                      GNC_HOW_RND_ROUND);
                        shares_sold = gnc_numeric_add (shares_sold, split_units,
                                                       units_denom,
                                                       GNC_HOW_RND_ROUND);
                    }
                }
            }

            income = gnc_numeric_add (income,
                                      convert_to_report_currency (
                                          trans_income, txn_currency,
                                          report_cur, db, options.date),
                                      currency_frac, GNC_HOW_RND_ROUND);
            brokerage = gnc_numeric_add (
                brokerage,
                convert_to_report_currency (trans_brokerage, txn_currency,
                                            report_cur, db, options.date),
                currency_frac, GNC_HOW_RND_ROUND);

            /* Share the transaction's fees between its purchases and its
             * sales, in proportion to the shares, unless they are being
             * omitted. */
            if (show_brokerage && gnc_numeric_positive_p (trans_brokerage) &&
                gnc_numeric_positive_p (trans_shares))
            {
                const gnc_numeric bought_fraction = gnc_numeric_div (
                    shares_bought, trans_shares, GNC_DENOM_AUTO,
                    GNC_HOW_DENOM_REDUCE);
                trans_bought = gnc_numeric_add (
                    trans_bought,
                    gnc_numeric_mul (trans_brokerage, bought_fraction, txn_frac,
                                     GNC_HOW_RND_ROUND),
                    txn_frac, GNC_HOW_RND_ROUND);
                const gnc_numeric sold_fraction = gnc_numeric_div (
                    shares_sold, trans_shares, GNC_DENOM_AUTO,
                    GNC_HOW_DENOM_REDUCE);
                trans_sold = gnc_numeric_add (
                    trans_sold,
                    gnc_numeric_mul (trans_brokerage, sold_fraction, txn_frac,
                                     GNC_HOW_RND_ROUND),
                    txn_frac, GNC_HOW_RND_ROUND);
            }

            /* A purchase paid for out of the transaction's income (a dividend
             * reinvestment) is not new money in.  The Scheme report adjusts
             * it by the residual of a dividend-reinvestment holding account,
             * which the native report does not detect. */
            if (gnc_numeric_positive_p (trans_income) &&
                gnc_numeric_positive_p (trans_bought))
            {
                trans_bought = gnc_numeric_sub (trans_bought, trans_income,
                                                txn_frac, GNC_HOW_RND_ROUND);
                if (gnc_numeric_negative_p (trans_bought))
                    trans_bought = gnc_numeric_zero ();
            }

            money_in = gnc_numeric_add (
                money_in,
                convert_to_report_currency (trans_bought, txn_currency,
                                            report_cur, db, options.date),
                currency_frac, GNC_HOW_RND_ROUND);
            money_out = gnc_numeric_add (
                money_out,
                convert_to_report_currency (
                    gnc_numeric_add (trans_sold, trans_spinoff, txn_frac,
                                     GNC_HOW_RND_ROUND),
                    txn_currency, report_cur, db, options.date),
                currency_frac, GNC_HOW_RND_ROUND);

            /* Pass 2: move the basis and take the realised gain. */
            for (GList* item = xaccTransGetSplitList (txn); item != nullptr;
                 item = item->next)
            {
                auto* other = static_cast<Split*> (item->data);
                const gnc_numeric split_units = xaccSplitGetAmount (other);
                const gnc_numeric split_value = xaccSplitGetValue (other);

                if (!gnc_numeric_zero_p (split_units) &&
                    xaccSplitGetAccount (other) == account)
                {
                    const gnc_numeric split_value_currency =
                        convert_to_report_currency (split_value, txn_currency,
                                                    report_cur, db,
                                                    options.date);
                    const gnc_numeric orig_basis =
                        sum_basis (basis_list, currency_frac);
                    gnc_numeric fees_currency = gnc_numeric_zero ();
                    if (!gnc_numeric_zero_p (trans_shares))
                    {
                        const gnc_numeric fee_ratio = gnc_numeric_div (
                            gnc_numeric_abs (split_units), trans_shares,
                            GNC_DENOM_AUTO, GNC_HOW_DENOM_REDUCE);
                        fees_currency = convert_to_report_currency (
                            gnc_numeric_mul (fee_ratio, trans_brokerage,
                                             txn_frac, GNC_HOW_RND_ROUND),
                            txn_currency, report_cur, db, options.date);
                    }
                    const gnc_numeric split_value_with_fees =
                        options.brokerage_fees == "include-in-basis"
                            ? gnc_numeric_add (split_value_currency,
                                               fees_currency, currency_frac,
                                               GNC_HOW_RND_ROUND)
                            : split_value_currency;

                    basis_list = basis_builder (basis_list, split_units,
                                                split_value_with_fees,
                                                options.basis_method,
                                                currency_frac);

                    if (!gnc_numeric_positive_p (split_value))
                    {
                        const gnc_numeric new_basis =
                            sum_basis (basis_list, currency_frac);
                        if (gnc_numeric_zero_p (new_basis) ||
                            gnc_numeric_negative_p (split_value))
                        {
                            const gnc_numeric disposal_gain = gnc_numeric_sub (
                                gnc_numeric_abs (split_value_with_fees),
                                gnc_numeric_sub (orig_basis, new_basis,
                                                 currency_frac,
                                                 GNC_HOW_RND_ROUND),
                                currency_frac, GNC_HOW_RND_ROUND);
                            gain = gnc_numeric_add (gain, disposal_gain,
                                                    currency_frac,
                                                    GNC_HOW_RND_ROUND);
                        }
                    }
                }
                else if (is_spin_off (other, account))
                {
                    basis_list = basis_builder (
                        basis_list, split_units,
                        convert_to_report_currency (split_value, txn_currency,
                                                    report_cur, db,
                                                    options.date),
                        options.basis_method, currency_frac);
                }
            }
        }

        const gnc_numeric basis = sum_basis (basis_list, currency_frac);
        const gnc_numeric ugain =
            gnc_numeric_sub (value, basis, currency_frac, GNC_HOW_RND_ROUND);
        const gnc_numeric both_gain =
            gnc_numeric_add (gain, ugain, currency_frac, GNC_HOW_RND_ROUND);
        const gnc_numeric total_return = gnc_numeric_add (both_gain, income,
                                                          currency_frac,
                                                          GNC_HOW_RND_ROUND);

        std::vector<Cell> row;
        row.push_back ({account_name (account), "anchor-cell"});
        if (options.show_symbol)
            row.push_back ({gnc_commodity_get_mnemonic (commodity), "text-cell"});
        if (options.show_listing)
            row.push_back ({gnc_commodity_get_namespace (commodity),
                            "text-cell"});
        if (options.show_shares)
            row.push_back ({format_shares (shares, options.share_places),
                            "number-cell"});
        if (options.show_price)
        {
            if (have_price)
                row.push_back (
                    {format_amount (price_value,
                                    gnc_price_get_currency (quote.price)),
                     "number-cell"});
            else
                row.push_back ({format_amount (price_value, report_cur),
                                "number-cell"});
        }
        row.push_back ({have_price ? " " : "**", "text-cell"});
        row.push_back ({format_amount (basis, report_cur), "number-cell"});
        row.push_back ({format_amount (value, report_cur), "number-cell"});
        row.push_back ({format_amount (money_in, report_cur), "number-cell"});
        row.push_back ({format_amount (money_out, report_cur), "number-cell"});
        row.push_back ({format_amount (gain, report_cur), "number-cell"});
        row.push_back ({format_amount (ugain, report_cur), "number-cell"});
        row.push_back ({format_amount (both_gain, report_cur), "number-cell"});
        row.push_back (rate_cell (both_gain, money_in));
        row.push_back ({format_amount (income, report_cur), "number-cell"});
        if (show_brokerage)
            row.push_back ({format_amount (brokerage, report_cur),
                            "number-cell"});
        row.push_back ({format_amount (total_return, report_cur),
                        "number-cell"});
        row.push_back (rate_cell (total_return, money_in));
        report.table.rows.push_back (std::move (row));

        total_basis = gnc_numeric_add_fixed (total_basis, basis);
        total_value = gnc_numeric_add_fixed (total_value, value);
        total_money_in = gnc_numeric_add_fixed (total_money_in, money_in);
        total_money_out = gnc_numeric_add_fixed (total_money_out, money_out);
        total_gain = gnc_numeric_add_fixed (total_gain, gain);
        total_ugain = gnc_numeric_add_fixed (total_ugain, ugain);
        total_income = gnc_numeric_add_fixed (total_income, income);
        total_brokerage = gnc_numeric_add_fixed (total_brokerage, brokerage);

        if (quote.price != nullptr)
            gnc_price_unref (quote.price);
    }

    if (!report.table.rows.empty ())
    {
        const gnc_numeric total_both_gain = gnc_numeric_add_fixed (
            total_gain, total_ugain);
        const gnc_numeric total_return = gnc_numeric_add_fixed (
            total_both_gain, total_income);

        report.table.rows.push_back ({label_cell ("", "", headings.size ())});
        std::vector<Cell> totals;
        totals.push_back (label_cell ("Total", "total-label-cell"));
        int spacer = 0;
        if (options.show_symbol)
            ++spacer;
        if (options.show_listing)
            ++spacer;
        if (options.show_shares)
            ++spacer;
        if (options.show_price)
            ++spacer;
        ++spacer; /* the price-source marker */
        for (int index = 0; index < spacer; ++index)
            totals.push_back (label_cell ("", "total-number-cell"));
        totals.push_back (label_cell (format_amount (total_basis, report_cur),
                                      "total-number-cell"));
        totals.push_back (label_cell (format_amount (total_value, report_cur),
                                      "total-number-cell"));
        totals.push_back (label_cell (format_amount (total_money_in, report_cur),
                                      "total-number-cell"));
        totals.push_back (
            label_cell (format_amount (total_money_out, report_cur),
                        "total-number-cell"));
        totals.push_back (label_cell (format_amount (total_gain, report_cur),
                                      "total-number-cell"));
        totals.push_back (label_cell (format_amount (total_ugain, report_cur),
                                      "total-number-cell"));
        totals.push_back (
            label_cell (format_amount (total_both_gain, report_cur),
                        "total-number-cell"));
        const Cell rate = rate_cell (total_both_gain, total_money_in);
        totals.push_back (label_cell (rate.text, "total-number-cell"));
        totals.push_back (label_cell (format_amount (total_income, report_cur),
                                      "total-number-cell"));
        if (show_brokerage)
            totals.push_back (
                label_cell (format_amount (total_brokerage, report_cur),
                            "total-number-cell"));
        totals.push_back (
            label_cell (format_amount (total_return, report_cur),
                        "total-number-cell"));
        const Cell total_rate = rate_cell (total_return, total_money_in);
        totals.push_back (label_cell (total_rate.text, "total-number-cell"));
        report.table.rows.push_back (std::move (totals));
    }
    return report;
}

/* --------------------------------------------------------- IFRS cost basis */

namespace
{

/* The Scheme report's per-transaction amounts: #f for "the transaction has no
 * split in that account". */
struct TxnInfo
{
    std::optional<gnc_numeric> stock_amt;
    std::optional<gnc_numeric> stock_val;
    std::optional<gnc_numeric> proceeds_val;
    std::optional<gnc_numeric> fees_cap_val;
    std::optional<gnc_numeric> fees_exp_val;
    std::optional<gnc_numeric> dividend_val;
    std::optional<gnc_numeric> capgains_val;
};

void
add_optional (std::optional<gnc_numeric>& into, gnc_numeric value)
{
    into = gnc_numeric_add_fixed (into.value_or (gnc_numeric_zero ()), value);
}

/* The sign tests the report's type table uses, with the Scheme definitions
 * they come from.  A #f amount never matches a "greater than" test, always
 * matches a "greater or equal" one, and matches `....` ("is zero or absent")
 * and `DrCr` ("any") -- but a *number* still has to be zero for `....`, which
 * is what tells a stock split (no value) from a purchase (a value). */
enum class SignTest
{
    DrGt0,
    CrGt0,
    DrGe0,
    CrGe0,
    /* `....`: zero, or the transaction has no such amount. */
    ZeroOrAbsent,
    /* `DrCr`: always true. */
    Always,
};

bool
sign_test (SignTest test, const std::optional<gnc_numeric>& value)
{
    switch (test)
    {
    case SignTest::DrGt0:
        return value.has_value () && gnc_numeric_positive_p (*value);
    case SignTest::CrGt0:
        return value.has_value () && gnc_numeric_negative_p (*value);
    case SignTest::DrGe0:
        return !value.has_value () || !gnc_numeric_negative_p (*value);
    case SignTest::CrGe0:
        return !value.has_value () || !gnc_numeric_positive_p (*value);
    case SignTest::ZeroOrAbsent:
        return !value.has_value () || gnc_numeric_zero_p (*value);
    case SignTest::Always:
        return true;
    }
    return true;
}

struct TxnType
{
    SignTest amt;
    SignTest val;
    SignTest proceeds;
    SignTest fees_cap;
    SignTest fees_exp;
    SignTest dividend;
    SignTest capgains;
    bool purchase;
    bool sale;
    const char* narrative;
};

/* open-types, long-types and short-types, transcribed row for row: the first
 * row all of whose tests pass names the transaction.  The columns are the
 * amount, value, proceeds, capitalised fees, expensed fees, dividend and
 * capital gain, in that order (the order txn-identify() destructures). */
const TxnType open_types[] = {
    {SignTest::DrGt0, SignTest::DrGt0, SignTest::CrGt0, SignTest::DrGe0,
     SignTest::ZeroOrAbsent, SignTest::ZeroOrAbsent, SignTest::ZeroOrAbsent,
     true, false, "Open Long"},
    {SignTest::CrGt0, SignTest::CrGt0, SignTest::DrGt0, SignTest::DrGe0,
     SignTest::ZeroOrAbsent, SignTest::ZeroOrAbsent, SignTest::ZeroOrAbsent,
     true, false, "Open Short"},
};

const TxnType long_types[] = {
    {SignTest::DrGt0, SignTest::DrGt0, SignTest::CrGt0, SignTest::DrGe0,
     SignTest::ZeroOrAbsent, SignTest::ZeroOrAbsent, SignTest::ZeroOrAbsent,
     true, false, "Buy"},
    {SignTest::CrGt0, SignTest::CrGt0, SignTest::DrGe0, SignTest::ZeroOrAbsent,
     SignTest::DrGe0, SignTest::ZeroOrAbsent, SignTest::Always, false, true,
     "Sell"},
    {SignTest::ZeroOrAbsent, SignTest::ZeroOrAbsent, SignTest::DrGt0,
     SignTest::ZeroOrAbsent, SignTest::DrGe0, SignTest::CrGt0,
     SignTest::ZeroOrAbsent, false, false, "Dividend"},
    {SignTest::DrGt0, SignTest::DrGt0, SignTest::DrGt0, SignTest::DrGe0,
     SignTest::ZeroOrAbsent, SignTest::CrGt0, SignTest::ZeroOrAbsent, true,
     false, "Dividend reinvestment (w/ remainder)"},
    {SignTest::DrGt0, SignTest::DrGt0, SignTest::CrGe0, SignTest::DrGe0,
     SignTest::ZeroOrAbsent, SignTest::CrGt0, SignTest::ZeroOrAbsent, true,
     false, "Dividend reinvestment (w/o remainder)"},
    {SignTest::ZeroOrAbsent, SignTest::CrGt0, SignTest::DrGt0,
     SignTest::DrGe0, SignTest::ZeroOrAbsent, SignTest::ZeroOrAbsent,
     SignTest::ZeroOrAbsent, true, false, "Return of capital"},
    {SignTest::ZeroOrAbsent, SignTest::CrGt0, SignTest::ZeroOrAbsent,
     SignTest::DrGe0, SignTest::ZeroOrAbsent, SignTest::DrGt0,
     SignTest::ZeroOrAbsent, true, false,
     "Return of capital (reclassification)"},
    {SignTest::ZeroOrAbsent, SignTest::DrGt0, SignTest::ZeroOrAbsent,
     SignTest::ZeroOrAbsent, SignTest::DrGe0, SignTest::CrGt0,
     SignTest::ZeroOrAbsent, true, false, "Notional distribution (dividend)"},
    {SignTest::ZeroOrAbsent, SignTest::DrGt0, SignTest::ZeroOrAbsent,
     SignTest::ZeroOrAbsent, SignTest::DrGe0, SignTest::ZeroOrAbsent,
     SignTest::CrGt0, true, false,
     "Notional distribution (capital gain)"},
    {SignTest::DrGt0, SignTest::ZeroOrAbsent, SignTest::CrGe0,
     SignTest::DrGe0, SignTest::ZeroOrAbsent, SignTest::ZeroOrAbsent,
     SignTest::ZeroOrAbsent, false, false, "Stock split"},
    {SignTest::CrGt0, SignTest::ZeroOrAbsent, SignTest::CrGe0,
     SignTest::DrGe0, SignTest::ZeroOrAbsent, SignTest::ZeroOrAbsent,
     SignTest::ZeroOrAbsent, false, false, "Reverse split"},
    {SignTest::CrGt0, SignTest::CrGt0, SignTest::DrGt0, SignTest::ZeroOrAbsent,
     SignTest::DrGe0, SignTest::ZeroOrAbsent, SignTest::Always, false, true,
     "Reverse split w/ cash in lieu for fractionals"},
};

const TxnType short_types[] = {
    {SignTest::CrGt0, SignTest::CrGt0, SignTest::DrGt0, SignTest::DrGe0,
     SignTest::ZeroOrAbsent, SignTest::ZeroOrAbsent, SignTest::ZeroOrAbsent,
     true, false, "Short Sell"},
    {SignTest::DrGt0, SignTest::DrGt0, SignTest::CrGe0, SignTest::ZeroOrAbsent,
     SignTest::DrGe0, SignTest::ZeroOrAbsent, SignTest::Always, false, true,
     "Cover Buy"},
    {SignTest::ZeroOrAbsent, SignTest::ZeroOrAbsent, SignTest::CrGt0,
     SignTest::ZeroOrAbsent, SignTest::DrGe0, SignTest::DrGt0,
     SignTest::ZeroOrAbsent, false, false, "Compensatory dividend"},
    {SignTest::ZeroOrAbsent, SignTest::DrGt0, SignTest::CrGt0,
     SignTest::DrGe0, SignTest::ZeroOrAbsent, SignTest::ZeroOrAbsent,
     SignTest::ZeroOrAbsent, true, false, "Compensatory return of capital"},
    {SignTest::ZeroOrAbsent, SignTest::DrGt0, SignTest::ZeroOrAbsent,
     SignTest::DrGe0, SignTest::ZeroOrAbsent, SignTest::CrGt0,
     SignTest::ZeroOrAbsent, true, false,
     "Compensatory return of capital (reclassification)"},
    {SignTest::ZeroOrAbsent, SignTest::CrGt0, SignTest::ZeroOrAbsent,
     SignTest::ZeroOrAbsent, SignTest::DrGe0, SignTest::DrGt0,
     SignTest::ZeroOrAbsent, true, false,
     "Compensatory notional distribution (dividend)"},
    {SignTest::ZeroOrAbsent, SignTest::CrGt0, SignTest::ZeroOrAbsent,
     SignTest::ZeroOrAbsent, SignTest::DrGe0, SignTest::ZeroOrAbsent,
     SignTest::DrGt0, true, false,
     "Compensatory notional distribution (capital gain)"},
    {SignTest::CrGt0, SignTest::ZeroOrAbsent, SignTest::CrGe0,
     SignTest::DrGe0, SignTest::ZeroOrAbsent, SignTest::ZeroOrAbsent,
     SignTest::ZeroOrAbsent, false, false, "Stock split"},
    {SignTest::DrGt0, SignTest::ZeroOrAbsent, SignTest::CrGe0,
     SignTest::DrGe0, SignTest::ZeroOrAbsent, SignTest::ZeroOrAbsent,
     SignTest::ZeroOrAbsent, false, false, "Reverse split"},
    {SignTest::DrGt0, SignTest::DrGt0, SignTest::CrGt0, SignTest::ZeroOrAbsent,
     SignTest::DrGe0, SignTest::ZeroOrAbsent, SignTest::Always, false, true,
     "Reverse split w/ cash in lieu for fractionals"},
};

struct Identification
{
    bool purchase = false;
    bool sale = false;
    const char* narrative = "Unknown";
};

Identification
identify_transaction (const TxnInfo& info, gnc_numeric cumulative_units)
{
    const TxnType* table = long_types;
    size_t count = sizeof long_types / sizeof long_types[0];
    if (gnc_numeric_negative_p (cumulative_units))
    {
        table = short_types;
        count = sizeof short_types / sizeof short_types[0];
    }
    else if (gnc_numeric_zero_p (cumulative_units))
    {
        table = open_types;
        count = sizeof open_types / sizeof open_types[0];
    }

    for (size_t index = 0; index < count; ++index)
    {
        const TxnType& type = table[index];
        if (sign_test (type.amt, info.stock_amt) &&
            sign_test (type.val, info.stock_val) &&
            sign_test (type.proceeds, info.proceeds_val) &&
            sign_test (type.fees_cap, info.fees_cap_val) &&
            sign_test (type.fees_exp, info.fees_exp_val) &&
            sign_test (type.dividend, info.dividend_val) &&
            sign_test (type.capgains, info.capgains_val))
            return {type.purchase, type.sale, type.narrative};
    }
    return {};
}

bool
split_action_is (Split* split, const std::string& action)
{
    const char* value = xaccSplitGetAction (split);
    return value != nullptr && action == value;
}

/* txn->info(): the transaction's amounts, one account at a time.  The
 * capitalised-fee test has to come before the stock-amount test because a
 * capitalised fee is a split *in* the stock account. */
TxnInfo
transaction_info (Transaction* txn, const IfrsOptions& options)
{
    TxnInfo info;
    for (GList* node = xaccTransGetSplitList (txn); node != nullptr;
         node = node->next)
    {
        auto* split = static_cast<Split*> (node->data);
        Account* account = xaccSplitGetAccount (split);
        if (account == options.proceeds_account)
            add_optional (info.proceeds_val, xaccSplitGetAmount (split));
        else if (account == options.capgains_account)
            add_optional (info.capgains_val, xaccSplitGetAmount (split));
        else if (account == options.fees_account)
            add_optional (info.fees_exp_val, xaccSplitGetAmount (split));
        else if (account == options.dividend_account)
            add_optional (info.dividend_val, xaccSplitGetAmount (split));
        else if (account == options.stock_account &&
                 split_action_is (split, options.cap_fee_action))
            add_optional (info.fees_cap_val, xaccSplitGetValue (split));
        else if (account == options.stock_account)
        {
            add_optional (info.stock_amt, xaccSplitGetAmount (split));
            add_optional (info.stock_val, xaccSplitGetValue (split));
        }
    }
    return info;
}

/* The split list a query over one account and its unique transactions gives:
 * one split per transaction, in date order. */
std::vector<Split*>
account_splits_date_order (Account* account)
{
    std::vector<Split*> splits;
    for (GList* node = xaccAccountGetSplitList (account); node != nullptr;
         node = node->next)
    {
        auto* split = static_cast<Split*> (node->data);
        if (xaccTransGetVoidStatus (xaccSplitGetParent (split)))
            continue;
        splits.push_back (split);
    }
    std::stable_sort (splits.begin (), splits.end (),
                      [] (Split* left, Split* right)
                      {
                          Transaction* left_txn = xaccSplitGetParent (left);
                          Transaction* right_txn = xaccSplitGetParent (right);
                          const time64 left_date = xaccTransGetDate (left_txn);
                          const time64 right_date = xaccTransGetDate (right_txn);
                          if (left_date != right_date)
                              return left_date < right_date;
                          return xaccTransOrder (left_txn, right_txn) < 0;
                      });

    /* Unique transactions: keep the first split of each. */
    std::vector<Split*> unique;
    std::set<Transaction*> seen;
    for (Split* split : splits)
        if (seen.insert (xaccSplitGetParent (split)).second)
            unique.push_back (split);
    return unique;
}

/* An amount in the report currency, or a blank cell when the report is not
 * formatting its cells and there is no amount. */
Cell
amount_cell (const std::optional<gnc_numeric>& amount, gnc_commodity* currency,
             bool format)
{
    if (!amount.has_value ())
        return blank_cell ();
    if (!format)
        return {format_amount (*amount, nullptr), "number-cell"};
    return {format_amount (*amount, currency), "number-cell"};
}

} // namespace

IfrsReport
ifrs_cost_basis (Account* root, const IfrsOptions& options)
{
    IfrsReport report;
    report.title = "IFRS weighted-average cost basis report";

    if (options.stock_account == nullptr)
    {
        report.warning = "no-stock-account";
        return report;
    }

    gnc_commodity* commodity = xaccAccountGetCommodity (options.stock_account);
    gnc_commodity* currency =
        gnc_account_get_currency_or_parent (options.stock_account);
    if (currency == nullptr)
        currency = commodity;
    gnc_commodity* report_cur = report_currency (root, options.currency);
    if (report_cur == nullptr)
        report_cur = currency;

    GNCPriceDB* db =
        gnc_pricedb_get_db (qof_instance_get_book (QOF_INSTANCE (root)));

    /* The document title the Scheme report sets. */
    {
        char from[32] = {0};
        char to[32] = {0};
        qof_print_date_buff (from, sizeof from, options.start);
        qof_print_date_buff (to, sizeof to, options.end);
        report.title = "Average-Cost (Basis) Report: From ";
        report.title += from;
        report.title += " to ";
        report.title += to;
        report.title += ". Report-currency ";
        report.title += gnc_commodity_get_mnemonic (report_cur);
    }

    CellTable& table = report.table;
    table.headings = {
        "date", "description", "trans-units", "cumul-units", "note", "curr",
        "fx", "purchase-val", "purchase-cost", "cash-dividends",
        "proceeds-val", "proceeds-cost", "conv-purchase-val",
        "conv-purchase-cost", "conv-dividends", "conv-proceeds-val",
        "conv-proceeds-cost", "cumulative-average-cost-basis",
        "average-cost-basis/unit-for-sale", "average-cost-basis-of-sale",
        "net-proceeds", "gain-post-commission", "gain-pre-commission (calc)",
        "gain-pre-commission (register)", "gain-pre-commission variance",
        "cumul-gross-profit", "cumul-net-profit", "cumul-tot-return"};

    const int report_frac = gnc_commodity_get_fraction (report_cur);

    auto convert = [&] (gnc_numeric amount, time64 when)
    {
        if (currency == nullptr || report_cur == nullptr ||
            gnc_commodity_equiv (currency, report_cur))
            return amount;
        return gnc_pricedb_convert_balance_nearest_price_t64 (
            db, amount, currency, report_cur, time64CanonicalDayTime (when));
    };

    gnc_numeric cumulative_units = gnc_numeric_zero ();
    gnc_numeric cumulative_basis = gnc_numeric_zero ();
    gnc_numeric cumulative_gross = gnc_numeric_zero ();
    gnc_numeric cumulative_net = gnc_numeric_zero ();
    gnc_numeric cumulative_return = gnc_numeric_zero ();

    for (Split* split : account_splits_date_order (options.stock_account))
    {
        Transaction* txn = xaccSplitGetParent (split);
        const time64 date = xaccTransGetDate (txn);

        /* The identification and the running totals are computed for every
         * transaction; only the rows are filtered. */
        const TxnInfo info = transaction_info (txn, options);
        const std::optional<gnc_numeric> trans_units = info.stock_amt;
        const std::optional<gnc_numeric> cash_value = info.proceeds_val;
        const std::optional<gnc_numeric> dividends = info.dividend_val;
        const std::optional<gnc_numeric> capgains = info.capgains_val;
        const std::optional<gnc_numeric> fees_expense = info.fees_exp_val;

        const std::optional<gnc_numeric> fees_value =
            (info.fees_cap_val.has_value () || fees_expense.has_value ())
                ? std::optional<gnc_numeric> (gnc_numeric_add_fixed (
                      info.fees_cap_val.value_or (gnc_numeric_zero ()),
                      fees_expense.value_or (gnc_numeric_zero ())))
                : std::nullopt;
        const std::optional<gnc_numeric> trans_value =
            (info.stock_val.has_value () || info.fees_cap_val.has_value ())
                ? std::optional<gnc_numeric> (gnc_numeric_add_fixed (
                      info.stock_val.value_or (gnc_numeric_zero ()),
                      info.fees_cap_val.value_or (gnc_numeric_zero ())))
                : std::nullopt;

        const gnc_numeric new_units =
            gnc_numeric_add_fixed (cumulative_units,
                                   trans_units.value_or (gnc_numeric_zero ()));

        Identification identified;
        const char* note = nullptr;
        if (gnc_numeric_negative_p (new_units) &&
            gnc_numeric_positive_p (cumulative_units))
            note = "ERROR: long→short";
        else if (gnc_numeric_negative_p (cumulative_units) &&
                 gnc_numeric_positive_p (new_units))
            note = "ERROR: short→long";
        else
        {
            identified = identify_transaction (info, cumulative_units);
            note = identified.narrative;
        }

        const bool sale = identified.sale;
        const bool purchase = identified.purchase;

        const std::optional<gnc_numeric> purchase_cost =
            purchase ? fees_value : std::nullopt;
        std::optional<gnc_numeric> purchase_val;
        if (purchase && trans_value.has_value ())
            purchase_val = gnc_numeric_sub_fixed (
                *trans_value, purchase_cost.value_or (gnc_numeric_zero ()));
        std::optional<gnc_numeric> cash_dividends;
        if (dividends.has_value ())
            cash_dividends = gnc_numeric_neg (*dividends);
        const std::optional<gnc_numeric> proceeds_cost =
            sale ? fees_value : std::nullopt;
        std::optional<gnc_numeric> proceeds_value;
        if (sale && cash_value.has_value ())
            proceeds_value = gnc_numeric_add_fixed (
                *cash_value, proceeds_cost.value_or (gnc_numeric_zero ()));

        const gnc_numeric fx = convert (gnc_numeric_create (1, 1), date);

        std::optional<gnc_numeric> conv_purchase_val;
        if (purchase_val.has_value ())
            conv_purchase_val = gnc_numeric_mul (fx, *purchase_val, report_frac,
                                                 GNC_HOW_RND_ROUND);
        std::optional<gnc_numeric> conv_purchase_cost;
        if (purchase_cost.has_value ())
            conv_purchase_cost = gnc_numeric_mul (fx, *purchase_cost,
                                                  report_frac,
                                                  GNC_HOW_RND_ROUND);
        std::optional<gnc_numeric> conv_dividends;
        if (cash_dividends.has_value ())
            conv_dividends = gnc_numeric_mul (fx, *cash_dividends, report_frac,
                                              GNC_HOW_RND_ROUND);
        std::optional<gnc_numeric> conv_proceeds_value;
        if (proceeds_value.has_value ())
            conv_proceeds_value = gnc_numeric_mul (fx, *proceeds_value,
                                                   report_frac,
                                                   GNC_HOW_RND_ROUND);
        std::optional<gnc_numeric> conv_proceeds_cost;
        if (proceeds_cost.has_value ())
            conv_proceeds_cost = gnc_numeric_mul (fx, *proceeds_cost,
                                                  report_frac,
                                                  GNC_HOW_RND_ROUND);

        /* The average cost basis: a sale's basis is the running average times
         * the units sold, and the running basis loses it. */
        std::optional<gnc_numeric> basis_per_unit;
        if (!gnc_numeric_zero_p (cumulative_units))
        {
            const gnc_numeric average = gnc_numeric_div (
                cumulative_basis, cumulative_units, GNC_DENOM_AUTO,
                GNC_HOW_DENOM_REDUCE);
            basis_per_unit = gnc_numeric_abs (average);
        }
        std::optional<gnc_numeric> basis_of_sale;
        if (proceeds_value.has_value () && basis_per_unit.has_value () &&
            trans_units.has_value ())
            basis_of_sale = gnc_numeric_mul (*basis_per_unit, *trans_units,
                                             GNC_DENOM_AUTO,
                                             GNC_HOW_DENOM_REDUCE);

        gnc_numeric new_basis = cumulative_basis;
        if (conv_purchase_val.has_value ())
            new_basis = gnc_numeric_add (new_basis, *conv_purchase_val, GNC_DENOM_AUTO,
                                       GNC_HOW_DENOM_REDUCE);
        if (options.capitalize_purchase_costs && conv_purchase_cost.has_value ())
            new_basis = gnc_numeric_add (new_basis, *conv_purchase_cost, GNC_DENOM_AUTO,
                                       GNC_HOW_DENOM_REDUCE);
        if (basis_of_sale.has_value ())
            new_basis = gnc_numeric_add (new_basis, *basis_of_sale, GNC_DENOM_AUTO,
                                       GNC_HOW_DENOM_REDUCE);

        std::optional<gnc_numeric> net_proceeds;
        if (conv_proceeds_value.has_value ())
            net_proceeds = gnc_numeric_sub_fixed (
                *conv_proceeds_value,
                conv_proceeds_cost.value_or (gnc_numeric_zero ()));

        std::optional<gnc_numeric> gain_post_commission;
        if (net_proceeds.has_value () || basis_of_sale.has_value () ||
            (!options.capitalize_purchase_costs &&
             conv_purchase_cost.has_value ()))
        {
            gnc_numeric amount =
                net_proceeds.value_or (gnc_numeric_zero ());
            amount = gnc_numeric_add_fixed (
                amount, basis_of_sale.value_or (gnc_numeric_zero ()));
            if (!options.capitalize_purchase_costs)
                amount = gnc_numeric_add_fixed (
                    amount, conv_purchase_cost.value_or (gnc_numeric_zero ()));
            gain_post_commission = amount;
        }

        std::optional<gnc_numeric> gain_pre_commission;
        if (conv_proceeds_value.has_value () || basis_of_sale.has_value ())
            gain_pre_commission = gnc_numeric_add_fixed (
                conv_proceeds_value.value_or (gnc_numeric_zero ()),
                basis_of_sale.value_or (gnc_numeric_zero ()));

        std::optional<gnc_numeric> gain_variance;
        if (gain_pre_commission.has_value () || capgains.has_value ())
            gain_variance = gnc_numeric_add_fixed (
                gain_pre_commission.value_or (gnc_numeric_zero ()),
                capgains.value_or (gnc_numeric_zero ()));

        const gnc_numeric new_gross = gnc_numeric_add_fixed (
            cumulative_gross,
            gain_pre_commission.value_or (gnc_numeric_zero ()));
        const gnc_numeric new_net = gnc_numeric_add_fixed (
            cumulative_net,
            gain_post_commission.value_or (gnc_numeric_zero ()));
        const gnc_numeric new_return = gnc_numeric_add_fixed (
            gnc_numeric_add_fixed (
                cumulative_return,
                gain_post_commission.value_or (gnc_numeric_zero ())),
            conv_dividends.value_or (gnc_numeric_zero ()));

        const bool in_window = options.start < date && date < options.end;

        cumulative_units = new_units;
        cumulative_basis = new_basis;
        cumulative_gross = new_gross;
        cumulative_net = new_net;
        cumulative_return = new_return;

        if (!in_window)
            continue;

        std::vector<Cell> row;
        row.push_back ({printed_date (date), "date-cell"});
        row.push_back ({xaccTransGetDescription (txn), "text-cell"});
        row.push_back (trans_units.has_value ()
                           ? Cell {format_amount (*trans_units, commodity),
                                   "number-cell"}
                           : blank_cell ());
        row.push_back ({format_amount (new_units, commodity), "number-cell"});
        row.push_back ({note != nullptr ? note : "", "text-cell"});
        row.push_back ({gnc_commodity_get_mnemonic (currency), "text-cell"});
        row.push_back ({format_amount (fx, report_cur), "number-cell"});
        row.push_back (amount_cell (purchase_val, currency, options.format_cells));
        row.push_back (
            amount_cell (purchase_cost, currency, options.format_cells));
        row.push_back (
            amount_cell (cash_dividends, currency, options.format_cells));
        row.push_back (
            amount_cell (proceeds_value, currency, options.format_cells));
        row.push_back (
            amount_cell (proceeds_cost, currency, options.format_cells));
        row.push_back (
            amount_cell (conv_purchase_val, report_cur, options.format_cells));
        row.push_back (
            amount_cell (conv_purchase_cost, report_cur, options.format_cells));
        row.push_back (
            amount_cell (conv_dividends, report_cur, options.format_cells));
        row.push_back (amount_cell (conv_proceeds_value, report_cur,
                                    options.format_cells));
        row.push_back (amount_cell (conv_proceeds_cost, report_cur,
                                    options.format_cells));
        row.push_back (
            amount_cell (new_basis, report_cur, options.format_cells));
        row.push_back (
            amount_cell (basis_per_unit, report_cur, options.format_cells));
        row.push_back (amount_cell (
            basis_of_sale.has_value ()
                ? std::optional<gnc_numeric> (gnc_numeric_neg (*basis_of_sale))
                : std::nullopt,
            report_cur, options.format_cells));
        row.push_back (
            amount_cell (net_proceeds, report_cur, options.format_cells));
        row.push_back (amount_cell (gain_post_commission, report_cur,
                                    options.format_cells));
        row.push_back (amount_cell (gain_pre_commission, report_cur,
                                    options.format_cells));
        row.push_back (
            amount_cell (capgains, report_cur, options.format_cells));
        row.push_back (
            amount_cell (gain_variance, report_cur, options.format_cells));
        row.push_back (
            amount_cell (new_gross, report_cur, options.format_cells));
        row.push_back (
            amount_cell (new_net, report_cur, options.format_cells));
        row.push_back (
            amount_cell (new_return, report_cur, options.format_cells));
        table.rows.push_back (std::move (row));
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

    const bool grouped = !table.column_headings.empty ();
    if (grouped)
    {
        /* A grouped column axis: the row-axis heading, then one heading per
         * amount column.  The Scheme multicolumn reports print the row axis
         * heading ("Date"/"Period") in the same row, in "total-label-cell"
         * cells; use the same class so a reader of the document sees the
         * heading row as the report's own row and not as the renderer's
         * column legend. */
        out << "<th class='total-label-cell'>" << html_escape (table.row_heading)
            << "</th>";
        for (const std::string& heading : table.column_headings)
            out << "<th class='total-label-cell'>" << html_escape (heading)
                << "</th>";
    }
    else
    {
        for (const Column column : table.columns)
            out << "<th>" << column_heading (column) << "</th>";
    }
    out << "</tr></thead><tbody>\n";

    for (const Row& row : table.rows)
    {
        const bool is_account = row.type == RowType::Account;
        out << "<tr>";
        if (grouped)
        {
            /* The label column, then one amount per column heading.  A
             * section heading carries no amounts at all, so its cells are
             * blank. */
            if (is_account)
            {
                out << "<td class='text-cell'>";
                if (!row.guid.empty ())
                    out << "<a href='gnc-register:acct-guid=" << row.guid
                        << "#'>";
                out << html_escape (row.label);
                if (!row.guid.empty ())
                    out << "</a>";
                out << "</td>";
            }
            else
                out << "<td class='total-label-cell'>"
                    << html_escape (row.label) << "</td>";

            for (size_t index = 0; index < table.column_headings.size ();
                 ++index)
            {
                const gnc_numeric value =
                    index < row.values.size () ? row.values[index]
                                               : gnc_numeric_zero ();
                if (row.blank)
                    out << "<td class='total-number-cell'> </td>";
                else if (is_account)
                    out << "<td class='number-cell'>"
                        << html_escape (format_amount (value, table.currency))
                        << "</td>";
                else
                    out << "<td class='total-number-cell'>"
                        << html_escape (format_amount (value, table.currency))
                        << "</td>";
            }
            out << "</tr>\n";
            continue;
        }
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
render_warning (const std::string& title, const std::string& heading,
                const std::string& text)
{
    /* gnc:html-make-generic-warning(): the report's name and a colon, the
     * warning's own heading (which may be empty) and the explanation. */
    std::ostringstream out;
    out << "<!DOCTYPE html>\n<html><head><meta charset='utf-8'><title>"
        << html_escape (title) << "</title><style>" << report_stylesheet ()
        << "</style></head><body><h3>" << html_escape (title) << ":</h3>\n";
    if (!heading.empty ())
        out << "<h3>" << html_escape (heading) << "</h3>\n";
    out << "<p>" << html_escape (text) << "</p>\n</body></html>\n";
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
     * report repeats a heading row per account), so the header is optional.
     * A report whose columns are a tree supplies several header rows. */
    if (!table.header_rows.empty ())
    {
        out << "<thead>";
        for (const std::vector<Cell>& row : table.header_rows)
        {
            out << "<tr>";
            for (const Cell& cell : row)
            {
                out << "<th";
                if (cell.span > 1)
                    out << " colspan='" << cell.span << "'";
                out << ">";
                out << (cell.text.empty () ? " " : html_escape (cell.text));
                out << "</th>";
            }
            out << "</tr>";
        }
        out << "</thead>";
    }
    else if (!table.headings.empty ())
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
            "Profit & Loss", "Balance Sheet (Multicolumn)",
            "Income Statement (Multicolumn)", "Equity Statement",
            "Transaction Report", "General Ledger",
            "General Journal", "Transaction Breakdown Report",
            "Reconciliation Report",
            "Receivable Aging", "Payable Aging", "Investment Lots",
            "Investment Portfolio", "Advanced Portfolio", "Lot Viewer",
            "IFRS weighted-average cost basis report",
            "Budget Report", "Budget Income Statement",
            "Budget Profit & Loss", "Budget Balance Sheet", "Budget Flow",
            "Printable Invoice", "Receipt", "Tax Invoice",
            "Australian Tax Invoice", "Tax Schedule Report/TXF Export",
            "Customer Report", "Vendor Report", "Employee Report",
            "Job Report", "Customer Summary"};
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

TaxScheduleOptions
tax_options (const ReportRequest& request)
{
    TaxScheduleOptions options;
    if (!request.all_dates)
    {
        options.start = request.start;
        options.end = request.end;
    }
    options.accounts = request.accounts;
    options.suppress_zero = request.tax_suppress_zero;
    options.alternate_period = request.tax_alternate_period;
    options.currency = request.currency;
    return options;
}

} // namespace

bool
ReportRequest::boolean_option (const std::string& name, bool fallback) const
{
    const auto found = booleans.find (name);
    return found != booleans.end () ? found->second : fallback;
}

std::string
ReportRequest::text_option (const std::string& name,
                            const std::string& fallback) const
{
    const auto found = texts.find (name);
    return found != texts.end () ? found->second : fallback;
}

std::string
ReportRequest::choice_option (const std::string& name,
                              const std::string& fallback) const
{
    const auto found = choices.find (name);
    return found != choices.end () ? found->second : fallback;
}

namespace
{

/* The Scheme reports' own default price sources: the Investment Portfolio
 * values at the most recent price and the Advanced Portfolio at the price
 * nearest its report date. */
const char*
default_price_source (bool advanced)
{
    return advanced ? "pricedb-nearest" : "pricedb-latest";
}

PortfolioOptions
portfolio_options (const ReportRequest& request, bool advanced)
{
    PortfolioOptions options;
    options.date = request.end.value_or (default_end_date ());
    const std::string source =
        request.choice_option ("price-source", default_price_source (advanced));
    if (source == "pricedb-before")
        options.price_source = PriceSource::Before;
    else if (source == "pricedb-nearest")
        options.price_source = PriceSource::Nearest;
    options.include_zero = request.boolean_option ("include-zero", false);
    options.accounts = request.accounts;
    options.basis_method =
        request.choice_option ("basis-method", "average-basis");
    options.brokerage_fees =
        request.choice_option ("brokerage-fees", "include-in-basis");
    options.show_symbol = request.boolean_option ("show-symbol", true);
    options.show_listing = request.boolean_option ("show-listing", true);
    options.show_shares = request.boolean_option ("show-shares", true);
    options.show_price = request.boolean_option ("show-price", true);
    return options;
}

/* A boolean switch descriptor, with the Scheme report's default. */
OptionDescriptor
switch_option (const char* name, const char* label, const char* description,
               bool default_value)
{
    OptionDescriptor option;
    option.name = name;
    option.type = OptionType::Bool;
    option.label = label;
    option.description = description;
    option.default_value = default_value ? "true" : "false";
    return option;
}

OptionDescriptor
choice_option_descriptor (const char* name, const char* label,
                          const char* description, const char* default_value,
                          std::vector<std::string> choices)
{
    OptionDescriptor option;
    option.name = name;
    option.type = OptionType::Choice;
    option.label = label;
    option.description = description;
    option.default_value = default_value;
    option.choices = std::move (choices);
    return option;
}

OptionDescriptor
account_selection_option (const char* name, const char* label,
                          const char* description)
{
    OptionDescriptor option;
    option.name = name;
    option.type = OptionType::AccountList;
    option.label = label;
    option.description = description;
    return option;
}

} // namespace

std::vector<OptionDescriptor>
report_options (const std::string& name)
{
    std::vector<OptionDescriptor> options;

    OwnerKind owner_kind;
    if (owner_report_kind (name, owner_kind))
    {
        /* new-owner-report.scm: one option page per owner type, the same
         * three date/display pages otherwise.  The owner option serialises as
         * the owner's GUID, which is what the route takes; the linked-
         * transactions and document-link columns and the "Due or Post Date"
         * choice are not reproduced. */
        OptionDescriptor owner;
        owner.name = "owner";
        owner.type = OptionType::Owner;
        owner.label = owner_kind_name (owner_kind);
        owner.description =
            "The " + std::string (owner_kind_name (owner_kind)) +
            " whose documents and payments the report covers, by GUID.  "
            "Without it the report renders the Scheme \"No valid owner "
            "selected\" warning.";
        options.push_back (owner);

        options.push_back (date_option (
            "start", "From",
            "The first day of the period whose rows are printed.  "
            "Transactions before it are the balance brought forward.",
            default_start_date ()));
        options.push_back (date_option (
            "end", "To",
            "The last day of the period whose rows are printed.  It is also "
            "the date the aging buckets are measured from.  It defaults to "
            "today, as the Scheme report's does.",
            default_aging_date ()));

        struct DisplayOption
        {
            const char* name;
            const char* label;
            const char* description;
            bool default_value;
        };
        static const DisplayOption displays[] = {
            {"show_date", "Date", "Display the transaction date?", true},
            {"show_due_date", "Due Date", "Display the invoice due date?", true},
            {"show_reference", "Reference",
             "Display the transaction reference?", true},
            {"show_type", "Type", "Display the transaction type?", true},
            {"show_description", "Description",
             "Display the transaction description?", true},
            {"show_job", "Job", "Display the associated job?", false},
            {"show_sale", "Sale", "Display the sale amount column?", false},
            {"show_tax", "Tax", "Display the tax column?", false},
            {"show_debits", "Debits", "Display the period debits column?",
             true},
            {"show_credits", "Credits", "Display the period credits column?",
             true},
            {"show_balance", "Balance", "Display a running balance?", true},
        };
        for (const DisplayOption& display : displays)
        {
            /* The Job Report has no job column option at all, as the Scheme
             * options-generator omits it for GNC-OWNER-JOB. */
            if (owner_kind == OwnerKind::Job &&
                std::string (display.name) == "show_job")
                continue;
            OptionDescriptor option;
            option.name = display.name;
            option.type = OptionType::Bool;
            option.label = display.label;
            option.description = display.description;
            option.default_value = display.default_value ? "true" : "false";
            options.push_back (option);
        }
        options.push_back (period_option ());
        return options;
    }

    if (name == "Customer Summary")
    {
        /* customer-summary.scm's General page and the display switches that
         * change the rows.  The income/expense account selections default to
         * every such account and the sort key/order to customer name
         * ascending, which is what the native report renders; those options
         * are not exposed because the route's single "accounts" name cannot
         * stand for two selections. */
        options.push_back (date_option (
            "start", "From",
            "The first day of the period the summary covers.",
            default_start_date ()));
        options.push_back (date_option (
            "end", "To", "The last day of the period the summary covers.",
            default_end_date ()));

        struct DisplayOption
        {
            const char* name;
            const char* label;
            const char* description;
            bool default_value;
        };
        static const DisplayOption displays[] = {
            {"show_expense", "Show Expense Column",
             "Show the column with the expenses per customer.", true},
            {"show_zero_lines", "Show Lines with All Zeros",
             "Show customer lines with no activity in the period.", false},
            {"show_inactive", "Show Inactive Customers",
             "Include customers that have been marked inactive.", false},
        };
        for (const DisplayOption& display : displays)
        {
            OptionDescriptor option;
            option.name = display.name;
            option.type = OptionType::Bool;
            option.label = display.label;
            option.description = display.description;
            option.default_value = display.default_value ? "true" : "false";
            options.push_back (option);
        }
        options.push_back (period_option ());
        return options;
    }

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

    if (name == "Tax Schedule Report/TXF Export")
    {
        /* The Scheme report's General page (From/To and the "Alternate
         * Period" multi-choice), its account selection and the two display
         * switches that change the rows.  The transaction-detail switches
         * are not exposed because the native report does not print detail. */
        options.push_back (date_option (
            "start", "From",
            "The first day of the period the schedule covers.  Its year is "
            "the tax year whose form line numbers are printed.",
            default_start_date ()));
        options.push_back (date_option (
            "end", "To",
            "The last day of the period the schedule covers.",
            default_end_date ()));

        OptionDescriptor alternate;
        alternate.name = "alternate-period";
        alternate.type = OptionType::Choice;
        alternate.label = "Alternate Period";
        alternate.description =
            "Override the From/To dates with one of the Scheme report's "
            "estimated-tax periods.  \"from-to\" uses the dates as given.";
        alternate.default_value = "from-to";
        alternate.choices = {"from-to", "1st-est", "2nd-est", "3rd-est",
                             "4th-est", "last-year", "1st-last", "2nd-last",
                             "3rd-last", "4th-last"};
        options.push_back (alternate);

        OptionDescriptor accounts = accounts_option ();
        accounts.description =
            "Accounts to report on, as a comma-separated list of account "
            "GUIDs; each GUID selects that account's subtree.  \"all\" selects "
            "every account in the book.  Without the option the report's own "
            "default applies: every tax-related account in the book.  "
            "Accounts that are not tax-related are ignored.";
        options.push_back (accounts);

        OptionDescriptor suppress;
        suppress.name = "suppress-zero";
        suppress.type = OptionType::Bool;
        suppress.label = "Suppress $0.00 values";
        suppress.description =
            "Do not print zero-valued tax code and account total rows.";
        suppress.default_value = "false";
        options.push_back (suppress);

        OptionDescriptor format;
        format.name = "format";
        format.type = OptionType::Choice;
        format.label = "Output";
        format.description =
            "Render the schedule table, or the TXF export the Scheme report "
            "writes for TurboTax.";
        format.default_value = "report";
        format.choices = {"report", "txf"};
        options.push_back (format);
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
    else if (name == "Balance Sheet (Multicolumn)" ||
             name == "Income Statement (Multicolumn)")
    {
        /* balsheet-pnl.scm's option page.  The two reports share it; the
         * profit & loss adds "include overall period".  Published are the
         * options the native table honours; the account selection (the Scheme
         * default is every account in the book, which is what the native
         * report covers), the dual-column layout (a presentation of the same
         * rows), the amount indenting and omit-zero-balances switches, the
         * non-recursive parent-balance mode, the "Common Currency" page, the
         * chart and the links are not, and the descriptors below say so where
         * a caller could otherwise expect them. */
        options.push_back (date_option (
            "start", "Start Date",
            "The first day of the report's period.  With a column period the "
            "first column is this date's period end; without one the balance "
            "sheet ignores the start date, as the Scheme report does, while "
            "the profit & loss's single column is the whole range.",
            default_start_date ()));
        options.push_back (date_option (
            "end", "End Date",
            "The last day of the report's period, and the date the last "
            "column's balance is drawn at.",
            default_end_date ()));

        OptionDescriptor duration;
        duration.name = "period-duration";
        duration.type = OptionType::Choice;
        duration.label = "Period duration";
        duration.description =
            "The duration between time periods.  \"disabled\" (the default) "
            "is the single-column form; every other choice adds one column "
            "per period.";
        duration.default_value = "disabled";
        duration.choices = {"disabled", "year", "half-year", "quarter",
                            "month", "two-weeks", "week"};
        options.push_back (duration);

        struct Switch
        {
            const char* name;
            const char* label;
            const char* description;
            bool default_value;
        };
        static const Switch switches[] = {
            {"reverse-chrono", "Period order is most recent first",
             "Print the amount columns most recent first, as the Scheme "
             "report's default does; off prints them oldest first.",
             true},
            {"show-zb-accts", "Include accounts with zero total balances",
             "Include accounts whose every column is zero; off drops them.",
             true},
            {"account-full-name", "Account full name instead of indenting",
             "Print the account's full name instead of its own name; the "
             "native table does not indent, so the default prints the names "
             "the Scheme report indents.", false},
            {"label-sections", "Label sections",
             "Print the section titles (Asset, Liability, Equity, ...).", true},
            {"total-sections", "Include totals",
             "Print each section's \"Total For\" row.", true},
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

        if (name == "Income Statement (Multicolumn)")
        {
            OptionDescriptor overall;
            overall.name = "include-overall-period";
            overall.type = OptionType::Bool;
            overall.label =
                "If more than 1 period column, include overall period?";
            overall.description =
                "Add a column covering the whole range after the period "
                "columns.  It has no effect on the single-column form.";
            overall.default_value = "false";
            options.push_back (overall);
        }
        return options;
    }
    else if (name == "Equity Statement")
    {
        /* equity-statement.scm's options.  The date range and the report
         * title are published; the currency page, the accounting-style rules
         * and the closing-entry pattern are not, because the native report
         * converts into the report currency, prints no rules and matches the
         * Scheme defaults' description pattern. */
        options.push_back (date_option (
            "start", "Start Date",
            "The first day of the period the statement covers.  The opening "
            "capital is drawn at the end of the day before it.",
            default_start_date ()));
        options.push_back (date_option (
            "end", "End Date",
            "The last day of the period the statement covers.",
            default_end_date ()));

        OptionDescriptor title;
        title.name = "report-title";
        title.type = OptionType::Text;
        title.label = "Report Title";
        title.description =
            "The document's title.  The Scheme option defaults to the report "
            "name and so does this.";
        title.default_value = "Equity Statement";
        options.push_back (title);
        /* No accounting-period preset: the statement runs from an opening
         * balance date to a closing one, so "all dates" has no meaning. */
        return options;
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
    else if (name == "General Journal")
    {
        /* general-journal.scm inherits the register report's options and then
         * sets its own defaults on them: journal layout, two amount columns,
         * no number column, no running balance.  Only the switches that
         * change this listing are published; the shares/price/value columns
         * and the amount single/two-column choice are not, because the
         * native listing has one fixed pair of amount columns.  The Scheme
         * "Totals" option is also not published: through the saved-report
         * path it does not change the page (the Debit/Credit totals always
         * print), so the effective default is what renders. */
        struct Switch
        {
            const char* name;
            const char* label;
            const char* description;
            bool default_value;
        };
        static const Switch switches[] = {
            {"show_date", "Date", "Display the transaction date?", true},
            {"show_num", "Num", "Display the transaction number?", false},
            {"show_description", "Description",
             "Display the transaction description on the heading line?", true},
            {"show_memo", "Memo",
             "Display the split memo on each split line?", true},
            {"show_account", "Account", "Display the split's account?", true},
            {"show_running_balance", "Running Balance",
             "Display a Balance column?  The register prints the transaction's "
             "net on its heading line, not a cumulative balance.", false},
        };
        for (const Switch& item : switches)
            options.push_back (switch_option (item.name, item.label,
                                              item.description,
                                              item.default_value));
        return options;
    }
    else if (name == "Transaction Breakdown Report")
    {
        /* txn-columns.scm: one account, a date range, a description filter
         * and the column limit.  The Scheme account option is a single
         * account (GncOptionAccountValue); the route's account selection is a
         * list, so this publishes the same name as an account list and the
         * report takes its first entry, which is what the harness sends. */
        OptionDescriptor account;
        account.name = "account";
        account.type = OptionType::AccountList;
        account.label = "Account";
        account.description =
            "The account whose transactions are broken down, by GUID.  The "
            "Scheme option takes a single account; this accepts the same "
            "list form as the other reports and uses the first account.  "
            "Without it the report renders the Scheme \"No data\" warning.";
        options.push_back (account);

        options.push_back (date_option (
            "start", "Start Date",
            "The first day of the period whose transactions are printed.",
            default_start_date ()));
        options.push_back (date_option (
            "end", "End Date",
            "The last day of the period whose transactions are printed.",
            default_end_date ()));

        OptionDescriptor filter;
        filter.name = "description-filter";
        filter.type = OptionType::Text;
        filter.label = "Description Filter";
        filter.description =
            "Print only transactions whose description contains this text; "
            "empty prints every transaction in the period.";
        filter.default_value = "";
        options.push_back (filter);

        OptionDescriptor limit;
        limit.name = "max-columns";
        limit.type = OptionType::Number;
        limit.label = "Limit for number of columns";
        limit.description =
            "The largest number of account columns the report will render; "
            "above it the report prints its \"Too many accounts\" warning, "
            "as the Scheme report does.";
        limit.default_value = "10";
        limit.minimum = 1;
        limit.maximum = 100;
        options.push_back (limit);
        return options;
    }
    else if (name == "Reconciliation Report")
    {
        /* reconcile-report.scm is trep-engine with different defaults: the
         * primary sort key is the reconciled status, the date filter is the
         * reconciled date (an unreconciled split is always included), and the
         * Funds In/Funds Out calculated columns replace the debit/credit
         * pair.  The account list and the two dates are the option surface
         * this exposes; the display switches the Scheme report flips
         * (Reconciled Date on, Running Balance and Memo off) are its
         * defaults, and the remaining trep options stay at theirs. */
        options.push_back (accounts_option ());
        options.push_back (date_option (
            "start", "Start Date",
            "The first day of the period a reconciled split's reconciled date "
            "must fall in.  An unreconciled split is always included, "
            "whatever this date is, which is why the option is accepted but "
            "has no effect on a book whose splits are all unreconciled -- the "
            "two fixture books in this tree are that case.",
            default_start_date ()));
        options.push_back (date_option (
            "end", "End Date",
            "The last day of the period a reconciled split's reconciled date "
            "must fall in.  An unreconciled split is always included, "
            "whatever this date is; the descriptor for start says what that "
            "means for the fixtures.",
            default_end_date ()));
        options.push_back (switch_option (
            "show_reconciled_date", "Reconciled Date",
            "Display the date a reconciled split was reconciled?", true));
        return options;
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
    else if (name == "Investment Portfolio" || name == "Advanced Portfolio")
    {
        const bool advanced = name == "Advanced Portfolio";
        if (advanced)
        {
            /* advanced-portfolio.scm has one date option ("Date"); the other
             * end is not a period bound but the date the holding is valued
             * at, so there is no start date and no period preset. */
            options.push_back (date_option (
                "end", "Date",
                "The date the portfolio is valued at: the units are the "
                "account's balance at the end of it and the price is the "
                "price source's price at it.",
                default_end_date ()));
        }
        else
        {
            options.push_back (date_option (
                "start", "Start Date",
                "Accepted so a date range can be handed to any report; the "
                "portfolio is valued as of its end date and does not use a "
                "start date.",
                default_start_date ()));
            options.push_back (date_option (
                "end", "Date",
                "The date the portfolio is valued at: the units are the "
                "account's balance at the end of it and the price is the "
                "price source's price at it.",
                default_end_date ()));
        }
        options.push_back (currency_option ());
        options.push_back (choice_option_descriptor (
            "price-source", "Price Source",
            "The source of price information.  \"pricedb-latest\" is the most "
            "recent price in the database, \"pricedb-nearest\" the price "
            "nearest to the report date and \"pricedb-before\" the nearest one "
            "before it.",
            advanced ? "pricedb-nearest" : "pricedb-latest",
            {"pricedb-latest", "pricedb-before", "pricedb-nearest"}));
        options.push_back (account_selection_option (
            "accounts", "Accounts",
            "Stock accounts to report on, as a comma-separated list of account "
            "GUIDs; each GUID selects that account's subtree.  \"all\" selects "
            "every account in the book.  Without the option the report's "
            "default selection applies: every stock and mutual fund account in "
            "the book."));
        options.push_back (switch_option (
            "include-zero", "Include accounts with no shares",
            "Include accounts that have a zero share balance.", false));
        if (advanced)
        {
            options.push_back (choice_option_descriptor (
                "basis-method", "Basis calculation method",
                "How the cost basis is accumulated when a holding is sold.",
                "average-basis",
                {"average-basis", "fifo-basis", "filo-basis"}));
            options.push_back (choice_option_descriptor (
                "brokerage-fees", "How to report brokerage fees",
                "Whether commissions are part of the basis, part of the gain, "
                "or left out of the report.",
                "include-in-basis",
                {"include-in-basis", "include-in-gain", "ignore-brokerage"}));
            options.push_back (switch_option (
                "show-symbol", "Show ticker symbols",
                "Display the ticker symbols.", true));
            options.push_back (switch_option (
                "show-listing", "Show listings", "Display exchange listings.",
                true));
            options.push_back (switch_option (
                "show-shares", "Show number of shares",
                "Display numbers of shares in accounts.", true));
            options.push_back (switch_option (
                "show-price", "Show prices", "Display share prices.", true));
        }
        /* No period preset: the end date is the valuation date, so "all
         * dates" cannot mean an unbounded end. */
        return options;
    }
    else if (name == "Lot Viewer")
    {
        /* lot-viewer.scm's General page: a date interval, one account and a
         * description filter. */
        options.push_back (date_option (
            "start", "Start Date",
            "The first day of the period whose transactions are listed.",
            default_start_date ()));
        options.push_back (date_option (
            "end", "End Date",
            "The last day of the period whose transactions are listed.",
            default_end_date ()));
        options.push_back (account_selection_option (
            "account", "Account",
            "The account to search for lots, as an account GUID (or \"all\", "
            "which selects every account and is then narrowed to the first "
            "one).  Without it the report renders its own default page: the "
            "Scheme option's unset value is a null account, whose split query "
            "matches every split in the book, so the page lists every lot the "
            "book's transactions touch, in the default currency."));
        options.push_back (OptionDescriptor{
            "description-filter", OptionType::Text, "Description Filter",
            "Only transactions whose description contains this string are "
            "listed.  Empty means every transaction in the period.", "", {}});
        return options;
    }
    else if (name == "IFRS weighted-average cost basis report")
    {
        /* ifrs-cost-basis.scm's General page.  The five account selections say
         * which splits are the stock, the proceeds, the dividends, the
         * capital gains and the fees; the report identifies each transaction
         * from their signs. */
        options.push_back (date_option (
            "start", "Start Date",
            "The first day of the period whose transaction rows are printed.  "
            "Earlier transactions still move the running cost basis.",
            default_start_date ()));
        options.push_back (date_option (
            "end", "End Date",
            "The last day of the period whose transaction rows are printed.",
            default_end_date ()));
        options.push_back (currency_option ());
        options.push_back (account_selection_option (
            "accounts", "Stock Account",
            "The stock account whose cost basis is accumulated, as an account "
            "GUID.  Without it the option's own default applies: the first "
            "stock or mutual fund account in the book, which is what the "
            "Scheme option resolves to.  A book with no such account renders "
            "the engine's \"specify report options\" warning."));
        options.push_back (account_selection_option (
            "proceeds-account", "Proceeds Account",
            "The asset or bank account the sale proceeds and purchase "
            "payments are posted to.  Each of these four selections has the "
            "same kind of default: the first account in the book of the "
            "option's own allowed types (an asset or bank account here), which "
            "is what an unset Scheme option resolves to.  Without a matching "
            "amount no transaction is identified as a purchase or a sale."));
        options.push_back (account_selection_option (
            "dividend-account", "Dividend Account",
            "The income account dividends are posted to.  Unset, it is the "
            "first income account in the book."));
        options.push_back (account_selection_option (
            "capgains-account", "Cap Gains Account",
            "The income account the register's realised capital gains are "
            "posted to.  Unset, it is the first income account in the book."));
        options.push_back (account_selection_option (
            "fees-account", "Fees Account",
            "The expense account commissions are posted to.  Unset, it is the "
            "first expense account in the book."));
        options.push_back (OptionDescriptor{
            "fee-action", OptionType::Text, "Action field filter for fees",
            "A split in the stock account whose action field equals this "
            "string is a capitalised fee.", "Fee", {}});
        options.push_back (switch_option (
            "format-cells", "Format monetary cells",
            "Print each amount with its currency.", true));
        options.push_back (switch_option (
            "format-short", "Alternative row-style for shorts",
            "Use the alternate row style for short positions instead of every "
            "other row.", true));
        options.push_back (switch_option (
            "capitalise-purchase-costs", "Capitalise purchase commissions",
            "Capitalise purchase commissions in the cumulative average cost "
            "and the gain after commission.", true));
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

    OwnerKind owner_kind;
    if (owner_report_kind (name, owner_kind))
    {
        /* The four Scheme registrations differ only by the owner kind: the
         * account type (A/Payable for vendors and employees, A/Receivable for
         * customers and jobs) and the matching GUID both follow from it. */
        OwnerReportOptions options = request.owner_options;
        options.kind = owner_kind;
        options.owner_guid = request.owner;
        if (request.all_dates)
        {
            options.start.reset ();
            options.end.reset ();
        }
        else
        {
            options.start = request.start.value_or (default_start_date ());
            options.end = request.end.value_or (default_aging_date ());
        }
        html = render_owner_report (owner_report (root, options));
        return true;
    }

    if (name == "Customer Summary")
    {
        CustomerSummaryOptions options = request.customer_summary_options;
        if (request.all_dates)
        {
            options.start.reset ();
            options.end.reset ();
        }
        else
        {
            options.start = request.start.value_or (default_start_date ());
            options.end = request.end.value_or (default_end_date ());
        }
        CustomerSummary summary = customer_summary (root, options);
        if (summary.warning == "No accounts selected")
            html = render_no_accounts ("Customer Summary", false);
        else
            html = render_cell_table (summary.table, summary.title);
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

    if (name == "Tax Schedule Report/TXF Export")
    {
        TaxScheduleReport report = tax_schedule (root, tax_options (request));
        if (request.tax_format == "txf")
            html = report.txf;
        else
            html = render_cell_table (report.table, report.title);
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
    if (name == "Balance Sheet (Multicolumn)" ||
        name == "Income Statement (Multicolumn)")
    {
        MulticolumnOptions options;
        options.start = start;
        options.end = end;
        ColumnPeriod period = ColumnPeriod::Disabled;
        const std::string period_name =
            request.choice_option ("period-duration", "disabled");
        if (!column_period_from_name (period_name, period))
        {
            /* The route rejects an unknown choice against the descriptor, so
             * this can only be a value the descriptor published and the
             * report cannot resolve; keep the default rather than guessing. */
            period = ColumnPeriod::Disabled;
        }
        options.period = period;
        options.reverse_chrono = request.boolean_option ("reverse-chrono", true);
        options.show_zero_accounts =
            request.boolean_option ("show-zb-accts", true);
        options.account_full_name =
            request.boolean_option ("account-full-name", false);
        options.label_sections =
            request.boolean_option ("label-sections", true);
        options.total_sections =
            request.boolean_option ("total-sections", true);
        options.include_overall_period =
            request.boolean_option ("include-overall-period", false);
        options.currency = request.currency;

        const bool balance_sheet = name == "Balance Sheet (Multicolumn)";
        std::string title = name;
        title += " ";
        if (balance_sheet && options.period == ColumnPeriod::Disabled)
            title += date_text (end);
        else
            title += interval;

        const bool no_accounts =
            root == nullptr || gnc_account_n_children (root) == 0;
        if (no_accounts)
        {
            html = render_no_accounts (name, false);
            return true;
        }
        html = render_html (balance_sheet
                                ? balance_sheet_multicolumn (root, options)
                                : income_statement_multicolumn (root, options),
                            title);
        return true;
    }
    if (name == "Equity Statement")
    {
        EquityStatementOptions options;
        options.start = start;
        options.end = end;
        options.currency = request.currency;
        const std::string title =
            request.text_option ("report-title", "Equity Statement");
        /* The Scheme report's own empty-selection page: a book with no
         * accounts at all cannot have an equity roll-forward to print. */
        if (root == nullptr || gnc_account_n_children (root) == 0)
        {
            html = render_no_accounts (title, false);
            return true;
        }
        html = render_html (equity_statement (root, options), title);
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
    if (name == "General Journal")
    {
        /* The Scheme report's query is hard-wired to every non-void
         * transaction in the book: it has no account, date or scope option,
         * so the defaults are the whole listing.  Only the register display
         * switches are options. */
        GeneralJournalOptions options;
        options.show_date = request.boolean_option ("show_date", true);
        options.show_num = request.boolean_option ("show_num", false);
        options.show_description =
            request.boolean_option ("show_description", true);
        options.show_memo = request.boolean_option ("show_memo", true);
        options.show_account = request.boolean_option ("show_account", true);
        options.show_running_balance =
            request.boolean_option ("show_running_balance", false);
        html = render_cell_table (general_journal (root, options),
                                  "General Journal");
        return true;
    }
    if (name == "Transaction Breakdown Report")
    {
        BreakdownOptions options;
        const auto account = request.account_lists.find ("account");
        if (account != request.account_lists.end () &&
            !account->second.empty ())
            options.account = account->second.front ();
        options.start = start;
        options.end = end;
        options.description_filter =
            request.text_option ("description-filter", "");
        options.max_columns = 10;
        if (const auto limit = request.texts.find ("max-columns");
            limit != request.texts.end ())
        {
            char* end_text = nullptr;
            const long parsed = std::strtol (limit->second.c_str (),
                                             &end_text, 10);
            if (end_text != limit->second.c_str ())
                options.max_columns = static_cast<int> (
                    std::min<long> (std::max<long> (parsed, 1), 100));
        }

        BreakdownReport report = transaction_breakdown (root, options);
        if (report.no_data)
        {
            html = render_warning (
                "Transaction Breakdown Report", "No data",
                "The selected accounts contain no data/transactions (or only "
                "zeroes) for the selected time period");
            return true;
        }
        if (report.too_many_accounts)
        {
            html = render_warning (
                "Transaction Breakdown Report", "Too many accounts",
                "The number of accounts linked by the transactions found "
                "exceeds the limit.  Select a different subset of "
                "transactions, or increase the limit in the options.");
            return true;
        }
        html = render_cell_table (report.table,
                                  "Transaction Breakdown Report");
        return true;
    }
    if (name == "Reconciliation Report")
    {
        /* reconcile-report.scm's own defaults: no account selected, which is
         * the engine's "No accounts selected" warning plus the report's
         * explanation. */
        if (request.accounts.empty ())
        {
            html = render_reconciliation_no_accounts ();
            return true;
        }
        ReconciliationOptions options;
        options.accounts = request.accounts;
        options.start = start;
        options.end = end;
        options.show_reconciled_date =
            request.boolean_option ("show_reconciled_date", true);
        CellTable table = reconciliation_report (root, options);
        if (table.rows.empty ())
        {
            html = render_warning (
                "Reconciliation Report", "No matching transactions found",
                "No transactions were found that match the time interval and "
                "account selection specified in the Options panel.");
            return true;
        }
        html = render_cell_table (table, "Reconciliation Report");
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
    if (name == "Investment Portfolio")
    {
        /* An as-of report: the end date is the valuation date, so a start
         * date is accepted and ignored. */
        PortfolioOptions options = portfolio_options (request, false);
        PortfolioReport report =
            investment_portfolio (root, options, request.currency);
        if (report.table.rows.empty ())
        {
            /* The Scheme report's empty selection renders the engine's
             * "No accounts selected" warning, not an empty table. */
            std::vector<Account*> accounts = options.accounts;
            if (accounts.empty ())
                accounts = default_portfolio_accounts (root);
            if (accounts.empty ())
            {
                html = render_no_accounts ("Investment Portfolio", false);
                return true;
            }
        }
        html = render_cell_table (report.table, report.title);
        return true;
    }
    if (name == "Advanced Portfolio")
    {
        PortfolioOptions options = portfolio_options (request, true);
        PortfolioReport report =
            advanced_portfolio (root, options, request.currency);
        if (report.table.rows.empty ())
        {
            std::vector<Account*> accounts = options.accounts;
            if (accounts.empty ())
                accounts = default_portfolio_accounts (root);
            if (accounts.empty ())
            {
                html = render_no_accounts ("Advanced Portfolio", false);
                return true;
            }
        }
        html = render_cell_table (report.table, report.title);
        return true;
    }
    if (name == "Lot Viewer")
    {
        LotViewerOptions options;
        options.start = request.start.value_or (default_start_date ());
        options.end = request.end.value_or (default_end_date ());
        options.account = request.accounts.empty ()
                              ? first_account_option (request, "account")
                              : request.accounts.front ();
        options.root = root;
        options.description_filter =
            request.text_option ("description-filter", "");
        LotViewerReport report = lot_viewer (options);
        if (report.warning == "no-data")
            html = render_warning (
                "Lot Viewer", "No data",
                "The selected accounts contain no data/transactions (or only "
                "zeroes) for the selected time period");
        else
            html = render_cell_table (report.table, report.title);
        return true;
    }
    if (name == "IFRS weighted-average cost basis report")
    {
        IfrsOptions options;
        options.start = gnc_time64_get_day_start (
            request.start.value_or (default_start_date ()));
        options.end = gnc_time64_get_day_start (
            request.end.value_or (default_end_date ()));
        /* Each of the five selections falls back to the value the engine's
         * own account-selection default would produce -- the first account of
         * its allowed types -- so the report's default run renders the same
         * ledger the Scheme report's default run does. */
        options.stock_account =
            request.accounts.empty ()
                ? first_account_of_types (root, {ACCT_TYPE_STOCK,
                                                 ACCT_TYPE_MUTUAL})
                : request.accounts.front ();
        options.proceeds_account =
            first_account_option (request, "proceeds-account");
        if (options.proceeds_account == nullptr)
            options.proceeds_account = first_account_of_types (
                root, {ACCT_TYPE_ASSET, ACCT_TYPE_BANK});
        options.dividend_account =
            first_account_option (request, "dividend-account");
        if (options.dividend_account == nullptr)
            options.dividend_account =
                first_account_of_types (root, {ACCT_TYPE_INCOME});
        options.capgains_account =
            first_account_option (request, "capgains-account");
        if (options.capgains_account == nullptr)
            options.capgains_account =
                first_account_of_types (root, {ACCT_TYPE_INCOME});
        options.fees_account = first_account_option (request, "fees-account");
        if (options.fees_account == nullptr)
            options.fees_account =
                first_account_of_types (root, {ACCT_TYPE_EXPENSE});
        options.cap_fee_action = request.text_option ("fee-action", "Fee");
        options.format_cells = request.boolean_option ("format-cells", true);
        options.short_alternate_row =
            request.boolean_option ("format-short", true);
        options.capitalize_purchase_costs =
            request.boolean_option ("capitalise-purchase-costs", true);
        options.currency = request.currency;
        IfrsReport report = ifrs_cost_basis (root, options);
        if (!report.warning.empty ())
            html = render_warning (report.title, "",
                                   "This report requires you to specify "
                                   "certain report options.");
        else
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

bool
render_tax_export (const std::string& name, Account* root,
                   const ReportRequest& request, std::string& out)
{
    if (name != "Tax Schedule Report/TXF Export" || root == nullptr)
        return false;
    out = tax_schedule (root, tax_options (request)).txf;
    return true;
}

} // namespace report
} // namespace gnc
