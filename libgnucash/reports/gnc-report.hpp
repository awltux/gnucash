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
};

struct Chart
{
    ChartType type = ChartType::Bar;
    std::string title;
    std::vector<std::string> labels;
    std::vector<gnc_numeric> values;
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
    /** Add a bar chart below the table. */
    bool chart = false;
};

/** Render a TableSpec as an HTML document.  A period-grouped spec needs both
 *  dates; without them nothing is rendered and the caller reports the
 *  problem. */
bool render_table_spec (const TableSpec& spec, Account* root,
                        std::string& html);

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
    /** A date-interval line under the title, as the Scheme transaction
     *  engine prints above its table ("01/01/2018 to 12/31/2018"). */
    std::string subtitle;
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
 *  an error -- and stays their default until a caller passes the accounts
 *  option (or "accounts=all"). */
std::string render_no_accounts (const std::string& title,
                                bool with_title_heading);

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
};

bool render_report (const std::string& name, Account* root,
                    const ReportRequest& request, std::string& html);

bool render_report (const std::string& name, Account* root, std::string& html);

} // namespace report
} // namespace gnc

#endif
