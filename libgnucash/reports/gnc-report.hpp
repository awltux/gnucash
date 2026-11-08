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

    /** Leave closing entries out of interval balances, as the Scheme
     *  reports' "pre-closing" balance mode does.  Closing entries are the
     *  transactions whose description matches the closing pattern. */
    bool skip_closing = false;
    std::string closing_pattern = "Closing Entries";

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
    /** A section heading ("Assets"): a label with no amount at all. */
    Section,
};

struct Row
{
    RowType type = RowType::Account;
    std::string label;
    std::string guid;
    int depth = 0;
    gnc_numeric amount = gnc_numeric_zero ();
    /** Print a blank amount cell instead of the (zero) amount. */
    bool blank = false;
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

/** The Balance Sheet ("Balance Sheet", balance-sheet.scm) with the Scheme
 *  report's defaults: single-column form, assets/liabilities/equity in that
 *  order, each account's own balance as of `as_of`, liability and equity
 *  amounts sign-reversed, section labels and section totals, and the income
 *  and expense balances rolled up into retained earnings.  Accounts deeper
 *  than the Scheme default display depth (3 levels) are shown flat, which is
 *  the same thing the Scheme report does for the books we test. */
Table balance_sheet (Account* root, time64 as_of);

/** The Income Statement / Profit & Loss ("Income Statement" and
 *  "Profit & Loss", both income-statement.scm) with the Scheme report's
 *  defaults: income and expense accounts, their interval balances for
 *  [start, end], sign-reversed income, section totals, and net income.
 *  Closing entries are excluded, as the Scheme report's pre-closing mode
 *  does. */
Table income_statement (Account* root, time64 start, time64 end);

/** The end date the Scheme report's date option defaults to. */
time64 default_end_date ();

/** The start date the Scheme report's period option defaults to. */
time64 default_start_date ();

/** Render a table as a standalone HTML document.  The markup is not the
 *  Scheme engine's, but it uses the same cell classes so a parity test can
 *  read both. */
std::string render_html (const Table& table, const std::string& title);

/* ----------------------------------------------------------- cell tables */

/** Reports that list entries rather than account balances need several text
 *  columns, so they carry their own cells instead of one amount per row. */
struct Cell
{
    std::string text;
    std::string css;
};

struct CellTable
{
    std::vector<std::string> headings;
    std::vector<std::vector<Cell>> rows;
};

std::string render_cell_table (const CellTable& table, const std::string& title);

/** The Transaction Report ("Transaction Report", transaction.scm) with the
 *  Scheme report's defaults: every split of the selected accounts in the
 *  period, grouped by account name in the Scheme column order (date, num,
 *  description, memo/notes, transfer, debit, credit), a "Total For" line per
 *  account and a grand total.
 *
 *  `accounts` is the report's account selection.  The Scheme report's
 *  default selection is empty, and an empty selection is not "the whole
 *  book": the Scheme report then shows its "No accounts selected" warning,
 *  so render_report() renders that instead of calling this function.  The
 *  selection is explicit here so stage 3's options can supply it. */
CellTable transaction_report (Account* root,
                              const std::vector<Account*>& accounts,
                              std::optional<time64> start,
                              std::optional<time64> end);

/** The General Ledger ("General Ledger", general-ledger.scm), with the Scheme
 *  report's defaults: groups keyed by account code in the Scheme's order
 *  (which means accounts sharing a code share a group), each line showing the
 *  split, the debit/credit pair and the account's balance after that split
 *  ("Running Balance"), a "Balance b/f" heading per group and a "Total For"
 *  line.  It takes the same explicit account selection as the transaction
 *  report, and its default selection is empty too. */
CellTable general_ledger (Account* root,
                          const std::vector<Account*>& accounts,
                          std::optional<time64> start,
                          std::optional<time64> end);

/** The Scheme engine's warning for a report whose account selection is
 *  empty: "No accounts selected", and the explanation that the report needs
 *  accounts chosen in its options.  This is the correct output for the
 *  default options of the transaction report and the general ledger -- not
 *  an error -- and stage 3 replaces it with real output once options can be
 *  set. */
std::string render_no_accounts (const std::string& title,
                                bool with_title_heading);

/* Report registry: names, and rendering by name.  Unknown names are left to
 * the Scheme engine (when the build has one). */
std::vector<std::string> native_report_names ();

/** Dates and account selection a caller may pin instead of the report's own
 *  defaults.  Stage 3 (options over HTTP) builds on this; the parity harness
 *  is its first user, because the Scheme reports can only be asked for a
 *  period and a selection through their own options.  An empty `accounts`
 *  means the report's default selection, which for the transaction report
 *  and the general ledger is empty and therefore renders the warning. */
struct ReportRequest
{
    std::optional<time64> start;
    std::optional<time64> end;
    std::vector<Account*> accounts;
};

bool render_report (const std::string& name, Account* root,
                    const ReportRequest& request, std::string& html);

bool render_report (const std::string& name, Account* root, std::string& html);

} // namespace report
} // namespace gnc

#endif
