/********************************************************************\
 * report-spec.cpp -- designer reports, stored outside the book      *
 *                                                                   *
 * This program is free software; you can redistribute it and/or     *
 * modify it under the terms of the GNU General Public License as    *
 * published by the Free Software Foundation; either version 2 of    *
 * the License, or (at your option) any later version.               *
\********************************************************************/

#include <config.h>

#include <algorithm>
#include <initializer_list>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

#include <glib.h>
#include <glib/gstdio.h>

extern "C"
{
#include <Account.h>
#include <gnc-commodity.h>
#include <gnc-date.h>
#include <qofbook.h>
#include <guid.h>
}

#include <gnc-report.hpp>

#include "json.hpp"
#include "report-spec.hpp"

namespace web
{
namespace
{

constexpr const char* store_version = "1";

bool
fail (std::string& error, const std::string& message)
{
    error = message;
    return false;
}

/* Tags are slugs, not free text: lower case, digits and hyphens.  Trimming
 * happens before validation, so " spending " is the tag "spending". */
std::string
trimmed (const std::string& text)
{
    size_t first = 0;
    while (first < text.size () && g_ascii_isspace (text[first]))
        ++first;
    size_t last = text.size ();
    while (last > first && g_ascii_isspace (text[last - 1]))
        --last;
    return text.substr (first, last - first);
}

bool
valid_tag (const std::string& tag)
{
    if (tag.empty () || tag.size () > 24)
        return false;
    for (const char character : tag)
    {
        const bool letter = character >= 'a' && character <= 'z';
        const bool digit = character >= '0' && character <= '9';
        if (!letter && !digit && character != '-')
            return false;
    }
    return true;
}

bool
valid_iso_date (const std::string& text)
{
    if (text.size () != 10 || text[4] != '-' || text[7] != '-')
        return false;
    int digits[8] = {0};
    for (int index = 0, digit = 0; index < 10; ++index)
    {
        if (index == 4 || index == 7)
            continue;
        if (text[index] < '0' || text[index] > '9')
            return false;
        digits[digit++] = text[index] - '0';
    }
    const int year = digits[0] * 1000 + digits[1] * 100 + digits[2] * 10 +
                     digits[3];
    const int month = digits[4] * 10 + digits[5];
    const int day = digits[6] * 10 + digits[7];
    return g_date_valid_dmy (day, static_cast<GDateMonth> (month), year) != 0;
}

bool
iso_to_time64 (const std::string& text, bool end_of_day, time64& out)
{
    if (!valid_iso_date (text))
        return false;
    const int year = std::stoi (text.substr (0, 4));
    const int month = std::stoi (text.substr (5, 2));
    const int day = std::stoi (text.substr (8, 2));
    out = end_of_day ? gnc_dmy2time64_end (day, month, year)
                     : gnc_dmy2time64 (day, month, year);
    return true;
}

bool
optional_string (const json::Value& object, const char* name,
                 std::string& out, bool& present, std::string& error)
{
    present = false;
    const json::Value* value = object.find (name);
    if (value == nullptr || value->is_null ())
        return true;
    if (!value->is_string ())
        return fail (error, std::string ("'") + name + "' must be a string");
    out = value->string;
    present = true;
    return true;
}

bool
lookup_currency (Account* root, const std::string& mnemonic,
                 gnc_commodity*& commodity, std::string& error)
{
    commodity = nullptr;
    if (mnemonic.empty ())
        return true;
    if (root == nullptr)
        return fail (error, "no book is open");
    QofBook* book = gnc_account_get_book (root);
    commodity = gnc_commodity_table_lookup (
        gnc_commodity_table_get_table (book), GNC_COMMODITY_NS_CURRENCY,
        mnemonic.c_str ());
    if (commodity == nullptr)
        return fail (error, "unknown currency '" + mnemonic + "'");
    return true;
}

bool
parse_spec_value (const json::Value& value, Account* root, bool check_book,
                  ReportSpec& spec, std::string& error)
{
    if (!value.is_object ())
        return fail (error, "a report spec must be a JSON object");

    static const std::set<std::string> allowed = {
        "id", "name", "title", "description", "tags", "source", "columns",
        "split_grouping", "grouping", "period_unit", "period_mode", "start",
        "end", "date_preset", "currency", "accounts", "account_types", "chart",
        "chart_series", "builtin"};
    for (const auto& entry : value.object)
    {
        if (allowed.count (entry.first) == 0)
            return fail (error, "unknown spec field '" + entry.first + "'");
    }

    bool present = false;
    /* "builtin" is output-only: GET /api/specs marks the built-in specs with
     * it so a client can tell them apart.  Accept it on input (a client may
     * post a built-in back) but never store it: a saved spec is never
     * built in, and only the registry assigns that flag. */
    const json::Value* builtin = value.find ("builtin");
    if (builtin != nullptr && !builtin->is_null () && !builtin->is_bool ())
        return fail (error, "'builtin' must be a boolean");
    if (!optional_string (value, "id", spec.id, present, error))
        return false;

    if (!optional_string (value, "name", spec.name, present, error))
        return false;
    if (spec.name.empty ())
        return fail (error, "the spec needs a name");
    if (spec.name.size () > 80)
        return fail (error, "'name' is longer than 80 characters");

    if (!optional_string (value, "title", spec.title, present, error))
        return false;
    if (spec.title.size () > 120)
        return fail (error, "'title' is longer than 120 characters");
    if (spec.title.empty ())
        spec.title = spec.name;

    if (!optional_string (value, "description", spec.description, present,
                          error))
        return false;
    if (spec.description.size () > 240)
        return fail (error, "'description' is longer than 240 characters");

    spec.tags.clear ();
    const json::Value* tags = value.find ("tags");
    if (tags != nullptr && !tags->is_null ())
    {
        if (!tags->is_array ())
            return fail (error, "'tags' must be an array of tag names");
        std::set<std::string> seen_tags;
        for (const json::Value& entry : tags->array)
        {
            if (!entry.is_string ())
                return fail (error, "'tags' entries must be strings");
            const std::string tag = trimmed (entry.string);
            if (!valid_tag (tag))
                return fail (error, "invalid tag '" + tag +
                                     "' (expected 1-24 lowercase letters, "
                                     "digits or hyphens)");
            if (seen_tags.insert (tag).second)
                spec.tags.push_back (tag);
        }
        if (spec.tags.size () > 12)
            return fail (error, "'tags' has more than 12 entries");
    }

    /* Where the rows come from.  A spec without the field is the account
     * report it has always been, which is what lets the six built-ins and
     * every store written before this stage load unchanged. */
    spec.source = "accounts";
    if (!optional_string (value, "source", spec.source, present, error))
        return false;
    const bool split_source = spec.source == "splits";
    const bool balance_sheet_source = spec.source == "balance-sheet";
    if (!split_source && !balance_sheet_source && spec.source != "accounts")
        return fail (error, "unknown source '" + spec.source +
                             "' (expected accounts, splits or balance-sheet)");

    const json::Value* columns = value.find ("columns");
    if (balance_sheet_source)
    {
        /* The report prints two columns and the source is its table, so the
         * columns are not a choice: omit them, or give exactly the report's
         * two in the report's order.  A caller asking for a third column (or
         * another order) is asking for a different report, not a setting. */
        spec.columns.clear ();
        if (columns != nullptr && !columns->is_null ())
        {
            if (!columns->is_array ())
                return fail (error,
                             "'columns' must be an array of column names");
            for (const json::Value& entry : columns->array)
            {
                if (!entry.is_string ())
                    return fail (error, "'columns' entries must be strings");
                spec.columns.push_back (entry.string);
            }
        }
        if (spec.columns.empty ())
            spec.columns = balance_sheet_columns ();
        else if (spec.columns != balance_sheet_columns ())
            return fail (error, "'columns' is fixed on a balance-sheet spec: "
                                 "the report prints exactly 'account-name' and "
                                 "'balance', in that order, so the columns are "
                                 "not a choice");
    }
    else
    {
        if (columns == nullptr || !columns->is_array () ||
            columns->array.empty ())
            return fail (
                error, "'columns' must be a non-empty array of column names");
        if (split_source && columns->array.size () > 11)
            return fail (error, "'columns' has more than 11 entries");
        if (!split_source && columns->array.size () > 8)
            return fail (error, "'columns' has more than 8 entries");
        spec.columns.clear ();
        std::set<std::string> seen_columns;
        for (const json::Value& entry : columns->array)
        {
            if (!entry.is_string ())
                return fail (error, "'columns' entries must be strings");
            if (split_source)
            {
                gnc::report::SplitColumn column;
                if (!gnc::report::split_column_from_name (entry.string, column))
                    return fail (error, "unknown split column '" + entry.string +
                                         "' (expected date, num, description, "
                                         "memo, transfer, account, reconcile, "
                                         "debit, credit, amount or "
                                         "running-balance)");
            }
            else
            {
                gnc::report::Column column;
                if (!gnc::report::column_from_name (entry.string, column))
                    return fail (error, "unknown column '" + entry.string +
                                         "' (expected account-name, debit, credit, "
                                         "balance or subtotal)");
            }
            if (!seen_columns.insert (entry.string).second)
                return fail (error, "column '" + entry.string +
                                     "' appears more than once");
            spec.columns.push_back (entry.string);
        }
    }

    spec.grouping = "none";
    if (!optional_string (value, "grouping", spec.grouping, present, error))
        return false;
    if (spec.grouping != "none" && spec.grouping != "account-tree" &&
        spec.grouping != "period")
        return fail (error, "unknown grouping '" + spec.grouping +
                             "' (expected none, account-tree or period)");
    if (split_source && spec.grouping != "none")
        return fail (error, "a split listing has no 'grouping'; its sections "
                             "come from 'split_grouping' (none, account, "
                             "account-code or transaction)");
    if (balance_sheet_source && spec.grouping != "none")
        return fail (error, "a balance-sheet spec has no 'grouping': the "
                             "report's sections are fixed (Assets, "
                             "Liabilities, Equity)");

    spec.split_grouping = "none";
    if (!optional_string (value, "split_grouping", spec.split_grouping, present,
                          error))
        return false;
    if (!split_source && spec.split_grouping != "none")
        return fail (error, "'split_grouping' is for a spec with source "
                             "'splits'");
    if (split_source && spec.split_grouping != "none" &&
        spec.split_grouping != "account" &&
        spec.split_grouping != "account-code" &&
        spec.split_grouping != "transaction")
        return fail (error, "unknown split_grouping '" + spec.split_grouping +
                             "' (expected none, account, account-code or "
                             "transaction)");

    spec.period_unit = "year";
    if (!optional_string (value, "period_unit", spec.period_unit, present,
                          error))
        return false;
    if (spec.period_unit != "year" && spec.period_unit != "month")
        return fail (error, "unknown period_unit '" + spec.period_unit +
                             "' (expected year or month)");
    if (split_source && spec.period_unit != "year")
        return fail (error, "a split listing has no 'period_unit': its rows "
                             "are splits, not periods");
    if (balance_sheet_source && spec.period_unit != "year")
        return fail (error, "a balance-sheet spec has no 'period_unit': it is "
                             "drawn as of one date, not over periods");

    spec.period_mode = "activity";
    if (!optional_string (value, "period_mode", spec.period_mode, present,
                          error))
        return false;
    if (spec.period_mode != "activity" && spec.period_mode != "cumulative")
        return fail (error, "unknown period_mode '" + spec.period_mode +
                             "' (expected activity or cumulative)");
    if (split_source && spec.period_mode != "activity")
        return fail (error, "a split listing has no 'period_mode': its rows "
                             "are splits, not period balances");
    if (balance_sheet_source && spec.period_mode != "activity")
        return fail (error, "a balance-sheet spec has no 'period_mode': it is "
                             "drawn as of one date, not over periods");
    if (spec.period_mode == "cumulative" && spec.grouping != "period")
        return fail (error, "period_mode 'cumulative' needs grouping "
                             "'period'");

    spec.query = SpecQuery{};
    if (!optional_string (value, "start", spec.query.start, present, error))
        return false;
    if (!optional_string (value, "end", spec.query.end, present, error))
        return false;
    if (!optional_string (value, "date_preset", spec.query.date_preset,
                          present, error))
        return false;
    if (!optional_string (value, "currency", spec.query.currency, present,
                          error))
        return false;

    /* The report has one date, its "Balance Sheet Date", which the spec calls
     * 'end'.  A start date, or a preset that would resolve to a range, is not
     * a setting the report has: the route accepts a start date for range
     * symmetry and ignores it, and the spec must not do the same silently. */
    if (balance_sheet_source && !spec.query.start.empty ())
        return fail (error, "a balance-sheet spec has no 'start': the report "
                             "is drawn as of one date, 'end'");
    if (balance_sheet_source && !spec.query.date_preset.empty ())
        return fail (error, "a balance-sheet spec has no 'date_preset': it "
                             "resolves to a range, and the report is drawn as "
                             "of one date, 'end'");

    if (!spec.query.date_preset.empty () &&
        (!spec.query.start.empty () || !spec.query.end.empty ()))
        return fail (error, "'date_preset' cannot be combined with 'start' "
                             "or 'end'");

    if (!spec.query.date_preset.empty () &&
        spec.query.date_preset != "this-year" &&
        spec.query.date_preset != "last-year" &&
        spec.query.date_preset != "all")
        return fail (error, "unknown date_preset '" + spec.query.date_preset +
                             "' (expected this-year, last-year or all)");

    time64 start = 0;
    time64 end = 0;
    if (!spec.query.start.empty () &&
        !iso_to_time64 (spec.query.start, false, start))
        return fail (error, "invalid start date '" + spec.query.start +
                             "' (expected YYYY-MM-DD)");
    if (!spec.query.end.empty () &&
        !iso_to_time64 (spec.query.end, true, end))
        return fail (error, "invalid end date '" + spec.query.end +
                             "' (expected YYYY-MM-DD)");
    if (!spec.query.start.empty () && !spec.query.end.empty () && start > end)
        return fail (error, "'start' is after 'end'");

    if (spec.grouping == "period")
    {
        /* date_preset "all" is allowed: the renderer resolves it to the
         * range the scoped transactions cover, which is what lets a built-in
         * period report work in every book. */
        if (spec.query.date_preset.empty () &&
            (spec.query.start.empty () || spec.query.end.empty ()))
            return fail (error, "a period-grouped report needs both a start "
                                 "and an end date, or date_preset 'all'");
    }

    if (check_book)
    {
        gnc_commodity* commodity = nullptr;
        if (!lookup_currency (root, spec.query.currency, commodity, error))
            return false;
    }

    spec.query.accounts.clear ();
    const json::Value* accounts = value.find ("accounts");
    if (accounts != nullptr && !accounts->is_null ())
    {
        if (!accounts->is_array ())
            return fail (error, "'accounts' must be an array of account GUIDs");
        if (accounts->array.size () > 1000)
            return fail (error, "'accounts' has more than 1000 entries");
        std::set<std::string> seen_accounts;
        for (const json::Value& entry : accounts->array)
        {
            if (!entry.is_string ())
                return fail (error, "'accounts' entries must be strings");
            GncGUID guid;
            if (!string_to_guid (entry.string.c_str (), &guid))
                return fail (error, "invalid account GUID '" + entry.string +
                                     "'");
            if (check_book)
            {
                if (root == nullptr)
                    return fail (error, "no book is open");
                Account* account =
                    xaccAccountLookup (&guid, gnc_account_get_book (root));
                if (account == nullptr)
                    return fail (error, "unknown account GUID '" +
                                         entry.string + "'");
            }
            if (seen_accounts.insert (entry.string).second)
                spec.query.accounts.push_back (entry.string);
        }
    }

    spec.account_types.clear ();
    const json::Value* account_types = value.find ("account_types");
    if (account_types != nullptr && !account_types->is_null ())
    {
        if (!account_types->is_array ())
            return fail (error, "'account_types' must be an array of account "
                                 "type names");
        if (account_types->array.size () > 16)
            return fail (error, "'account_types' has more than 16 entries");
        std::set<std::string> seen_types;
        for (const json::Value& entry : account_types->array)
        {
            if (!entry.is_string ())
                return fail (error, "'account_types' entries must be strings");
            GNCAccountType type;
            if (!gnc::report::account_type_from_name (entry.string, type))
                return fail (error, "unknown account type '" + entry.string +
                                     "' (expected BANK, CASH, ASSET, "
                                     "LIABILITY, STOCK, MUTUAL, CURRENCY, "
                                     "INCOME, EXPENSE, EQUITY, RECEIVABLE, "
                                     "PAYABLE or TRADING)");
            if (seen_types.insert (entry.string).second)
                spec.account_types.push_back (entry.string);
        }
    }

    if (balance_sheet_source && !spec.query.accounts.empty ())
        return fail (error, "a balance-sheet spec covers the whole chart of "
                             "accounts, as the report does, so it has no "
                             "'accounts'");
    if (balance_sheet_source && !spec.account_types.empty ())
        return fail (error, "a balance-sheet spec covers the whole chart of "
                             "accounts, as the report does, so it has no "
                             "'account_types'");

    if (split_source && spec.split_grouping == "transaction")
    {
        /* The General Journal's layout is the whole book's journal: the
         * native report has no account, type or date option, so a spec for it
         * has no scope either. */
        if (!spec.query.accounts.empty ())
            return fail (error, "a transaction-grouped split listing covers "
                                 "the whole book, as the General Journal does, "
                                 "so it has no 'accounts'");
        if (!spec.account_types.empty ())
            return fail (error, "a transaction-grouped split listing covers "
                                 "the whole book, as the General Journal does, "
                                 "so it has no 'account_types'");
        if (!spec.query.start.empty () || !spec.query.end.empty () ||
            !spec.query.date_preset.empty ())
            return fail (error, "a transaction-grouped split listing covers "
                                 "the whole book, as the General Journal does, "
                                 "so it has no date range");
    }

    spec.chart.clear ();
    if (!optional_string (value, "chart", spec.chart, present, error))
        return false;
    if (!spec.chart.empty () && spec.chart != "bar" && spec.chart != "line")
        return fail (error, "unknown chart '" + spec.chart +
                             "' (expected bar or line)");
    if (split_source && !spec.chart.empty ())
        return fail (error, "a split listing has no chart: a chart plots "
                             "account or period amounts");
    if (balance_sheet_source && !spec.chart.empty ())
        return fail (error, "a balance-sheet spec has no 'chart': the report "
                             "always prints its own chart of the three section "
                             "totals");

    spec.chart_series.clear ();
    if (split_source || balance_sheet_source)
    {
        const json::Value* series = value.find ("chart_series");
        if (series != nullptr && !series->is_null () && series->is_array () &&
            !series->array.empty ())
            return fail (error, balance_sheet_source
                                      ? "a balance-sheet spec has no "
                                        "'chart_series': the report always "
                                        "prints its own chart of the three "
                                        "section totals"
                                      : "a split listing has no chart_series: "
                                        "a chart plots account or period "
                                        "amounts");
    }
    const json::Value* series = value.find ("chart_series");
    if (series != nullptr && !series->is_null ())
    {
        if (!series->is_array ())
            return fail (error, "'chart_series' must be an array of amount "
                                 "column names");
        if (series->array.size () > 3)
            return fail (error, "'chart_series' has " +
                                 std::to_string (series->array.size ()) +
                                 " entries; a chart draws one, two or three "
                                 "series");
        std::set<std::string> seen_series;
        for (const json::Value& entry : series->array)
        {
            if (!entry.is_string ())
                return fail (error, "'chart_series' entries must be strings");
            if (entry.string != "debit" && entry.string != "credit" &&
                entry.string != "balance")
                return fail (error, "chart series '" + entry.string +
                                     "' is not an amount column (expected "
                                     "debit, credit or balance)");
            if (!seen_series.insert (entry.string).second)
                return fail (error, "chart series '" + entry.string +
                                     "' appears more than once");
            bool shown = false;
            for (const std::string& column : spec.columns)
                shown = shown || column == entry.string;
            if (!shown)
                return fail (error, "chart series '" + entry.string +
                                     "' is not one of the report's columns");
            spec.chart_series.push_back (entry.string);
        }
    }

    if (!spec.chart.empty () && spec.chart_series.empty ())
    {
        /* The chart plots the amount columns the report already prints:
         * both sides when both are shown, else the one amount column. */
        bool debit = false;
        bool credit = false;
        bool balance = false;
        for (const std::string& column : spec.columns)
        {
            debit = debit || column == "debit";
            credit = credit || column == "credit";
            balance = balance || column == "balance";
        }
        if (debit && credit)
        {
            spec.chart_series = {"debit", "credit"};
        }
        else if (balance)
        {
            spec.chart_series = {"balance"};
        }
        else if (debit)
        {
            spec.chart_series = {"debit"};
        }
        else if (credit)
        {
            spec.chart_series = {"credit"};
        }
        else
        {
            /* A store written by an older designer may carry a chart over a
             * report with no amount column: it rendered no chart then, so
             * load it without one instead of making the whole store
             * unreadable.  A new preview or save is still rejected. */
            if (check_book)
                return fail (error, "a chart needs a numeric column: add "
                                     "'debit', 'credit' or 'balance' to "
                                     "'columns' (or name one in "
                                     "'chart_series')");
        }
    }

    if (spec.chart_series.size () > 1)
    {
        bool debit = false;
        bool credit = false;
        bool balance = false;
        for (const std::string& name : spec.chart_series)
        {
            debit = debit || name == "debit";
            credit = credit || name == "credit";
            balance = balance || name == "balance";
        }
        if (balance)
        {
            /* Balance is the net of the two sides.  Plotting it beside just
             * one of them would draw a total and its own part as independent
             * series.  The complete trio is different: on a period report the
             * balance is that period's net, so income/expense/net-profit (or
             * assets/liabilities/net-worth) is one chart, not a total beside
             * its parts.  A flat or account-tree row's balance is the account
             * balance, not a period net, so the trio means nothing there. */
            const bool trio = debit && credit && balance &&
                              spec.chart_series.size () == 3;
            if (!trio)
                return fail (error, "a chart cannot mix the 'balance' series "
                                     "with 'debit' or 'credit' unless it draws "
                                     "all three");
            if (spec.grouping != "period")
                return fail (error, "the three-series chart (debit, credit "
                                     "and balance) needs grouping 'period'; "
                                     "on a flat or account-tree report the "
                                     "balance is not a period net");
        }
    }

    if (spec.chart.empty () && !spec.chart_series.empty ())
        return fail (error, "'chart_series' needs a chart type in 'chart'");

    return true;
}

std::string
json_field (const char* name, const std::string& value)
{
    return std::string ("\"") + name + "\":" + json::quote (value);
}

} // namespace

const std::vector<std::string>&
balance_sheet_columns ()
{
    static const std::vector<std::string> columns = {"account-name",
                                                     "balance"};
    return columns;
}

std::string
spec_store_path ()
{
    const char* override_path = g_getenv ("GNC_WEBUI_REPORTS");
    if (override_path != nullptr && override_path[0] != '\0')
        return override_path;

    const char* xdg = g_getenv ("XDG_DATA_HOME");
    if (xdg != nullptr && xdg[0] != '\0')
        return std::string (xdg) + "/gnucash/webui/reports.json";

    const char* home = g_getenv ("HOME");
    return std::string (home != nullptr ? home : ".") +
           "/.local/share/gnucash/webui/reports.json";
}

bool
load_specs (std::vector<ReportSpec>& specs, std::string& error)
{
    specs.clear ();
    const std::string path = spec_store_path ();
    if (!g_file_test (path.c_str (), G_FILE_TEST_EXISTS))
        return true;

    gchar* contents = nullptr;
    gsize length = 0;
    if (!g_file_get_contents (path.c_str (), &contents, &length, nullptr))
        return fail (error, "cannot read " + path);
    const std::string text (contents, length);
    g_free (contents);
    if (text.empty ())
        return true; /* an empty store is an empty list */

    json::Value root;
    if (!json::parse (text, root, error))
    {
        error = path + ": " + error;
        return false;
    }
    if (!root.is_object ())
        return fail (error, path + ": the report store must be a JSON object");

    const json::Value* version = root.find ("version");
    if (version != nullptr && (!version->is_string () ||
                               version->string != store_version))
        return fail (error, path + ": unsupported report store version");

    const json::Value* list = root.find ("specs");
    if (list == nullptr || !list->is_array ())
        return fail (error, path + ": 'specs' must be an array");

    for (const json::Value& entry : list->array)
    {
        ReportSpec spec;
        std::string reason;
        if (!parse_spec_value (entry, nullptr, false, spec, reason))
            return fail (error, path + ": invalid saved report: " + reason);
        if (spec.id.empty ())
            return fail (error, path + ": a saved report has no id");
        specs.push_back (std::move (spec));
    }
    return true;
}

bool
save_specs (const std::vector<ReportSpec>& specs, std::string& error)
{
    const std::string path = spec_store_path ();
    gchar* dir = g_path_get_dirname (path.c_str ());
    if (g_mkdir_with_parents (dir, 0700) != 0)
    {
        const std::string message = "cannot create " + std::string (dir);
        g_free (dir);
        return fail (error, message);
    }
    g_free (dir);

    std::ostringstream out;
    out << "{\"version\":\"" << store_version << "\",\"specs\":[";
    for (size_t index = 0; index < specs.size (); ++index)
    {
        if (index > 0)
            out << ",";
        out << spec_to_json (specs[index]);
    }
    out << "]}\n";

    const std::string temporary = path + ".tmp";
    GError* gerror = nullptr;
    if (!g_file_set_contents (temporary.c_str (), out.str ().c_str (),
                              static_cast<gssize> (out.str ().size ()),
                              &gerror))
    {
        error = "cannot write " + temporary;
        if (gerror != nullptr)
        {
            error += ": ";
            error += gerror->message;
            g_error_free (gerror);
        }
        return false;
    }
    if (g_rename (temporary.c_str (), path.c_str ()) != 0)
    {
        error = "cannot replace " + path;
        g_unlink (temporary.c_str ());
        return false;
    }
    return true;
}

bool
spec_from_json (const std::string& body, Account* root, ReportSpec& spec,
                std::string& error)
{
    json::Value value;
    if (!json::parse (body, value, error))
        return false;
    spec = ReportSpec{};
    return parse_spec_value (value, root, true, spec, error);
}

std::string
spec_to_json (const ReportSpec& spec, bool builtin)
{
    std::ostringstream out;
    out << "{";
    if (builtin)
        out << "\"builtin\":true,";
    out << json_field ("id", spec.id) << ",";
    out << json_field ("name", spec.name) << ",";
    out << json_field ("title", spec.title) << ",";
    out << json_field ("description", spec.description) << ",";
    out << "\"tags\":[";
    for (size_t index = 0; index < spec.tags.size (); ++index)
    {
        if (index > 0)
            out << ",";
        out << json::quote (spec.tags[index]);
    }
    out << "],";
    out << json_field ("source", spec.source) << ",";
    out << "\"columns\":[";
    for (size_t index = 0; index < spec.columns.size (); ++index)
    {
        if (index > 0)
            out << ",";
        out << json::quote (spec.columns[index]);
    }
    out << "],";
    if (spec.source == "splits")
        out << json_field ("split_grouping", spec.split_grouping) << ",";
    out << json_field ("grouping", spec.grouping) << ",";
    out << json_field ("period_unit", spec.period_unit) << ",";
    out << json_field ("period_mode", spec.period_mode) << ",";
    out << json_field ("start", spec.query.start) << ",";
    out << json_field ("end", spec.query.end) << ",";
    out << json_field ("date_preset", spec.query.date_preset) << ",";
    out << json_field ("currency", spec.query.currency) << ",";
    out << "\"account_types\":[";
    for (size_t index = 0; index < spec.account_types.size (); ++index)
    {
        if (index > 0)
            out << ",";
        out << json::quote (spec.account_types[index]);
    }
    out << "],";
    out << "\"accounts\":[";
    for (size_t index = 0; index < spec.query.accounts.size (); ++index)
    {
        if (index > 0)
            out << ",";
        out << json::quote (spec.query.accounts[index]);
    }
    out << "],";
    out << json_field ("chart", spec.chart) << ",";
    out << "\"chart_series\":[";
    for (size_t index = 0; index < spec.chart_series.size (); ++index)
    {
        if (index > 0)
            out << ",";
        out << json::quote (spec.chart_series[index]);
    }
    out << "]}";
    return out.str ();
}

namespace
{

ReportSpec
builtin_base (const char* id, const char* name, const char* description)
{
    ReportSpec spec;
    spec.id = id;
    spec.name = name;
    spec.title = name;
    spec.description = description;
    return spec;
}

} // namespace

const std::vector<ReportSpec>&
builtin_specs ()
{
    /* Every built-in scopes by account type rather than by GUID, so the same
     * spec is meaningful in every book; the period ones take their dates from
     * the book's own transactions for the same reason. */
    static const std::vector<ReportSpec> specs = [] ()
    {
        std::vector<ReportSpec> all;

        ReportSpec income_expenses = builtin_base (
            "builtin-income-expenses-month", "Income vs Expenses by Month",
            "Built-in, scoped to Income and Expense accounts: one point per "
            "month over the range the book's transactions cover. Credit is "
            "income, Debit is expenses.");
        income_expenses.columns = {"account-name", "debit", "credit"};
        income_expenses.tags = {"income", "spending", "trend", "period"};
        income_expenses.grouping = "period";
        income_expenses.period_unit = "month";
        income_expenses.query.date_preset = "all";
        income_expenses.account_types = {"INCOME", "EXPENSE"};
        income_expenses.chart = "line";
        income_expenses.chart_series = {"credit", "debit"};
        all.push_back (std::move (income_expenses));

        ReportSpec expenses = builtin_base (
            "builtin-expenses-by-account", "Expenses by Account",
            "Built-in, scoped to Expense accounts over all dates: one bar per "
            "expense account with activity.");
        expenses.columns = {"account-name", "debit"};
        expenses.tags = {"spending", "breakdown"};
        expenses.query.date_preset = "all";
        expenses.account_types = {"EXPENSE"};
        expenses.chart = "bar";
        expenses.chart_series = {"debit"};
        all.push_back (std::move (expenses));

        ReportSpec income = builtin_base (
            "builtin-income-by-account", "Income by Account",
            "Built-in, scoped to Income accounts over all dates: one bar per "
            "income account with activity.");
        income.columns = {"account-name", "credit"};
        income.tags = {"income", "breakdown"};
        income.query.date_preset = "all";
        income.account_types = {"INCOME"};
        income.chart = "bar";
        income.chart_series = {"credit"};
        all.push_back (std::move (income));

        ReportSpec net_worth = builtin_base (
            "builtin-net-worth-month", "Net Worth by Month",
            "Built-in, scoped to asset and liability accounts (bank, cash, "
            "card, stock, A/R and A/P): each point is the balance at that "
            "month's end, so the line is net worth, not the month's change.");
        net_worth.columns = {"account-name", "balance"};
        net_worth.tags = {"net-worth", "trend"};
        net_worth.grouping = "period";
        net_worth.period_unit = "month";
        net_worth.period_mode = "cumulative";
        net_worth.query.date_preset = "all";
        net_worth.account_types = {"BANK",  "CASH",   "ASSET",
                                   "STOCK", "MUTUAL", "CURRENCY",
                                   "CREDIT", "LIABILITY", "RECEIVABLE",
                                   "PAYABLE"};
        net_worth.chart = "line";
        net_worth.chart_series = {"balance"};
        all.push_back (std::move (net_worth));

        /* The two Scheme net-worth charts draw three series: the asset side,
         * the liability side and their net.  A period report keeps the two
         * sides separately (see spec_period_rows), so a cumulative period
         * chart can draw all three: the same figures the Scheme report plots
         * at its month steps.  The series are the balance signs, not the
         * account types; on a book whose asset balances are all non-negative
         * and liability balances non-positive (the normal case) the two
         * agree, and the description says so. */
        for (const auto& [id, name, chart] :
             std::vector<std::tuple<const char*, const char*, const char*>>{
                 {"builtin-net-worth-barchart", "Net Worth Barchart", "bar"},
                 {"builtin-net-worth-linechart", "Net Worth Linechart",
                  "line"}})
        {
            ReportSpec spec = builtin_base (
                id, name,
                "Built-in, scoped to asset and liability accounts: one point "
                "per month over the range the book's transactions cover. "
                "Debit is the positive-balance side, Credit the negative "
                "side, Balance net worth.");
            spec.columns = {"account-name", "debit", "credit", "balance"};
            spec.tags = {"net-worth", std::string (chart) + "-chart"};
            spec.grouping = "period";
            spec.period_unit = "month";
            spec.period_mode = "cumulative";
            spec.query.date_preset = "all";
            spec.account_types = {"BANK",  "CASH",   "ASSET",
                                  "STOCK", "MUTUAL", "CURRENCY",
                                  "CREDIT", "LIABILITY", "RECEIVABLE",
                                  "PAYABLE"};
            spec.chart = chart;
            spec.chart_series = {"debit", "credit", "balance"};
            all.push_back (std::move (spec));
        }

        return all;
    }();
    return specs;
}

std::vector<std::string>
builtin_spec_names ()
{
    std::vector<std::string> names;
    for (const ReportSpec& spec : builtin_specs ())
        names.push_back (spec.name);
    return names;
}

bool
builtin_spec_by_id (const std::string& id, ReportSpec& spec)
{
    for (const ReportSpec& candidate : builtin_specs ())
    {
        if (candidate.id == id)
        {
            spec = candidate;
            return true;
        }
    }
    return false;
}

/* ------------------------------------------- native reports and the designer */

namespace
{

/* The value the native route would use for an option the caller leaves out.
 * Dates arrive as ISO strings and the currency as a mnemonic, so this is
 * exactly what a spec field would hold. */
std::string
option_default (const std::vector<gnc::report::OptionDescriptor>& options,
                const char* name)
{
    for (const auto& option : options)
    {
        if (option.name == name)
            return option.default_value;
    }
    return "";
}

/* A report family: the C++ file and entry point that build it. */
struct NativeSource
{
    const char* file;
    const char* symbol;
};

NativeSource
native_source (const std::string& name)
{
    struct Entry
    {
        const char* report;
        const char* file;
        const char* symbol;
    };
    /* One row per native report, so the designer panel can name the code the
     * report really is.  A name the native layer does not know never reaches
     * here (native_designer_info() rejects it), but render_report() is the
     * honest fallback for a report added without a row. */
    static const Entry entries[] = {
        {"Trial Balance", "libgnucash/reports/gnc-report.cpp",
         "trial_balance()"},
        {"Balance Sheet", "libgnucash/reports/gnc-report.cpp",
         "balance_sheet()"},
        {"Income Statement", "libgnucash/reports/gnc-report.cpp",
         "income_statement()"},
        {"Profit & Loss", "libgnucash/reports/gnc-report.cpp",
         "income_statement()"},
        {"Balance Sheet (Multicolumn)", "libgnucash/reports/gnc-report.cpp",
         "balance_sheet_multicolumn()"},
        {"Income Statement (Multicolumn)", "libgnucash/reports/gnc-report.cpp",
         "income_statement_multicolumn()"},
        {"Equity Statement", "libgnucash/reports/gnc-report.cpp",
         "equity_statement()"},
        {"Transaction Report", "libgnucash/reports/gnc-report.cpp",
         "transaction_report()"},
        {"General Ledger", "libgnucash/reports/gnc-report.cpp",
         "general_ledger()"},
        {"General Journal", "libgnucash/reports/gnc-report.cpp",
         "general_journal()"},
        {"Transaction Breakdown Report", "libgnucash/reports/gnc-report.cpp",
         "transaction_breakdown()"},
        {"Reconciliation Report", "libgnucash/reports/gnc-report.cpp",
         "reconciliation_report()"},
        {"Receivable Aging", "libgnucash/reports/gnc-report.cpp",
         "receivable_aging()"},
        {"Payable Aging", "libgnucash/reports/gnc-report.cpp",
         "payable_aging()"},
        {"Investment Lots", "libgnucash/reports/gnc-report.cpp",
         "investment_lots()"},
        {"Investment Portfolio", "libgnucash/reports/gnc-report.cpp",
         "investment_portfolio()"},
        {"Advanced Portfolio", "libgnucash/reports/gnc-report.cpp",
         "advanced_portfolio()"},
        {"Lot Viewer", "libgnucash/reports/gnc-report.cpp", "lot_viewer()"},
        {"IFRS weighted-average cost basis report",
         "libgnucash/reports/gnc-report.cpp", "ifrs_cost_basis()"},
        {"Budget Report", "libgnucash/reports/gnc-report.cpp",
         "budget_report()"},
        {"Budget Income Statement", "libgnucash/reports/gnc-report.cpp",
         "budget_income_statement()"},
        {"Budget Profit & Loss", "libgnucash/reports/gnc-report.cpp",
         "budget_income_statement()"},
        {"Budget Balance Sheet", "libgnucash/reports/gnc-report.cpp",
         "budget_balance_sheet()"},
        {"Budget Flow", "libgnucash/reports/gnc-report.cpp", "budget_flow()"},
        {"Printable Invoice", "libgnucash/reports/gnc-invoice-print.cpp",
         "render_printable_invoice()"},
        {"Easy Invoice", "libgnucash/reports/gnc-invoice-print.cpp",
         "render_easy_invoice()"},
        {"Fancy Invoice", "libgnucash/reports/gnc-invoice-print.cpp",
         "render_fancy_invoice()"},
        {"Receipt", "libgnucash/reports/gnc-invoice-receipt.cpp",
         "render_receipt()"},
        {"Tax Invoice", "libgnucash/reports/gnc-invoice-tax.cpp",
         "render_tax_invoice()"},
        {"Australian Tax Invoice", "libgnucash/reports/gnc-invoice-tax.cpp",
         "render_tax_invoice()"},
        {"Tax Schedule Report/TXF Export", "libgnucash/reports/gnc-report.cpp",
         "tax_schedule(), render_tax_export()"},
        {"Customer Report", "libgnucash/reports/gnc-owner.cpp",
         "owner_report()"},
        {"Vendor Report", "libgnucash/reports/gnc-owner.cpp",
         "owner_report()"},
        {"Employee Report", "libgnucash/reports/gnc-owner.cpp",
         "owner_report()"},
        {"Job Report", "libgnucash/reports/gnc-owner.cpp", "owner_report()"},
        {"Customer Summary", "libgnucash/reports/gnc-owner.cpp",
         "customer_summary()"},
        {"Cash Flow", "libgnucash/reports/gnc-report.cpp", "cash_flow()"},
        {"Cash Flow Barchart", "libgnucash/reports/gnc-report.cpp",
         "cash_flow_barchart()"},
        {"Income and GST Statement", "libgnucash/reports/gnc-report.cpp",
         "gst_statement()"},
    };
    for (const Entry& entry : entries)
    {
        if (name == entry.report)
            return {entry.file, entry.symbol};
    }
    return {"libgnucash/reports/gnc-report.cpp", "render_report()"};
}

bool
one_of (const std::string& name, std::initializer_list<const char*> names)
{
    for (const char* candidate : names)
    {
        if (name == candidate)
            return true;
    }
    return false;
}

/* Why the designer has nothing to populate.  Each family names the thing the
 * report has that a spec row cannot hold; no family pretends a field is
 * missing rather than a different report. */
std::string
no_spec_note (const std::string& name)
{
    if (one_of (name, {"Transaction Report", "General Ledger",
                       "General Journal"}))
    {
        return "It lists transactions and splits with columns that belong to "
               "that listing (date, number, description, transfer, running "
               "balance).  A split spec (source 'splits') can reproduce its "
               "cells, but the designer has no portable starting point: the "
               "Transaction Report and the General Ledger take an account "
               "selection, and a selection is a list of this book's account "
               "GUIDs, not a description that fits every book.";
    }
    if (name == "Transaction Breakdown Report")
    {
        return "It lists transactions and splits, but its columns are not a "
               "fixed vocabulary: it has one column per account the period's "
               "transactions touch, headed by a multi-row account-tree header, "
               "with a per-account total row.  A spec's columns are a fixed "
               "list, so it cannot describe the dynamic account columns.";
    }
    if (name == "Reconciliation Report")
    {
        return "It lists transactions and splits, sorted into reconciled "
               "status sections, with the reconciled-date inclusion rule and "
               "the calculated Funds In/Funds Out columns.  A split spec has "
               "no status section and no calculated column, so it cannot "
               "describe the report.";
    }
    if (one_of (name, {"Receivable Aging", "Payable Aging"}))
    {
        return "It groups invoices and payments into aging buckets from their "
               "due or post dates.  A spec's rows are accounts or periods, so "
               "it cannot describe the buckets or the owner rows.";
    }
    if (one_of (name, {"Investment Lots", "Investment Portfolio",
                       "Advanced Portfolio", "Lot Viewer",
                       "IFRS weighted-average cost basis report"}))
    {
        return "Its rows and amounts come from lots, holdings, prices and "
               "their gains.  A spec has no lot, price or gain column, so it "
               "cannot describe the report.";
    }
    if (name == "Balance Sheet (Multicolumn)")
    {
        return "It prints the Balance Sheet's sections, sign reversal and "
               "retained-earnings line again, but as one amount column per "
               "period end: each row carries a value per date, the column "
               "headings are dates, and the single-column form of the report "
               "is not the Balance Sheet either (its headings are 'Asset', "
               "'Total For Asset' and so on, and a parent account prints its "
               "subtree and then its own balance on two rows).  The table "
               "model already has the grouped column axis the report uses "
               "(column_headings with one Row::values entry per column), but "
               "a spec has no field for it: 'period_unit' groups rows by "
               "period, and no field asks for a period column axis.  "
               "Expressing it therefore needs a new spec field (the report's "
               "'Period duration' and 'Period order is most recent first') "
               "and the multicolumn row rules, which is a second source, not "
               "a setting on this one.";
    }
    if (name == "Equity Statement")
    {
        return "It is an equity roll-forward, not a balance list: opening "
               "capital, the period's net income or loss, investments, "
               "withdrawals and closing capital, each derived from the equity, "
               "income and expense accounts over two dates.  No spec source "
               "produces those rows; an account spec's row is one account's "
               "own amount over a date range, with no opening balance and no "
               "capital-movement line.";
    }
    if (name == "Income Statement (Multicolumn)")
    {
        return "It is the Income Statement's rows with one amount column per "
               "period end: income sign-reversed, a Net Income block, and the "
               "same rows across the columns.  The table model's grouped "
               "column axis (column_headings and Row::values) renders that "
               "shape, but a spec has no field asking for a period column "
               "axis -- 'period_unit' groups rows by period -- and an account "
               "spec's rows carry no sections or sign reversal, so it cannot "
               "be described as a spec today.";
    }
    if (name == "Budget Report" || name == "Budget Income Statement" ||
        name == "Budget Profit & Loss" || name == "Budget Balance Sheet" ||
        name == "Budget Flow")
    {
        return "Its amounts come from the book's budget object and budget "
               "periods, not from splits in a date range.  A spec queries "
               "splits, so it cannot describe a budget value.";
    }
    if (one_of (name, {"Printable Invoice", "Easy Invoice", "Fancy Invoice",
                       "Receipt", "Tax Invoice", "Australian Tax Invoice",
                       "Customer Report", "Vendor Report", "Employee Report",
                       "Job Report", "Customer Summary"}))
    {
        return "It prints a document built from an invoice or an owner "
               "object, with the layout's own headings and totals.  A spec's "
               "rows are account amounts, so it cannot describe the document.";
    }
    if (name == "Tax Schedule Report/TXF Export")
    {
        return "It reads the book's tax codes and can also emit a TXF file.  "
               "A spec has no tax-code row or column and cannot produce the "
               "export.";
    }
    if (name == "Cash Flow" || name == "Cash Flow Barchart")
    {
        return "It classifies splits into cash-flow categories and draws a "
               "selected-account table.  A spec has no category column, so it "
               "cannot describe the report.";
    }
    if (name == "Income and GST Statement")
    {
        return "It sums the book's tax codes into GST collected and paid.  A "
               "spec has no tax-code column, so it cannot describe the "
               "statement.";
    }
    return "Its query, columns and totals are written in C++; the designer's "
           "vocabulary (account rows, debit/credit/balance columns and period "
           "grouping) cannot describe them.";
}

/* Fill the starting point's dates and currency from the native descriptors,
 * so it runs over the same period, in the same commodity, the report itself
 * uses by default. */
void
apply_default_dates (ReportSpec& spec,
                     const std::vector<gnc::report::OptionDescriptor>& options)
{
    spec.query.start = option_default (options, "start");
    spec.query.end = option_default (options, "end");
    spec.query.currency = option_default (options, "currency");
}

} // namespace

bool
native_designer_info (const std::string& report, NativeDesignerInfo& info)
{
    const auto options = gnc::report::report_options (report);
    if (options.empty ())
        return false;

    info = NativeDesignerInfo{};
    info.report = report;
    const NativeSource source = native_source (report);
    info.source_file = source.file;
    info.source_symbol = source.symbol;
    info.equivalence = "none";

    if (report == "Trial Balance")
    {
        /* The one report whose C++ table is exactly a spec: a flat row per
         * account over the same account types, Debit and Credit columns and
         * the same as-of date.  The copy has its own name and title, but its
         * cells are the report's cells. */
        ReportSpec spec;
        spec.name = report + " (copy)";
        spec.title = spec.name;
        spec.columns = {"account-name", "debit", "credit"};
        spec.grouping = "none";
        spec.period_unit = "year";
        spec.period_mode = "activity";
        apply_default_dates (spec, options);
        /* The C++ trial balance ignores its start date and shows every
         * account type below the root; clearing the start (which the
         * descriptor defaults to today) is exactly that query. */
        spec.query.start.clear ();
        info.spec = std::move (spec);
        info.has_spec = true;
        info.equivalence = "exact";
        info.note =
            "A flat row per account with Debit and Credit columns over the "
            "same as-of date, which is the C++ trial balance's own table.  The "
            "copy has its own name and title; its cells are the report's "
            "cells.";
        return true;
    }

    if (report == "Balance Sheet")
    {
        /* The source is the report's own table, so the only things that carry
         * over are the as-of date and the commodity.  The fixed columns are
         * the report's two, and the copy gets its own name and title. */
        ReportSpec spec;
        spec.name = report + " (copy)";
        spec.title = spec.name;
        spec.source = "balance-sheet";
        spec.columns = {"account-name", "balance"};
        spec.query.end = option_default (options, "end");
        spec.query.currency = option_default (options, "currency");
        info.spec = std::move (spec);
        info.has_spec = true;
        info.equivalence = "exact";
        info.note =
            "Its cells are the report's own: the Assets, Liabilities and "
            "Equity sections in the report's order, one row per account as of "
            "the Balance Sheet Date with liabilities and equity "
            "sign-reversed, the income and expense balances folded into one "
            "Retained Earnings line inside equity, and the report's totals.  "
            "The structure is the report's, not a choice: a balance-sheet "
            "spec has no start date, grouping, column selection, account "
            "scope or chart option, and the report's chart of the three "
            "section totals is always printed.  The copy has its own name and "
            "title; its cells are the report's cells.";
        return true;
    }

    if (report == "Income Statement" || report == "Profit & Loss")
    {
        /* Close, and labelled as approximate: the accounts and the period are
         * the report's, but the columns and the row structure are not. */
        ReportSpec spec;
        spec.name = report + " (starting point)";
        spec.title = spec.name;
        spec.columns = {"account-name", "debit", "credit"};
        spec.grouping = "none";
        spec.period_unit = "year";
        spec.period_mode = "activity";
        spec.account_types = {"INCOME", "EXPENSE"};
        apply_default_dates (spec, options);
        info.spec = std::move (spec);
        info.has_spec = true;
        info.equivalence = "approximate";
        info.note =
            "It lists the same Income and Expense accounts over the report's "
            "default period, but as a flat Debit/Credit list: the C++ report "
            "prints one sign-reversed Balance column, groups the rows under "
            "Revenues and Expenses, adds a subtotal for every parent and a Net "
            "income line.  The accounts and period are the report's; the "
            "columns, signs and totals are approximate.";
        return true;
    }

    info.note = no_spec_note (report);
    return true;
}

std::string
native_designer_json (const NativeDesignerInfo& info)
{
    std::ostringstream out;
    out << "{\"report\":" << json::quote (info.report);
    out << ",\"equivalence\":" << json::quote (info.equivalence);
    out << ",\"note\":" << json::quote (info.note);
    out << ",\"source\":{\"file\":" << json::quote (info.source_file)
        << ",\"symbol\":" << json::quote (info.source_symbol) << "}";
    out << ",\"spec\":";
    if (info.has_spec)
        out << spec_to_json (info.spec);
    else
        out << "null";
    out << "}";
    return out.str ();
}

namespace
{

std::string
folded (const std::string& text)
{
    gchar* lower = g_utf8_strdown (text.c_str (), -1);
    if (lower == nullptr)
        return text;
    const std::string result (lower);
    g_free (lower);
    return result;
}

bool
has_tag (const ReportSpec& spec, const std::string& tag)
{
    for (const std::string& candidate : spec.tags)
    {
        if (g_ascii_strcasecmp (candidate.c_str (), tag.c_str ()) == 0)
            return true;
    }
    return false;
}

} // namespace

bool
spec_matches (const ReportSpec& spec, const std::string& tag,
              const std::string& text)
{
    if (!tag.empty () && !has_tag (spec, tag))
        return false;
    if (text.empty ())
        return true;

    const std::string needle = folded (text);
    if (folded (spec.name).find (needle) != std::string::npos ||
        folded (spec.description).find (needle) != std::string::npos)
        return true;
    for (const std::string& candidate : spec.tags)
    {
        if (folded (candidate).find (needle) != std::string::npos)
            return true;
    }
    return false;
}

std::string
specs_to_json (const std::vector<ReportSpec>& specs, const std::string& tag,
               const std::string& text)
{
    std::ostringstream out;
    out << "{\"specs\":[";
    bool first = true;
    for (const ReportSpec& spec : specs)
    {
        if (!spec_matches (spec, tag, text))
            continue;
        if (!first)
            out << ",";
        first = false;
        out << spec_to_json (spec);
    }
    out << "],\"builtin\":[";
    first = true;
    for (const ReportSpec& spec : builtin_specs ())
    {
        if (!spec_matches (spec, tag, text))
            continue;
        if (!first)
            out << ",";
        first = false;
        out << spec_to_json (spec, true);
    }
    out << "]}";
    return out.str ();
}

bool
render_spec_html (const ReportSpec& spec, Account* root, std::string& html,
                  std::string& error)
{
    if (root == nullptr)
        return fail (error, "no book is open");

    gnc::report::TableSpec table;
    table.title = spec.title.empty () ? spec.name : spec.title;
    if (spec.source == "splits")
    {
        table.source = gnc::report::SpecSource::Splits;
        for (const std::string& name : spec.columns)
        {
            gnc::report::SplitColumn column;
            if (!gnc::report::split_column_from_name (name, column))
                return fail (error, "unknown split column '" + name + "'");
            table.split_columns.push_back (column);
        }
        if (table.split_columns.empty ())
            return fail (error, "the spec has no columns");
        if (spec.split_grouping == "account")
            table.split_grouping = gnc::report::SplitGrouping::Account;
        else if (spec.split_grouping == "account-code")
            table.split_grouping = gnc::report::SplitGrouping::AccountCode;
        else if (spec.split_grouping == "transaction")
            table.split_grouping = gnc::report::SplitGrouping::Transaction;
        else
            table.split_grouping = gnc::report::SplitGrouping::None;
    }
    else if (spec.source == "balance-sheet")
    {
        /* The report's own table: its columns, sections and signs come from
         * balance_sheet_spec_table(), and the spec's own two fields are the
         * as-of date (`table.end` below) and the currency.  Nothing is copied
         * from spec.columns here, so a spec cannot select a third column even
         * by posting one the validator would accept. */
        table.source = gnc::report::SpecSource::BalanceSheet;
    }
    else
    {
        for (const std::string& name : spec.columns)
        {
            gnc::report::Column column;
            if (!gnc::report::column_from_name (name, column))
                return fail (error, "unknown column '" + name + "'");
            table.columns.push_back (column);
        }
        if (table.columns.empty ())
            return fail (error, "the spec has no columns");
    }

    if (!spec.query.date_preset.empty ())
    {
        gnc::report::DateRange range;
        if (!gnc::report::resolve_period (spec.query.date_preset, range))
            return fail (error, "unknown date_preset '" +
                                 spec.query.date_preset + "'");
        table.start = range.start;
        table.end = range.end;
    }
    else
    {
        if (!spec.query.start.empty ())
        {
            time64 when = 0;
            if (!iso_to_time64 (spec.query.start, false, when))
                return fail (error, "invalid start date '" +
                                     spec.query.start + "'");
            table.start = when;
        }
        if (!spec.query.end.empty ())
        {
            time64 when = 0;
            if (!iso_to_time64 (spec.query.end, true, when))
                return fail (error, "invalid end date '" +
                                     spec.query.end + "'");
            table.end = when;
        }
    }

    gnc_commodity* currency = nullptr;
    if (!lookup_currency (root, spec.query.currency, currency, error))
        return false;
    table.currency = currency;

    for (const std::string& text : spec.query.accounts)
    {
        GncGUID guid;
        if (!string_to_guid (text.c_str (), &guid))
            return fail (error, "invalid account GUID '" + text + "'");
        Account* account =
            xaccAccountLookup (&guid, gnc_account_get_book (root));
        if (account == nullptr)
            return fail (error, "unknown account GUID '" + text + "'");
        table.accounts.push_back (account);
    }

    for (const std::string& name : spec.account_types)
    {
        GNCAccountType type;
        if (!gnc::report::account_type_from_name (name, type))
            return fail (error, "unknown account type '" + name + "'");
        table.account_types.push_back (type);
    }

    if (spec.grouping == "account-tree")
        table.grouping = gnc::report::GroupingMode::AccountTree;
    else if (spec.grouping == "period")
        table.grouping = gnc::report::GroupingMode::Period;
    else
        table.grouping = gnc::report::GroupingMode::None;

    table.period_unit = spec.period_unit == "month"
                            ? gnc::report::PeriodUnit::Month
                            : gnc::report::PeriodUnit::Year;
    table.cumulative = spec.period_mode == "cumulative";

    table.chart = !spec.chart.empty ();
    table.chart_type = spec.chart == "line" ? gnc::report::ChartType::Line
                                            : gnc::report::ChartType::Bar;
    for (const std::string& name : spec.chart_series)
    {
        if (name != "debit" && name != "credit" && name != "balance")
            return fail (error, "chart series '" + name +
                                 "' is not an amount column");
        gnc::report::Column column;
        if (!gnc::report::column_from_name (name, column))
            return fail (error, "unknown chart series '" + name + "'");
        table.chart_columns.push_back (column);
    }

    if (!gnc::report::render_table_spec (table, root, html, &error))
    {
        if (error.empty ())
            error = "the report could not be rendered";
        return false;
    }
    return true;
}

} // namespace web
