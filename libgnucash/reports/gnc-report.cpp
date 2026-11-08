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
    gnc_commodity* currency = gnc_default_report_currency ();
    if (currency == nullptr && root != nullptr)
        currency = xaccAccountGetCommodity (root);

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
    gnc_commodity* currency = gnc_default_report_currency ();
    if (currency == nullptr && root != nullptr)
        currency = xaccAccountGetCommodity (root);

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
balance_sheet (Account* root, time64 as_of)
{
    gnc_commodity* currency = gnc_default_report_currency ();
    if (currency == nullptr && root != nullptr)
        currency = xaccAccountGetCommodity (root);

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
    return table;
}

/* ----------------------------------------------------- income statement */

Table
income_statement (Account* root, time64 start, time64 end)
{
    gnc_commodity* currency = gnc_default_report_currency ();
    if (currency == nullptr && root != nullptr)
        currency = xaccAccountGetCommodity (root);

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

    out << "</tbody></table></body></html>\n";
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
        << html_escape (title)
        << "</title><style>"
           "body{font-family:system-ui,sans-serif;margin:1rem;color:#222}"
           "h3{font-size:1.1rem}"
           "</style></head><body>";
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
            "Profit & Loss", "Transaction Report", "General Ledger"};
}

bool
render_report (const std::string& name, Account* root,
               const ReportRequest& request, std::string& html)
{
    if (root == nullptr)
        return false;

    const time64 end = request.end.value_or (default_end_date ());
    const time64 start = request.start.value_or (default_start_date ());

    char date[32] = {0};
    qof_print_date_buff (date, sizeof date, end);

    if (name == "Trial Balance")
    {
        html = render_html (trial_balance (root, end),
                            std::string ("Trial Balance ") + date);
        return true;
    }
    if (name == "Balance Sheet")
    {
        html = render_html (balance_sheet (root, end),
                            std::string (" Balance Sheet ") + date);
        return true;
    }
    if (name == "Income Statement" || name == "Profit & Loss")
    {
        char from[32] = {0};
        qof_print_date_buff (from, sizeof from, start);
        std::string title = name;
        title += " For Period Covering ";
        title += from;
        title += " to ";
        title += date;
        html = render_html (income_statement (root, start, end), title);
        return true;
    }
    if (name == "Transaction Report")
    {
        /* The Scheme report's default account selection is empty, and an
         * empty selection produces its "No accounts selected" warning, not a
         * report over the whole book.  Same output here until the options
         * surface (stage 3) can choose accounts. */
        if (request.accounts.empty ())
        {
            html = render_no_accounts ("Transaction Report", false);
            return true;
        }
        html = render_cell_table (
            transaction_report (root, request.accounts, request.start,
                                request.end),
            "Transaction Report");
        return true;
    }
    if (name == "General Ledger")
    {
        if (request.accounts.empty ())
        {
            html = render_no_accounts ("General Ledger", true);
            return true;
        }
        html = render_cell_table (
            general_ledger (root, request.accounts, request.start,
                            request.end),
            "General Ledger");
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
