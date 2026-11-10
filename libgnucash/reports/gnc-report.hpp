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

#include <map>
#include <optional>
#include <string>
#include <vector>

#include <Account.h>
#include <gnc-budget.h>
#include <gnc-numeric.h>

#include "gnc-invoice.hpp"

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

/** Every column a report spec may ask for, in display order. */
std::vector<Column> column_vocabulary ();

/** Resolve a spec's column name ("debit", ...).  False for a name outside
 *  the vocabulary; the caller reports it as a bad spec. */
bool column_from_name (const std::string& name, Column& column);

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
    /** One amount per Table::column_headings entry, in the same order, for a
     *  table with a grouped column axis.  Empty means the row is a
     *  single-amount row and `amount` is what it prints. */
    std::vector<gnc_numeric> values;
    /** A period row is the net of its accounts' activity, but the two sides
     *  are not recoverable from the net: keep them so the Debit/Credit
     *  columns and a multi-series chart can print the activity separately.
     *  Both are positive magnitudes and amount == debit - credit. */
    bool split_amounts = false;
    gnc_numeric debit = gnc_numeric_zero ();
    gnc_numeric credit = gnc_numeric_zero ();
    /** Print a blank amount cell instead of the (zero) amount. */
    bool blank = false;
};

/** A chart section attached to a table.  The renderer emits the data as JSON
 *  and draws it with the vendored Chart.js; the data lives in the document so
 *  a reader (or a test) can see the numbers the chart is made of without
 *  running the script. */
enum class ChartType
{
    Bar,
    Line,
};

/** One line or bar series: a label and one value per chart label. */
struct ChartSeries
{
    std::string label;
    std::vector<gnc_numeric> values;
};

struct Chart
{
    ChartType type = ChartType::Bar;
    std::string title;
    std::vector<std::string> labels;
    /** One or two series, each drawn over the same labels. */
    std::vector<ChartSeries> series;
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
    /** A grouped column axis: one heading per amount column, in display
     *  order.  A report whose rows carry a value per period (the multicolumn
     *  statements) sets this, and its rows carry one Row::values entry per
     *  heading; the renderer emits one amount cell per heading instead of the
     *  single amount column `columns` describes.  Empty for the ordinary
     *  table. */
    std::vector<std::string> column_headings;
    /** The heading of the row axis when `column_headings` is set: the Scheme
     *  multicolumn reports head it "Date" (balance sheet) or "Period"
     *  (profit & loss).  Empty prints an empty corner cell. */
    std::string row_heading;
    /** An optional chart section rendered below the table. */
    std::optional<Chart> chart;
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

/* ------------------------------------------------------------ report spec */

/** How a designer-built report groups its rows. */
enum class GroupingMode
{
    /** One flat row per account in scope (a Trial Balance shape). */
    None,
    /** The account tree, with subtree rollups and a subtotal per parent. */
    AccountTree,
    /** One row per period in the date range, totalling the scope. */
    Period,
};

enum class PeriodUnit
{
    Year,
    Month,
};

/** A report built from the vocabulary rather than from a C++ report: the
 *  designer saves this shape as JSON, and the same renderer serves a preview
 *  and a saved report. */
struct TableSpec
{
    std::string title;
    std::vector<Column> columns;
    GroupingMode grouping = GroupingMode::None;
    PeriodUnit period_unit = PeriodUnit::Year;
    std::optional<time64> start;
    std::optional<time64> end;
    gnc_commodity* currency = nullptr;
    /** The accounts in scope.  Empty means every account in the book. */
    std::vector<Account*> accounts;
    /** Further restrict the scope to these account types.  Empty means every
     *  type.  This is what makes a built-in spec portable between books: a
     *  GUID means one book, an account type means every book. */
    std::vector<GNCAccountType> account_types;
    /** A period row prints its period's activity (the default) or the
     *  balance as of the end of the period ("cumulative"). */
    bool cumulative = false;
    /** Add a chart below the table. */
    bool chart = false;
    ChartType chart_type = ChartType::Bar;
    /** The amount columns the chart plots, in series order: one or two of
     *  Debit/Credit/Balance.  Empty means derive from `columns`. */
    std::vector<Column> chart_columns;
};

/** The account type names a spec may scope by, in display order: the engine's
 *  persistent names (xaccAccountTypeEnumAsString), without the hidden root. */
std::vector<std::string> account_type_vocabulary ();

/** Resolve an account type name from account_type_vocabulary().  False for a
 *  name outside the vocabulary (including "ROOT"). */
bool account_type_from_name (const std::string& name, GNCAccountType& type);

/** Render a TableSpec as an HTML document.  A period-grouped spec with no
 *  dates uses the range the scoped transactions cover; when the scope has no
 *  transactions `error` (when given) explains it and nothing is rendered. */
bool render_table_spec (const TableSpec& spec, Account* root,
                        std::string& html, std::string* error = nullptr);

/* --------------------------------------------------------------- reports */

/** The reports take an optional date range so stage 3's options can ask for
 *  a period or an as-of date.  A missing bound means that side is not
 *  bounded: the whole book for an as-of report, every split for a period
 *  report.  A null `currency` means the report's default commodity
 *  (gnc_default_report_currency(), falling back to the root account's
 *  commodity); a non-null one is what the report converts into. */

/** The Trial Balance "current" variant, matching the Scheme report's
 *  defaults for the fixture book: every account in the selected account
 *  types, its own balance at `end_date`, positive in the debit column and
 *  negative (shown as an absolute credit) in the credit column, plus the
 *  two column totals.
 *
 *  The Scheme "current" variant ignores its start date (its account table
 *  is given only an end date), so this report is as-of: a caller's start
 *  date is accepted by the route for range symmetry but has no effect. */
Table trial_balance (Account* root, std::optional<time64> end_date,
                     gnc_commodity* currency = nullptr);

/** The Balance Sheet ("Balance Sheet", balance-sheet.scm) with the Scheme
 *  report's defaults: single-column form, assets/liabilities/equity in that
 *  order, each account's own balance as of `as_of`, liability and equity
 *  amounts sign-reversed, section labels and section totals, and the income
 *  and expense balances rolled up into retained earnings.  Accounts deeper
 *  than the Scheme default display depth (3 levels) are shown flat, which is
 *  the same thing the Scheme report does for the books we test.
 *
 *  The Scheme report has a single "Balance Sheet Date" option, so this
 *  report is as-of too: a start date has no effect. */
Table balance_sheet (Account* root, std::optional<time64> as_of,
                     gnc_commodity* currency = nullptr);

/** The Income Statement / Profit & Loss ("Income Statement" and
 *  "Profit & Loss", both income-statement.scm) with the Scheme report's
 *  defaults: income and expense accounts, their interval balances for
 *  [start, end], sign-reversed income, section totals, and net income.
 *  Closing entries are excluded, as the Scheme report's pre-closing mode
 *  does. */
Table income_statement (Account* root, std::optional<time64> start,
                        std::optional<time64> end,
                        gnc_commodity* currency = nullptr);

/* ---------------------------------------------- multicolumn statements */

/** The period a multicolumn statement's columns cover: the Scheme
 *  balsheet-pnl.scm report's "Period duration" option.  Disabled is the
 *  single-column form, which is the option's default. */
enum class ColumnPeriod
{
    Disabled,
    Year,
    HalfYear,
    Quarter,
    Month,
    TwoWeeks,
    Week,
};

/** The route's name for a period: "disabled", "year", "half-year",
 *  "quarter", "month", "two-weeks", "week". */
const char* column_period_name (ColumnPeriod period);

/** Resolve a period name.  False for a name outside the vocabulary; the
 *  caller reports it as a bad option. */
bool column_period_from_name (const std::string& name, ColumnPeriod& period);

/** What the two multicolumn statements let a caller set.  The Scheme report
 *  has more switches; the ones here are the ones the native table can honour
 *  (see balance_sheet_multicolumn()). */
struct MulticolumnOptions
{
    std::optional<time64> start;
    std::optional<time64> end;
    /** One column per period (newest first when `reverse_chrono`), or the
     *  single-column form when Disabled. */
    ColumnPeriod period = ColumnPeriod::Disabled;
    /** The Scheme report's "Period order is most recent first": the default,
     *  and it reverses the amount columns only. */
    bool reverse_chrono = true;
    /** "Include accounts with zero total balances": off drops the accounts
     *  whose every column is zero. */
    bool show_zero_accounts = true;
    /** "Account full name instead of indenting". */
    bool account_full_name = false;
    /** "Label sections" and "Include totals". */
    bool label_sections = true;
    bool total_sections = true;
    /** "If more than 1 period column, include overall period?": the profit &
     *  loss adds a column covering the whole range. */
    bool include_overall_period = false;
    gnc_commodity* currency = nullptr;
};

/** The Balance Sheet (Multicolumn) ("Balance Sheet (Multicolumn)",
 *  balsheet-pnl.scm) with the Scheme report's default options, which are the
 *  single-column form as of the end date: one row per account in the report's
 *  own sorted order, a parent's row showing its subtree's balance (the
 *  report's "Parent account amounts include children" default) and its own
 *  balance on a second row when it has one, the section titles "Asset",
 *  "Liability", "Equity" and the totals "Total For Asset", "Total For
 *  Liability", "Total For Equity", "Total For Liability and Equity".
 *  Liabilities and equity are sign-reversed, and the income and expense
 *  balances print as one "Retained Earnings" row inside equity, also
 *  reversed.
 *
 *  With a period set the report shows one column per period end ("Date"
 *  headed), in reverse chronological order by default, each holding the
 *  account's balance as of that date including closing entries, exactly as
 *  the Scheme report's gnc:account-accumulate-at-dates does.
 *
 *  Deliberately not reproduced: the "Common Currency" page (the native table
 *  converts every amount into the report currency, which is the same page on
 *  a book whose accounts share a commodity), the exchange-rate rows, the
 *  chart, the dual-column layout (a presentation of the same rows, which the
 *  native renderer emits as one table), and the hyperlinks. */
Table balance_sheet_multicolumn (Account* root,
                                 const MulticolumnOptions& options);

/** The Income Statement (Multicolumn) ("Income Statement (Multicolumn)",
 *  balsheet-pnl.scm): the same account table with the income and expense
 *  accounts, one column per period, each column the account's activity
 *  between two period ends (the Scheme report's balance difference, with
 *  closing entries left out), income sign-reversed, the section titles
 *  "Income" and "Expense", the totals "Total For Income", "Total For
 *  Expense", and the "Net Income" block, which is forced on: "Total For Net
 *  Income" is revenue minus expenses.
 *
 *  The row order follows the Scheme report's own panel order, which depends
 *  on the form: the single-column form prints Income, the Net Income block,
 *  then Expense (the Scheme report's two side-by-side panels read left to
 *  right), while a period-column run prints Income, Expense, then Net
 *  Income.
 *
 *  Not reproduced: as for balance_sheet_multicolumn(), plus the optional
 *  overall-period column is available ("include_overall_period") and the
 *  chart is not. */
Table income_statement_multicolumn (Account* root,
                                    const MulticolumnOptions& options);

/** Options for the Equity Statement.  The Scheme report's own period is a
 *  range of dates; its closing-entry pattern, currency page and display
 *  switches are not reproduced (see equity_statement()). */
struct EquityStatementOptions
{
    std::optional<time64> start;
    std::optional<time64> end;
    gnc_commodity* currency = nullptr;
};

/** The Equity Statement ("Equity Statement", equity-statement.scm): the
 *  owner's equity roll-forward, "Capital, <start date>" (the opening equity,
 *  i.e. the equity accounts plus retained earnings as of the end of the day
 *  before the start date), the period's "Net income"/"Net loss",
 *  "Investments", "Withdrawals", "Increase in capital"/"Decrease in capital"
 *  and "Capital, <end date>".  Income is sign-reversed (a credit is a
 *  positive net income), withdrawals are the positive splits of the equity
 *  accounts and investments the change in equity that the withdrawals do not
 *  explain, exactly as the Scheme report derives them.
 *
 *  Closing entries are recognised the way the Scheme report's own option
 *  does (its "Closing Entries pattern", a case-insensitive substring of the
 *  description, "Closing Entries") or by the transaction's closing flag.
 *
 *  Deliberately not reproduced, because they need the price database's
 *  average-cost source rather than a query: the "Unrealized Gains/Losses for
 *  Period" row.  On a book whose assets, liabilities and equity share the
 *  report commodity the Scheme report's own calculation is identically zero
 *  and the row is omitted, which is what this renders; a book with
 *  foreign-currency holdings would need it.  Also not reproduced: the
 *  currency page ("Show Foreign Currencies" is always off, "Show Exchange
 *  Rates" is always off), the accounting-style rules (a presentation of the
 *  same rows) and the "Report Title" option, which the route sets through
 *  the document title. */
Table equity_statement (Account* root,
                        const EquityStatementOptions& options);

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
    /** How many table columns the cell occupies.  The Scheme renderer's
     *  table cells take a colspan, and the reports that print an account
     *  heading or a warning inside the table use it to span the row. */
    int span = 1;
};

struct CellTable
{
    std::vector<std::string> headings;
    /** A multi-row header, for the reports whose columns are a tree (the
     *  Transaction Breakdown Report heads each account column with one row
     *  per account-tree level).  When this is non-empty it replaces
     *  `headings`; every row is emitted as a `<tr>` of `<th>` cells. */
    std::vector<std::vector<Cell>> header_rows;
    std::vector<std::vector<Cell>> rows;
    /** A date-interval line under the title, as the Scheme transaction
     *  engine prints above its table ("01/01/2018 to 12/31/2018"). */
    std::string subtitle;
    /** Raw HTML between the headings and the table, for a report whose
     *  Scheme page carries text of its own there (the Cash Flow's "Selected
     *  Accounts" list).  It is markup, not a cell: a report that has nothing
     *  to say here leaves it empty. */
    std::string preamble;
    /** Raw HTML after the table, for a report whose Scheme page prints a
     *  note under the table (the GST statement's tax-setup explanation). */
    std::string postamble;
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

/* -------------------------------------------------------- entry listings */

/** The General Journal's display switches.  The Scheme report is the
 *  register renderer in journal mode over every account in the book, and its
 *  options are the register's display page; the switches here are the ones
 *  that change the listing.  `show_totals` is not one of them: the Scheme
 *  report's own "Totals" option has no effect through the saved-report path
 *  (the Debit/Credit totals always print), so the effective default -- on --
 *  is what this renders. */
struct GeneralJournalOptions
{
    bool show_date = true;
    bool show_description = true;
    bool show_account = true;
    bool show_num = false;
    bool show_memo = true;
    bool show_running_balance = false;
};

/** The General Journal ("General Journal", general-journal.scm): every
 *  non-void transaction in the book, in the register's journal layout -- a
 *  date/description heading per transaction, one line per split with its
 *  memo, account and debit or credit, and the Total Debits / Total Credits
 *  totals.  Accounts are printed by full name with the engine's account
 *  separator, as the register prints them.
 *
 *  The totals are one Debit/Credit pair over the whole listing; the register
 *  prints one pair per commodity, so a book whose journal mixes commodities
 *  would need that split and is not reproduced.  The fixtures are
 *  single-currency. */
CellTable general_journal (Account* root,
                           const GeneralJournalOptions& options);

/** The Transaction Breakdown Report's options. */
struct BreakdownOptions
{
    /** The Scheme report's single account selection; null is its default and
     *  renders the engine's "No data" warning. */
    Account* account = nullptr;
    std::optional<time64> start;
    std::optional<time64> end;
    /** The Scheme report's "Description Filter": a substring of the
     *  transaction description; empty passes everything. */
    std::string description_filter;
    /** The Scheme report's "Limit for number of columns", 1..100. */
    int max_columns = 10;
};

/** The Transaction Breakdown Report ("Transaction Breakdown Report",
 *  txn-columns.scm): the transactions of one account in the period, one row
 *  per transaction, with one column per account any of those transactions
 *  touches.  The columns are headed by the account tree and ordered by
 *  account type then account order; each cell holds the split's value (and,
 *  when the account's commodity differs from the transaction's, the amount
 *  in the account's commodity in brackets); the last row totals each account
 *  over the period. */
struct BreakdownReport
{
    CellTable table;
    /** No transaction in scope: the Scheme report's "No data" warning. */
    bool no_data = false;
    /** More accounts than the column limit: the Scheme report's "Too many
     *  accounts" warning. */
    bool too_many_accounts = false;
};

BreakdownReport transaction_breakdown (Account* root,
                                       const BreakdownOptions& options);

/** The Reconciliation Report's options.  The report's defining date rule is
 *  fixed: a reconciled split is included when its reconciled date is in the
 *  period, and an unreconciled split is always included.  The primary sort
 *  key is the reconciled status, so the rows are grouped Unreconciled,
 *  Cleared, Reconciled and Frozen in that order, by date within a group. */
struct ReconciliationOptions
{
    std::vector<Account*> accounts;
    std::optional<time64> start;
    std::optional<time64> end;
    bool show_reconciled_date = true;
};

/** The Reconciliation Report ("Reconciliation Report", reconcile-report.scm):
 *  the trep listing with the reconciliation date rule and the Funds In/Funds
 *  Out calculated columns, grouped by reconciled status, with a Total For
 *  line per group and a Grand Total.
 *
 *  The totals are summed in the report currency; the Scheme report collects
 *  them per commodity and prints a pair of totals per commodity, which a
 *  multi-currency selection would need.  The fixtures are single-currency. */
CellTable reconciliation_report (Account* root,
                                 const ReconciliationOptions& options);

/** The Reconciliation Report with no account selected: the engine's "No
 *  accounts selected" warning followed by the report's own explanation, which
 *  the Scheme renderer passes as its empty-report message. */
std::string render_reconciliation_no_accounts ();

/** The Scheme engine's warning for a report whose account selection is
 *  empty: "No accounts selected", and the explanation that the report needs
 *  accounts chosen in its options.  This is the correct output for the
 *  default options of the transaction report and the general ledger -- not
 *  an error -- and stays their default until a caller passes the accounts
 *  option (or "accounts=all"). */
std::string render_no_accounts (const std::string& title,
                                bool with_title_heading);

/** The Scheme engine's gnc:html-make-generic-warning(): the report's name and
 *  a colon, an optional warning heading and an explanation.  A report whose
 *  options are incomplete renders this instead of a table. */
std::string render_warning (const std::string& title, const std::string& heading,
                            const std::string& text);

/** The Customer Summary's page for a book with no customer on its owner list:
 *  the document title and the sentence "No valid customer found.", which is
 *  what customer-summary.scm prints there -- with no table and therefore no
 *  total rows. */
std::string render_no_customers (const std::string& title);

/* ---------------------------------------------------------- aging reports */

/** The headings of the aging reports' six amount columns, in display order.
 *  They are the Scheme report's own bucket names, and the sixth one ("91+
 *  days") is open-ended. */
std::vector<std::string> aging_bucket_headings ();

/** Which invoice date decides an invoice's bucket: the due date (the Scheme
 *  report's default) or the post date.  The Scheme option is called "Due or
 *  Post Date"; the native route exposes the report's date but not this
 *  choice yet, so it stays at the default. */
enum class AgingDate
{
    Due,
    Posted,
};

struct AgingOptions
{
    /** The report date ("To") as a time64, resolved to the end of its day.
     *  It anchors the buckets: the four 30-day buckets run back from it, the
     *  "Current" bucket is everything not due yet, and "Pre-Payment" holds
     *  payments that are not applied to any invoice. */
    time64 as_of = 0;
    AgingDate date_driver = AgingDate::Due;
    /** Show owners whose six buckets are all zero, as the Scheme report's
     *  "Show zero balance items" option does. */
    bool show_zeros = false;
    /** Include inactive owners, as the Scheme report's "Show all owners
     *  including inactive" option does.  The default hides them. */
    bool show_inactive = false;
};

/** A rendered aging report: the owner rows and one Total row per A/Receivable
 *  (or A/Payable) account, in the Scheme report's column layout.
 *
 *  The Scheme report prints an explanation instead of a table when there is
 *  nothing to age, so `warning` carries that text and `table` stays empty.
 *  `title` is the document heading, "Receivable Aging - mm/dd/yy".
 *
 *  Not reproduced: the Scheme report's optional owner address columns (which
 *  are off by default), its sort-by/sort-order choices (the default is by
 *  owner name), and its list of transactions it could not process (splits in
 *  an A/Payable or A/Receivable account whose transaction is neither an
 *  invoice nor a payment); those splits are skipped here as they are there,
 *  so the amounts agree. */
struct AgingReport
{
    CellTable table;
    std::string warning;
    std::string title;
};

/** The Receivable Aging ("Receivable Aging", new-aging.scm) over every
 *  A/Receivable account below `root`: one row per customer, the six buckets
 *  and the row total, and a Total row per account. */
AgingReport receivable_aging (Account* root, const AgingOptions& options);

/** The Payable Aging, the same report over the A/Payable accounts.  Its
 *  amounts are the negation of the payable balances, so a bill owed is a
 *  positive number, exactly as the Scheme report shows it. */
AgingReport payable_aging (Account* root, const AgingOptions& options);

/** The end of today, which is the Scheme aging reports' default report date
 *  (their "To" option defaults to the relative date 'today'). */
time64 default_aging_date ();

/* ------------------------------------------------------- investment lots */

/** The period the Investment Lots report covers.  Both bounds are inclusive
 *  and are taken at the end of their day. */
struct LotOptions
{
    /** Purchases before this date still count towards a lot's amount and
     *  basis, but are not shown in the bought columns; sales before it are
     *  not counted as realised.  This is what makes the report's "Opened"
     *  date able to precede its "Start date". */
    time64 start = 0;
    /** Splits after this date are ignored.  It is also the date the end price
     *  and the short-term/long-term test are taken at. */
    time64 end = 0;
};

/** A rendered Investment Lots report: the table and its title
 *  ("Investment Lots, <interval>"). */
struct LotReport
{
    CellTable table;
    std::string title;
};

/** The Investment Lots report ("Investment Lots", investment-lots.scm) for
 *  every stock and mutual fund account below `root`, with the Scheme report's
 *  default options: one section per account (the account's full name and its
 *  end price), a 20-column table of lots -- opened/closed, bought amount,
 *  value and average price, sold splits, amount, basis, value and average
 *  price, end amount/basis/value, short- and long-term realised gain,
 *  realised CAGR, short- and long-term unrealised gain and unrealised CAGR --
 *  the account's lots total, and a grand total.
 *
 *  A sale's basis is its share of the lot's running basis, rounded to the
 *  report currency; its gain is the sale's value minus that basis; the end
 *  value is the lot's remaining amount at the end-date price.  Purchases
 *  before `start` are counted in the basis and the end amount but not in the
 *  bought columns, exactly as the Scheme report does.  Short- and long-term
 *  are decided by gnc:date-year-delta() with the Scheme default of one year.
 *
 *  Not reproduced, and documented rather than silently different: the Scheme
 *  report's optional chart (its four gain series per account do not fit the
 *  shared Chart type), its column/display/validation option pages (the
 *  defaults above are what is rendered), its account selection (the report
 *  always covers every stock and mutual fund account, which is the Scheme
 *  option's default), and its "Show blanks instead of zeros" toggle is always
 *  on because that is the default.  Lots are reported in the price source's
 *  default ("Last up through report date"); a null `currency` means
 *  gnc_default_report_currency(). */
LotReport investment_lots (Account* root, const LotOptions& options,
                           gnc_commodity* currency = nullptr);

/* ------------------------------------------------------ portfolio listings */

/** The price source a portfolio listing values its holdings at.  The Scheme
 *  reports offer five; the three price-database sources are implemented.
 *  The two cost-based sources ("weighted-average" and "average-cost") need
 *  the reports' whole purchase history as the price of a share, and are
 *  deliberately not reproduced: the option's descriptor does not offer them,
 *  so a caller asking for one is a 400 rather than a silently different
 *  number. */
enum class PriceSource
{
    /** The most recent price in the database, whatever its date. */
    Latest,
    /** The nearest price before the report date. */
    Before,
    /** The nearest price in time to the report date (either side). */
    Nearest,
};

struct PortfolioOptions
{
    /** The report date: units are the balance at the end of it and the price
     *  is looked up at it. */
    time64 date = 0;
    PriceSource price_source = PriceSource::Latest;
    /** Include accounts whose share balance is zero, as "Include accounts
     *  with no shares" does.  The default is off. */
    bool include_zero = false;
    /** The accounts to report on.  Empty means the reports' default
     *  selection: every stock and mutual fund account below the root. */
    std::vector<Account*> accounts;
    /** "Share decimal places", the precision units are printed with. */
    int share_places = 2;

    /* Advanced Portfolio only. */
    /** "average-basis", "fifo-basis" or "filo-basis". */
    std::string basis_method = "average-basis";
    /** "include-in-basis", "include-in-gain" or "ignore-brokerage". */
    std::string brokerage_fees = "include-in-basis";
    bool show_symbol = true;
    bool show_listing = true;
    bool show_shares = true;
    bool show_price = true;
};

struct PortfolioReport
{
    CellTable table;
    std::string title;
};

/** The Investment Portfolio ("Investment Portfolio", portfolio.scm): one row
 *  per stock or mutual fund account with the account, its symbol and listing,
 *  its units at `date`, the price the source gives and the holding's value in
 *  the report currency, then the Total row.  An account's units are its own
 *  balance at the report date (the Scheme report does not roll up
 *  sub-accounts), and its value is the units converted into the report
 *  currency at the same price source the Price column prints.
 *
 *  Not reproduced, because it is not part of any amount: the Scheme report's
 *  href markup (the native table prints the same texts without links). */
PortfolioReport investment_portfolio (Account* root,
                                      const PortfolioOptions& options,
                                      gnc_commodity* currency = nullptr);

/** The Advanced Portfolio ("Advanced Portfolio", advanced-portfolio.scm): one
 *  row per account with the basis, value, money in and out, realised and
 *  unrealised gain, total gain and its rate, income, brokerage fees and total
 *  return, then the grand total.  The basis is accumulated over the account's
 *  whole transaction history with the chosen basis method, which is the
 *  Scheme report's own basis-builder: "average-basis" keeps one running
 *  average, the two FIFO/LIFO methods keep the list of parcels.
 *
 *  Deliberately not reproduced, and documented rather than silently
 *  different: the "weighted-average"/"average-cost" price sources, the
 *  Scheme report's transaction-price fallback for a commodity with no
 *  price-database price (the native report values such a holding at the
 *  report currency's price of 1 and says so with the same "**" marker the
 *  Scheme report prints), its dividend-reinvestment residual account
 *  detection, and its spin-off handling.  Those paths need transaction
 *  patterns this option surface cannot describe; the defaults of all of them
 *  are what this renders, and the parity harness proves the result on a book
 *  that exercises the ordinary purchase/sale path. */
PortfolioReport advanced_portfolio (Account* root,
                                    const PortfolioOptions& options,
                                    gnc_commodity* currency = nullptr);

/* ------------------------------------------------------------- lot viewer */

struct LotViewerOptions
{
    time64 start = 0;
    time64 end = 0;
    /** The account whose lots are listed.  Null is the report's "no account
     *  chosen" case: the Scheme option's unset value is a null account, so its
     *  split query adds no account term at all and matches every split in the
     *  book, and the report lists every lot it can reach with the amounts in
     *  the default currency.  That is the report's own default page and is
     *  reproduced here; `root` is the account tree the null case walks. */
    Account* account = nullptr;
    /** The book's root account, used only when `account` is null. */
    Account* root = nullptr;
    /** The Scheme report's "Description Filter": only transactions whose
     *  description contains this string are listed.  Empty means all. */
    std::string description_filter;
};

struct LotViewerReport
{
    CellTable table;
    std::string title;
    /** The engine's warning when there is nothing to list. */
    std::string warning;
};

/** The Lot Viewer ("Lot Viewer", lot-viewer.scm): one row per transaction of
 *  the account in the period -- date, description, transaction type -- and
 *  one column per lot that any of those transactions touches, holding the
 *  amount of the transaction's split in that lot, then an "Other Account"
 *  column for the transaction's remaining splits, and a Balance row with each
 *  lot's balance.  The lot columns are headed by the lot title and its GUID's
 *  first eight characters, and the row under the headings is the lot's
 *  document (empty for a lot with no invoice, which is every lot a book has
 *  no invoice for).
 *
 *  The Scheme report prints each split's amount in the *account's* commodity,
 *  including the splits of the other accounts in the transaction; that is what
 *  the "Other Account" column shows, and it is reproduced.  With no account
 *  selected the report renders the page the Scheme report renders: a null
 *  account adds no term to its split query, so the page is the whole book's
 *  transactions and every lot they touch, with the amounts in the default
 *  currency. */
LotViewerReport lot_viewer (const LotViewerOptions& options);

/* --------------------------------------------------------- IFRS cost basis */

/** The IFRS weighted-average cost basis report ("IFRS weighted-average cost
 *  basis report", ifrs-cost-basis.scm): a running ledger over one stock
 *  account's transactions, with the cumulative average cost basis, the basis
 *  and gain of each sale, the gain the register recorded and the variance
 *  between the two, and the cumulative gross profit, net profit and total
 *  return.
 *
 *  The report identifies each transaction from the signs of its stock,
 *  proceeds, fee, dividend and capital-gain amounts, which is why it needs
 *  five account selections: the accounts say which splits are which.  An
 *  unset selection falls back to what the engine's own account-selection
 *  default resolves to -- the first account in the book of that option's
 *  allowed types -- so the report's default run is the Scheme report's
 *  default run.  Its output is in the report currency, converted at the
 *  transaction date's nearest price (an identity in a single-currency
 *  book). */
struct IfrsOptions
{
    /** Both bounds are start-of-day times and the rows printed are those
     *  whose transaction date is strictly inside them, exactly as the Scheme
     *  report's `(< startdate date enddate)` does.  Transactions outside the
     *  window still move the running totals. */
    time64 start = 0;
    time64 end = 0;
    Account* stock_account = nullptr;
    Account* proceeds_account = nullptr;
    Account* dividend_account = nullptr;
    Account* capgains_account = nullptr;
    Account* fees_account = nullptr;
    /** "Action field filter for fees": a split in the stock account whose
     *  action field equals this string is a capitalised fee. */
    std::string cap_fee_action = "Fee";
    bool capitalize_purchase_costs = true;
    bool format_cells = true;
    bool short_alternate_row = true;
    gnc_commodity* currency = nullptr;
};

struct IfrsReport
{
    CellTable table;
    std::string title;
    /** The Scheme report's "you must specify options" warning, printed when
     *  no stock account is selected. */
    std::string warning;
};

IfrsReport ifrs_cost_basis (Account* root, const IfrsOptions& options);

/* ---------------------------------------------------------- budget report */


/** The Budget Report ("Budget Report", budget.scm), the budget-vs-actual
 *  account table.  This is the core of the six Scheme budget reports: one row
 *  per account in the report's default scope (the ASSET, LIABILITY, INCOME
 *  and EXPENSE accounts below `root`, two display levels deep, exactly the
 *  Scheme option defaults), one Budget/Actual pair per budget period, and the
 *  whole budget as the period range (the Scheme "Report for range of budget
 *  periods" option defaults to off).
 *
 *  `budget` is normally the book's default budget; null renders no rows.  An
 *  empty `accounts` means the Scheme report's default type scope; a non-empty
 *  list is the caller's explicit selection (the web route has already
 *  expanded each GUID to its subtree).  A budget value set on a parent is
 *  used as is -- budget values are inclusive of sub-accounts -- and a parent
 *  with no value of its own sums its children's values, converted into the
 *  parent's commodity at the period start, as
 *  gnc:get-account-period-rolledup-budget-value() does.  The actual value is
 *  gnc_budget_get_account_period_actual_value(), which includes descendants
 *  and excludes closing entries.  Both are sign-reversed for the account
 *  types gnc_reverse_balance() reverses (income by default), and a zero
 *  budget prints "." exactly as the Scheme report prints it.
 *
 *  Not reproduced, because the native option surface has no descriptor for
 *  them: selecting a budget other than the book's default (gnc_budget_lookup()
 *  exists, but the option type does not), the Scheme report's budget-period
 *  range selection (its "start"/"end" are period numbers, not dates, so the
 *  route's date options cannot honestly stand in), the accumulate option,
 *  the optional Difference and row-total columns, and the optional budget
 *  notes column.  The defaults of all of those are what this renders. */
CellTable budget_report (Account* root, GncBudget* budget,
                         const std::vector<Account*>& accounts);

/** The Budget Income Statement and, through the same renderer, the Budget
 *  Profit & Loss ("Budget Income Statement" / "Budget Profit & Loss",
 *  budget-income-statement.scm).  Both Scheme names register the same
 *  renderer, so there is one implementation here and the caller supplies the
 *  title.
 *
 *  This is the *budgeted* statement, not budget-versus-actual: each row is the
 *  sum of the account's own budget values over every budget period (the
 *  Scheme report's default range).  The default scope is every INCOME and
 *  EXPENSE account below `root`, displayed three levels deep, with the
 *  Scheme's parent-balance default (the parent's own budget value, not its
 *  children's) and no subtotal rows.  Income accounts are sign-reversed;
 *  expenses are not.  The table is "Revenues" (with Total Revenue),
 *  "Expenses" (with Total Expenses) and the Scheme net line, which is the
 *  negated sum of *both* sections' raw budget values.  That is why a
 *  positive-budget book can print a "Net loss": it is the Scheme report's
 *  arithmetic, and the oracle is the report.
 *
 *  The Scheme report's period columns do not exist here: its default (and the
 *  only range this option surface can express) is the whole budget, so each
 *  account row has one amount.  Deliberately not reproduced, because the
 *  native option surface has no descriptor for them: choosing a budget other
 *  than the book's default, the numeric start/end period-range options, the
 *  display-depth and flatten switches, the show-foreign/omit-zero/parent-
 *  subtotal/rule/link/two-column/order switches, and the price-source choice.
 *  The defaults of all of those are what is rendered, with one limit: every
 *  amount is converted into the report currency rather than laid out in the
 *  Scheme default's per-commodity columns, and accounts below the three-level
 *  display limit are omitted from the rows (their amounts still count in the
 *  section totals).  An empty `accounts` means the default INCOME/EXPENSE
 *  scope; a non-empty list is the caller's selection (the route has expanded
 *  each GUID to its subtree). */
CellTable budget_income_statement (Account* root, GncBudget* budget,
                                   const std::vector<Account*>& accounts,
                                   gnc_commodity* currency = nullptr);

/** The Budget Balance Sheet ("Budget Balance Sheet",
 *  budget-balance-sheet.scm) with the Scheme defaults: the single-column
 *  report form, every account in the Scheme's default type filter, three
 *  display levels, and the whole budget.  Account rows show the account's
 *  balance immediately before the budget starts plus its budget values over
 *  every period; liabilities and equity are credit-normal, so their rows are
 *  sign-reversed.  The assets section prints Existing/Allocated/Unallocated
 *  Assets and Total Assets; liabilities print Existing/New/Total Liabilities;
 *  equity prints the equity accounts, Existing/New Retained Earnings (or
 *  Losses), Unrealized Gains (or Losses) when nonzero, Existing/New/Total
 *  Equity, and Total Liabilities & Equity.
 *
 *  A parent account's row is its own balance, not the subtree total, which is
 *  the Scheme parent-balance default.  The initial balance is as of the
 *  budget's first period start and therefore excludes transactions on that
 *  day, as gnc:account-get-comm-balance-at-date() does with the start date.
 *
 *  Deliberately not reproduced: choosing another budget, the account-level
 *  and flatten options, the display toggles (zero accounts/balances, section
 *  labels and totals, new/existing totals, rules, links), the multi-column
 *  report form, and the price-source choice.  Their defaults are rendered,
 *  except that amounts are converted into the report currency rather than
 *  laid out in per-commodity columns, and an account below the three-level
 *  display limit is omitted from the rows while still counting in the totals.
 *  `accounts` is the default type-filtered set when empty, otherwise the
 *  caller's already-expanded selection. */
CellTable budget_balance_sheet (Account* root, GncBudget* budget,
                                const std::vector<Account*>& accounts,
                                gnc_commodity* currency = nullptr);

/** The Budget Flow ("Budget Flow", budget-flow.scm) with the Scheme defaults:
 *  the first budget period, every account below `root`, grouped by the
 *  Scheme's account-type buckets (Assets, Liabilities, Equities, Income,
 *  Expenses, Trading Accounts), and only leaf accounts displayed.  Each leaf
 *  row prints its budget and actual amount for the period in the account's
 *  own commodity, then a Total row per type; the type totals are printed in
 *  the Scheme's reverse order and a grand total closes the table.
 *
 *  Budget Flow has no date option at all, and its Scheme "Period" option is a
 *  period *number*.  The native option surface has no period-index type, so
 *  this report always renders period 1 -- the Scheme option's default -- and
 *  does not pretend to map a date onto it.  Also not reproduced: choosing a
 *  budget other than the book's default and the price-source choice.  The
 *  account selection and report currency are honoured. */
CellTable budget_flow (Account* root, GncBudget* budget,
                       const std::vector<Account*>& accounts,
                       gnc_commodity* currency = nullptr);

/** Render the Budget Report's "no budget" explanation, the Scheme engine's
 *  gnc:html-make-generic-budget-warning(). */
std::string render_no_budget (const std::string& title);

/* ------------------------------------------------------------- cash flow */

/** The Cash Flow report's options, as its Scheme option page defines them. */
struct CashFlowOptions
{
    /** The period.  Unset means the report's own default range, which is the
     *  accounting period (the Scheme date-interval option's default). */
    std::optional<time64> start;
    std::optional<time64> end;
    /** The account selection.  Empty means the report's default selection:
     *  every bank, cash, asset, stock and mutual fund account below the
     *  root.  The route expands each GUID to that account's subtree, which is
     *  what the Scheme report's "Always show sub-accounts" switch adds, so
     *  that switch has nothing left to do here and is not published. */
    std::vector<Account*> accounts;
    /** "Account Display Depth": how many levels of the selection the
     *  "Selected Accounts" list prints.  It does not change which accounts
     *  the figures cover -- that is the selection -- and the default is 2. */
    int display_depth = 2;
    /** "Show Full Account Names": print "Assets:Checking" rather than
     *  "Checking" on the account rows. */
    bool show_full_names = true;
    /** "Show Selected Accounts": print the list of accounts the report
     *  covers above the table. */
    bool show_selected_accounts = true;
    /** "Include Trading Accounts in report": count the counterpart splits
     *  that sit in a trading account.  Off by default. */
    bool include_trading = false;
    gnc_commodity* currency = nullptr;
};

/** The Cash Flow ("Cash Flow", cash-flow.scm): the selected accounts' period
 *  activity split into money that came *into* them and money that went *out*
 *  of them, as cash-flow-calc.scm classifies it.  That classification is not
 *  a debit/credit balance: for every split of a transaction that touches a
 *  selected account, the transaction's *other* splits are examined, and a
 *  counterpart split with a negative value is money in (its value flowed from
 *  that account into the selection) while a positive one is money out.  Each
 *  counterpart is counted once per transaction, in its absolute value
 *  converted into the report currency at the transaction's date.
 *
 *  The rows are the "Money into selected accounts comes from" section with
 *  one row per contributing account and the Money In total, then the same for
 *  "Money out of selected accounts goes to" and Money Out, then the
 *  difference.  Rows are sorted by account full name with the engine's
 *  Unicode collation, which is the order the Scheme report sorts them into.
 *
 *  A selection that is empty after the default is applied renders the
 *  Scheme engine's "No accounts selected" warning rather than a table.
 *
 *  Deliberately not reproduced, because they are presentation rather than
 *  figure: the horizontal rules the Scheme table puts between the sections,
 *  and the exchange-rates table behind "Show Exchange Rates" (the native
 *  report always converts with the pricedb-nearest source, which is the
 *  Scheme option's default). */
struct CashFlowReport
{
    CellTable table;
    std::string title;
    /** The account rows the report could not resolve to a table because the
     *  selection was empty: then `table` has no rows and render_report()
     *  renders the engine's warning instead. */
    bool no_accounts = false;
};

CashFlowReport cash_flow (Account* root, const CashFlowOptions& options);

/* ------------------------------------------------------- GST statement */

/** The Income and GST Statement's options.  The Scheme report is a
 *  trep-engine listing with calculated columns; the options below are the
 *  ones that change what it computes (the account selections and the format
 *  switches) plus its period and currency. */
struct GstOptions
{
    /** The period.  Unset means the report's own default: the previous
     *  calendar quarter (the Scheme options' relative start/end defaults). */
    std::optional<time64> start;
    std::optional<time64> end;
    /** "Sales": the income accounts whose splits are the report's sales.
     *  Empty means every income account below the root, which is the Scheme
     *  option's default selection. */
    std::vector<Account*> sales;
    /** "Purchases": the expense accounts.  Empty means every expense
     *  account below the root. */
    std::vector<Account*> purchases;
    /** "Tax Accounts": the asset (tax paid) and liability (tax collected)
     *  accounts.  Empty is the Scheme option's default and leaves the tax
     *  columns empty. */
    std::vector<Account*> tax_accounts;
    gnc_commodity* currency = nullptr;
    /** The "Report Format" choice: "default", "au-bas" or "uk-vat". */
    std::string format = "default";

    /* The default format's optional columns.  The three "individual"
     * switches replace a summed column with one column per account. */
    bool individual_sales = false;
    bool individual_purchases = false;
    bool individual_tax = false;
    bool gross_balance = false;
    bool net_balance = false;
    bool tax_payable = false;
};

struct GstReport
{
    CellTable table;
    std::string title;
    /** No transaction in scope: the trep engine's "No matching transactions
     *  found" warning instead of a table. */
    bool no_data = false;
};

/** The Income and GST Statement ("Income and GST Statement",
 *  income-gst-statement.scm): one row per transaction that touches a sales or
 *  purchases account in the period, with the date, num, description and
 *  memo/notes, then one calculated column per heading.  Each column sums the
 *  transaction's splits in a set of accounts -- net sales (income, negated),
 *  tax on sales (liability tax accounts, negated), net purchases (expense),
 *  tax on purchases (asset tax accounts) -- and the gross/net/tax-payable
 *  columns combine those sums, exactly as gst-calculated-cells does.  A
 *  column with no matching split prints blank, not zero.
 *
 *  The rows are the trep engine's: the splits of the sales and purchases
 *  accounts in the period, non-void and non-closing, only from transactions
 *  whose type is NONE or INVOICE (the report's own split filter), one per
 *  transaction, ordered by transaction date and then by the engine's split
 *  order.  The "Common Currency" default is forced by the report, so every
 *  amount is converted into the report currency at its transaction's date
 *  with the pricedb-nearest source.
 *
 *  The AU BAS and UK VAT formats are the same rows under different column
 *  definitions, and are reproduced; the UK format's *EUVAT* / *EUGOODS*
 *  account-description markers are honoured.  Not reproduced, because it is
 *  not part of the table: the Scheme report's tax-setup explanation, which its
 *  renderer appends only when the tax-account selection is empty -- the
 *  oracle's own rendered document does not contain it for these fixtures, so
 *  the native document matches the oracle rather than the source. */
GstReport gst_statement (Account* root, const GstOptions& options);

/* ------------------------------------------------------- tax schedule */

/** Options for the US Tax Schedule report.  The Scheme report's own options
 *  are From/To, an "Alternate Period" preset, an account selection and a
 *  handful of display switches; the report reproduces the period and scope,
 *  plus the switch that changes the rows (suppress zero values).
 *  Transaction detail is not printed: the native report presents the
 *  schedule rows and their totals, which is what the parity claim compares. */
struct TaxScheduleOptions
{
    std::optional<time64> start;
    std::optional<time64> end;
    std::vector<Account*> accounts;
    bool suppress_zero = false;
    /** "from-to" (the default), or one of the Scheme report's estimated-tax
     *  presets: 1st-est, 2nd-est, 3rd-est, 4th-est, last-year, 1st-last,
     *  2nd-last, 3rd-last, 4th-last. */
    std::string alternate_period = "from-to";
    gnc_commodity* currency = nullptr;
};

struct TaxScheduleReport
{
    CellTable table;
    std::string title;
    /** The TXF export, with the Scheme report's CRLF line endings. */
    std::string txf;
    time64 from_date = 0;
    time64 to_date = 0;
    int tax_year = 0;
    std::string entity;
};

/** The US Tax Schedule report ("Tax Schedule Report/TXF Export",
 *  taxtxf.scm): one row block per form/schedule line, then per tax-related
 *  account, with its amount for the period and the account/line/form
 *  subtotals.  Income amounts are sign-reversed (a credit prints positive),
 *  expenses keep their sign, exactly as the Scheme report's splt-rpt-amount
 *  does.  The category data (form, description, TXF format, category key and
 *  per-year line number) comes from gnc-tax-codes.cpp.
 *
 *  Deliberately not reproduced, because they are balance-sheet/TXF-detail
 *  features rather than the schedule table this port proves: transaction
 *  detail rows, the beginning/ending balance rows the Scheme report prints
 *  for asset, liability and equity accounts, the N438/N440 paired rows, and
 *  TXF formats 4 and 5 (capital gains, which need the sold/basis/sale
 *  description data).  A code with one of those formats is omitted from the
 *  TXF file rather than written with guessed fields. */
TaxScheduleReport tax_schedule (Account* root,
                                const TaxScheduleOptions& options);

/** The TXF export for the same options, for a caller that wants the file
 *  without the HTML table. */
bool tax_txf (Account* root, const TaxScheduleOptions& options,
              std::string& out);

/** Render the Tax Schedule report's TXF export from a route request (the
 *  same option surface render_report() uses).  False when `name` is not the
 *  tax report. */
struct ReportRequest;
bool render_tax_export (const std::string& name, Account* root,
                        const ReportRequest& request, std::string& out);

/** Render an aging report as a standalone HTML document, including its
 *  explanation-instead-of-a-table case. */
std::string render_aging (const AgingReport& report);

/* Report registry: names, and rendering by name.  Unknown names are left to
 * the Scheme engine (when the build has one). */
std::vector<std::string> native_report_names ();

/* --------------------------------------------------------------- options */

/** What a caller may set for a native report.  The names are the query
 *  parameters of the /report/<name> route and the keys of the JSON
 *  descriptor the option endpoint returns, so they are part of the API. */
enum class OptionType
{
    /** An absolute date, written "YYYY-MM-DD". */
    Date,
    /** An account selection: one or more account GUIDs, or "all". */
    AccountList,
    /** A commodity mnemonic the report converts into, e.g. "USD". */
    Currency,
    /** One of `choices`, e.g. a period preset. */
    Choice,
    /** Free text, e.g. the invoice report's custom title. */
    Text,
    /** A boolean, written "true" or "false". */
    Bool,
    /** A whole number between `minimum` and `maximum`, written in decimal. */
    Number,
    /** The id of an invoice in the book (or its GUID). */
    Invoice,
    /** An owner selection: the GUID of a customer, vendor, employee or job,
     *  which the report name decides. */
    Owner,
};

struct OptionDescriptor
{
    std::string name;
    OptionType type = OptionType::Date;
    /** Human label, taken from the Scheme report's own option name where it
     *  has one (e.g. "Date of Report" for the Trial Balance). */
    std::string label;
    std::string description;
    /** The default as text: an ISO date, "" for an empty account list, a
     *  commodity mnemonic, or the chosen `choices` entry. */
    std::string default_value;
    std::vector<std::string> choices;
    /** The inclusive range a Number option accepts.  Both zero means no
     *  bounds. */
    int minimum = 0;
    int maximum = 0;
};

/** The options a native report accepts, in display order.  Empty for a name
 *  the native layer does not know. */
std::vector<OptionDescriptor> report_options (const std::string& name);

/** Whether `report` accepts the option `option`.  This is what the route
 *  consults before rejecting an option a report does not have. */
bool report_has_option (const std::string& report, const std::string& option);

/** A date range resolved from an accounting-period preset.  Both bounds may
 *  be unset ("all"). */
struct DateRange
{
    std::optional<time64> start;
    std::optional<time64> end;
};

/** The period presets the native reports offer. */
std::vector<std::string> period_presets ();

/** Resolve "this-year", "last-year" or "all" against the engine's
 *  accounting-period helpers.  "this-year" is the interval
 *  gnc_accounting_period_fiscal_start()/end() report, i.e. the interval the
 *  reports' own date options default to; "last-year" is that interval one
 *  year earlier; "all" leaves both bounds unset.  Returns false for an
 *  unknown preset. */
bool resolve_period (const std::string& preset, DateRange& range);

/** How the owner reports display their history table.  The defaults are the
 *  Scheme report's: date, due date, reference, type, description, debits,
 *  credits and balance on; the job column only for the non-job reports, sale
 *  and tax off.  The "Due or Post Date" choice, the linked-transactions
 *  column and the document-link column are not reproduced. */
struct OwnerReportOptions
{
    OwnerKind kind = OwnerKind::Customer;
    /** The selected owner's GUID.  Empty is "no owner selected". */
    std::string owner_guid;
    /** The report's From/To bounds.  Unset means no bound. */
    std::optional<time64> start;
    std::optional<time64> end;
    bool show_date = true;
    bool show_due_date = true;
    bool show_reference = true;
    bool show_type = true;
    bool show_description = true;
    /** The associated job column.  The Job Report has no such option, which
     *  its descriptors express by not publishing the name. */
    bool show_job = false;
    bool show_sale = false;
    bool show_tax = false;
    bool show_debits = true;
    bool show_credits = true;
    bool show_balance = true;
};

/** The Customer Summary display switches.  Sort key and sort order stay at
 *  the Scheme defaults (customer name, ascending); income/expense account
 *  selection stays at the Scheme default (every such account in the book). */
struct CustomerSummaryOptions
{
    /** The report's From/To bounds.  Unset means no bound. */
    std::optional<time64> start;
    std::optional<time64> end;
    bool show_expense = true;
    bool show_zero_lines = false;
    bool show_inactive = false;
};

/** Dates and account selection a caller may pin instead of the report's own
 *  defaults.  Stage 3 (options over HTTP) builds on this; the parity harness
 *  is its first user, because the Scheme reports can only be asked for a
 *  period and a selection through their own options.  An empty `accounts`
 *  means the report's default selection, which for the transaction report
 *  and the general ledger is empty and therefore renders the warning.
 *
 *  `currency` is the report currency option; null means the report's
 *  default.  `all_dates` says the caller asked for no date bound at all
 *  (period=all), which is different from "use the report's default
 *  period" -- the defaults are a real period, not the whole book. */
struct ReportRequest
{
    std::optional<time64> start;
    std::optional<time64> end;
    std::vector<Account*> accounts;
    gnc_commodity* currency = nullptr;
    bool all_dates = false;
    /** The owner reports' owner selection: the GUID of a customer, vendor,
     *  employee or job (the report name decides which). */
    std::string owner;
    /** The owner reports' display switches; the defaults are the Scheme
     *  report's defaults. */
    OwnerReportOptions owner_options;
    /** The Customer Summary's display switches. */
    CustomerSummaryOptions customer_summary_options;
    /** The invoice report's "Invoice Number": the document id (or GUID) of
     *  the invoice to print.  Empty is the engine's "no invoice selected"
     *  case and renders the warning, not an error. */
    std::string invoice;
    /** The invoice report's display switches; the defaults are the Scheme
     *  report's defaults. */
    InvoicePrintOptions invoice_options;
    /** The Tax Invoice report's display switches; the Australian variant
     *  shares them and changes only the headings the renderer derives from
     *  its `australian` flag. */
    TaxInvoiceOptions tax_invoice_options;
    /** The Tax Schedule report's "Alternate Period" choice.  Empty means the
     *  report's "from-to" default. */
    std::string tax_alternate_period;
    /** The Tax Schedule report's output selection: "report" (the default)
     *  or "txf". */
    std::string tax_format;
    /** The Tax Schedule report's "Suppress $0.00 values" switch. */
    bool tax_suppress_zero = false;

    /** Options the descriptors publish but the request has no dedicated field
     *  for: a report's own choice, text and boolean switches, by descriptor
     *  name.  The route validates the name against the descriptors and, for a
     *  choice, the value against the descriptor's choices, so a report reads
     *  only names and values it published.  A report that does not find the
     *  name it wants uses its own default, which is what an omitted option
     *  means anyway. */
    std::map<std::string, std::string> choices;
    std::map<std::string, std::string> texts;
    std::map<std::string, bool> booleans;

    /** Further account selections, by descriptor name.  A report may publish
     *  more than one (the IFRS report needs five); "accounts" is the first,
     *  and stays in `accounts`. */
    std::map<std::string, std::vector<Account*>> account_lists;

    /** Read a boolean option, falling back to `fallback` when the caller did
     *  not send it. */
    bool boolean_option (const std::string& name, bool fallback) const;
    std::string text_option (const std::string& name,
                             const std::string& fallback) const;
    std::string choice_option (const std::string& name,
                               const std::string& fallback) const;
};

bool render_report (const std::string& name, Account* root,
                    const ReportRequest& request, std::string& html);

bool render_report (const std::string& name, Account* root, std::string& html);

} // namespace report
} // namespace gnc

#endif
