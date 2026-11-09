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
#include <set>
#include <sstream>
#include <string>
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
        "id", "name", "title", "columns", "grouping", "period_unit",
        "start", "end", "date_preset", "currency", "accounts", "chart"};
    for (const auto& entry : value.object)
    {
        if (allowed.count (entry.first) == 0)
            return fail (error, "unknown spec field '" + entry.first + "'");
    }

    bool present = false;
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

    const json::Value* columns = value.find ("columns");
    if (columns == nullptr || !columns->is_array () || columns->array.empty ())
        return fail (error, "'columns' must be a non-empty array of column names");
    if (columns->array.size () > 8)
        return fail (error, "'columns' has more than 8 entries");
    spec.columns.clear ();
    std::set<std::string> seen_columns;
    for (const json::Value& entry : columns->array)
    {
        if (!entry.is_string ())
            return fail (error, "'columns' entries must be strings");
        gnc::report::Column column;
        if (!gnc::report::column_from_name (entry.string, column))
            return fail (error, "unknown column '" + entry.string +
                                 "' (expected account-name, debit, credit, "
                                 "balance or subtotal)");
        if (!seen_columns.insert (entry.string).second)
            return fail (error, "column '" + entry.string +
                                 "' appears more than once");
        spec.columns.push_back (entry.string);
    }

    spec.grouping = "none";
    if (!optional_string (value, "grouping", spec.grouping, present, error))
        return false;
    if (spec.grouping != "none" && spec.grouping != "account-tree" &&
        spec.grouping != "period")
        return fail (error, "unknown grouping '" + spec.grouping +
                             "' (expected none, account-tree or period)");

    spec.period_unit = "year";
    if (!optional_string (value, "period_unit", spec.period_unit, present,
                          error))
        return false;
    if (spec.period_unit != "year" && spec.period_unit != "month")
        return fail (error, "unknown period_unit '" + spec.period_unit +
                             "' (expected year or month)");

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
        if (spec.query.date_preset == "all")
            return fail (error, "a period-grouped report cannot use "
                                 "date_preset 'all'");
        if (spec.query.date_preset.empty () &&
            (spec.query.start.empty () || spec.query.end.empty ()))
            return fail (error, "a period-grouped report needs both a start "
                                 "and an end date");
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

    spec.chart.clear ();
    if (!optional_string (value, "chart", spec.chart, present, error))
        return false;
    if (!spec.chart.empty () && spec.chart != "bar")
        return fail (error, "unknown chart '" + spec.chart +
                             "' (expected bar)");

    return true;
}

std::string
json_field (const char* name, const std::string& value)
{
    return std::string ("\"") + name + "\":" + json::quote (value);
}

} // namespace

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
spec_to_json (const ReportSpec& spec)
{
    std::ostringstream out;
    out << "{";
    out << json_field ("id", spec.id) << ",";
    out << json_field ("name", spec.name) << ",";
    out << json_field ("title", spec.title) << ",";
    out << "\"columns\":[";
    for (size_t index = 0; index < spec.columns.size (); ++index)
    {
        if (index > 0)
            out << ",";
        out << json::quote (spec.columns[index]);
    }
    out << "],";
    out << json_field ("grouping", spec.grouping) << ",";
    out << json_field ("period_unit", spec.period_unit) << ",";
    out << json_field ("start", spec.query.start) << ",";
    out << json_field ("end", spec.query.end) << ",";
    out << json_field ("date_preset", spec.query.date_preset) << ",";
    out << json_field ("currency", spec.query.currency) << ",";
    out << json_field ("chart", spec.chart) << ",";
    out << "\"accounts\":[";
    for (size_t index = 0; index < spec.query.accounts.size (); ++index)
    {
        if (index > 0)
            out << ",";
        out << json::quote (spec.query.accounts[index]);
    }
    out << "]}";
    return out.str ();
}

std::string
specs_to_json (const std::vector<ReportSpec>& specs)
{
    std::ostringstream out;
    out << "{\"specs\":[";
    for (size_t index = 0; index < specs.size (); ++index)
    {
        if (index > 0)
            out << ",";
        out << spec_to_json (specs[index]);
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
    for (const std::string& name : spec.columns)
    {
        gnc::report::Column column;
        if (!gnc::report::column_from_name (name, column))
            return fail (error, "unknown column '" + name + "'");
        table.columns.push_back (column);
    }
    if (table.columns.empty ())
        return fail (error, "the spec has no columns");

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

    if (spec.grouping == "account-tree")
        table.grouping = gnc::report::GroupingMode::AccountTree;
    else if (spec.grouping == "period")
        table.grouping = gnc::report::GroupingMode::Period;
    else
        table.grouping = gnc::report::GroupingMode::None;

    table.period_unit = spec.period_unit == "month"
                            ? gnc::report::PeriodUnit::Month
                            : gnc::report::PeriodUnit::Year;
    table.chart = spec.chart == "bar";

    if (!gnc::report::render_table_spec (table, root, html))
        return fail (error, "the report could not be rendered");
    return true;
}

} // namespace web
