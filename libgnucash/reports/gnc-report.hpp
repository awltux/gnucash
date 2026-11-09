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
};

bool render_report (const std::string& name, Account* root,
                    const ReportRequest& request, std::string& html);

bool render_report (const std::string& name, Account* root, std::string& html);

} // namespace report
} // namespace gnc

#endif
