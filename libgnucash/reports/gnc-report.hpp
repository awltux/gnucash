/********************************************************************\
 * gnc-report.hpp -- native report layer (no Guile)                 *
 *                                                                  *
 * The query/grouping/render model shared by native reports.  It    *
 * deliberately speaks the engine's own vocabulary -- accounts,     *
 * splits, gnc_numeric -- rather than replacing any of it.          *
 *                                                                  *
 * This program is free software; you can redistribute it and/or    *
 * modify it under the terms of the GNU General Public License as   *
 * published by the Free Software Foundation; either version 2 of   *
 * the License, or (at your option) any later version.              *
\********************************************************************/

#ifndef GNC_REPORT_HPP
#define GNC_REPORT_HPP

#include <optional>
#include <string>
#include <vector>

#include <Account.h>
#include <gnc-numeric.h>

namespace gnc
{
namespace report
{

/* ------------------------------------------------------------------ query */

/** Which splits a report sees.  The account scope is a subtree root; the
 *  date range is inclusive and compared to the transaction's posted date,
 *  exactly as gnc_account_foreach_split_between_dates() does. */
struct Query
{
    Account* scope = nullptr;
    std::optional<time64> start;
    std::optional<time64> end;
    bool skip_voided = false;
    bool reconciled_only = false;

    /** Convert each account's amounts into this commodity.  Null means the
     *  account's own commodity, i.e. no conversion. */
    gnc_commodity* currency = nullptr;

    /** Sum the matching splits of one account (own splits only). */
    gnc_numeric sum (Account* account) const;
};

/* --------------------------------------------------------- column vocabulary */

enum class Column
{
    AccountName,
    Debit,
    Credit,
    Balance,
    Subtotal,
};

/** Stable name used by the report spec: "account-name", "debit", ... */
const char* column_name (Column column);

/** Human heading used by the HTML renderer: "Account Name", "Debit", ... */
const char* column_heading (Column column);

/* -------------------------------------------------------------- table model */

enum class RowType
{
    Account,
    Subtotal,
    Total,
};

struct Row
{
    RowType type = RowType::Account;
    std::string label;
    std::string guid;
    int depth = 0;
    gnc_numeric amount = gnc_numeric_zero ();
};

/** A rendered table.  `amount` is signed: the Debit column shows positive
 *  amounts, the Credit column their negations, and Balance shows the sign. */
struct Table
{
    std::vector<Column> columns;
    std::vector<Row> rows;
    gnc_commodity* currency = nullptr;
    bool has_totals = false;
    gnc_numeric debit_total = gnc_numeric_zero ();
    gnc_numeric credit_total = gnc_numeric_zero ();
};

/* --------------------------------------------------------------- grouping */

struct Grouping
{
    /** A parent row shows the sum of its subtree instead of its own splits. */
    bool rollup = false;
    /** Emit a subtotal row after each parent's displayed children. */
    bool subtotals = false;
};

/** Build a table from the account tree below `root`, in the engine's sorted
 *  order.  Every account accepted by `filter` gets a row; parents are
 *  visited before their children (flatten semantics), and rollup/subtotal
 *  rows aggregate the subtree.  A null filter accepts every account. */
Table account_table (Account* root, const Query& query,
                     const std::vector<Column>& columns,
                     const Grouping& grouping,
                     bool (*filter) (const Account*) = nullptr);

/* --------------------------------------------------------------- reports */

/** The Trial Balance "current" variant, matching the Scheme report's
 *  defaults for the fixture book: every account in the selected account
 *  types, its own balance at `end_date`, positive in the debit column and
 *  negative (shown as an absolute credit) in the credit column, plus the
 *  two column totals. */
Table trial_balance (Account* root, time64 end_date);

/** The end date the Scheme report's date option defaults to. */
time64 default_end_date ();

/** Render a table as a standalone HTML document.  The markup is not the
 *  Scheme engine's, but it uses the same cell classes so a parity test can
 *  read both. */
std::string render_html (const Table& table, const std::string& title);

/* Report registry: names, and rendering by name.  Unknown names are left to
 * the Scheme engine (when the build has one). */
std::vector<std::string> native_report_names ();
bool render_report (const std::string& name, Account* root,
                    std::string& html);

} // namespace report
} // namespace gnc

#endif
