/********************************************************************\
 * gnc-tax.cpp -- the US Tax Schedule report and TXF export          *
 *                                                                  *
 * A native port of gnucash/report/reports/locale-specific/us/       *
 * taxtxf.scm.  The report groups tax-related accounts by their TXF  *
 * code, prints the form/line/account rows and their totals, and     *
 * writes the same TXF records the Scheme report exports.  The code  *
 * table itself is data (gnc-tax-codes.cpp), generated from the      *
 * Scheme category list.                                             *
 *                                                                  *
 * This program is free software; you can redistribute it and/or    *
 * modify it under the terms of the GNU General Public License as   *
 * published by the Free Software Foundation; either version 2 of   *
 * the License, or (at your option) any later version.              *
\********************************************************************/

#include <config.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <glib.h>

#include "gnc-report.hpp"
#include "gnc-tax-codes.hpp"

extern "C"
{
#include <Account.h>
#include <Split.h>
#include <Transaction.h>
#include <gnc-date.h>
#include <gnc-numeric.h>
#include <gnc-ui-util.h>
#include <gnc-version.h>
}

#include <Account.hpp>

namespace gnc
{
namespace report
{
namespace
{

using tax::Class;
using tax::TaxCode;

const char* const crlf = "\r\n";

gnc_numeric
numeric_add (gnc_numeric left, gnc_numeric right)
{
    return gnc_numeric_add (left, right, GNC_DENOM_AUTO, GNC_HOW_DENOM_LCD);
}

/* The Scheme renderer rounds to the commodity's smallest unit before printing
 * (gnc:default-html-gnc-monetary-renderer). */
std::string
format_amount (gnc_numeric amount, gnc_commodity* currency)
{
    if (currency == nullptr)
        return {};
    const gnc_numeric rounded = gnc_numeric_convert (
        amount, gnc_commodity_get_fraction (currency), GNC_HOW_RND_ROUND);
    const char* text = xaccPrintAmount (
        rounded, gnc_commodity_print_info (currency, FALSE));
    return text != nullptr ? std::string (text) : std::string ();
}

std::string
account_full_name (Account* account)
{
    gchar* name = gnc_account_get_full_name (account);
    std::string text = name != nullptr ? name : "";
    g_free (name);
    return text;
}

/* The account types whose split sign the Scheme report reverses before it
 * adds the split to the account total (taxtxf.scm, splt-rpt-amount). */
bool
reverses_balance (GNCAccountType type)
{
    return type == ACCT_TYPE_INCOME || type == ACCT_TYPE_CREDIT ||
           type == ACCT_TYPE_PAYABLE || type == ACCT_TYPE_LIABILITY ||
           type == ACCT_TYPE_EQUITY;
}

Class
class_for (GNCAccountType type)
{
    switch (type)
    {
    case ACCT_TYPE_INCOME:
        return Class::Income;
    case ACCT_TYPE_EXPENSE:
        return Class::Expense;
    case ACCT_TYPE_BANK:
    case ACCT_TYPE_CASH:
    case ACCT_TYPE_ASSET:
    case ACCT_TYPE_STOCK:
    case ACCT_TYPE_MUTUAL:
    case ACCT_TYPE_RECEIVABLE:
        return Class::Asset;
    default:
        return Class::LiabEq;
    }
}

const char*
entity_description (const std::string& entity)
{
    if (entity == "F1040")
        return "Files US Form 1040 Tax Return";
    if (entity == "F1065")
        return "Files US Form 1065 Tax Return";
    if (entity == "F1120")
        return "Files US Form 1120 Tax Return";
    if (entity == "F1120S")
        return "Files US Form 1120S Tax Return";
    return "None specified";
}

/* The line number of a code for a tax year: the first (newest) year entry
 * that is not later than the year, or "" when the table starts after it.
 * This is taxtxf.scm's get-line-info. */
std::string
line_for_year (const TaxCode& code, int year)
{
    for (size_t index = 0; index < code.line_count; ++index)
    {
        if (code.lines[index].year <= year)
            return code.lines[index].line;
    }
    return {};
}

/* taxtxf.scm keeps these two codes paired: either both are represented or
 * neither.  The report is scoped to income/expense accounts, so the pairing
 * is not exercised here; it is noted rather than guessed at. */

std::string
code_digits (const std::string& code)
{
    return code.size () > 1 ? code.substr (1) : code;
}

/* The Scheme form-line-acct-less comparator: form and copy first, then the
 * line number with its numeric and alphabetic parts in numeric order, then
 * the line, code and account name. */
std::vector<std::string>
line_parts (const std::string& line)
{
    std::vector<std::string> parts;
    std::string current;
    bool digits = false;
    for (const char character : line)
    {
        const bool is_digit = g_ascii_isdigit (character);
        if (!current.empty () && is_digit != digits)
        {
            parts.push_back (current);
            current.clear ();
        }
        current += character;
        digits = is_digit;
    }
    if (!current.empty ())
        parts.push_back (current);
    return parts;
}

int
compare_parts (const std::vector<std::string>& left,
               const std::vector<std::string>& right)
{
    const size_t count = std::min (left.size (), right.size ());
    for (size_t index = 0; index < count; ++index)
    {
        const bool left_digits =
            std::all_of (left[index].begin (), left[index].end (),
                         [] (char c) { return g_ascii_isdigit (c); });
        const bool right_digits =
            std::all_of (right[index].begin (), right[index].end (),
                         [] (char c) { return g_ascii_isdigit (c); });
        if (left_digits && right_digits)
        {
            const long left_value = std::stol (left[index]);
            const long right_value = std::stol (right[index]);
            if (left_value != right_value)
                return left_value < right_value ? -1 : 1;
        }
        else
        {
            const int result = left[index].compare (right[index]);
            if (result != 0)
                return result < 0 ? -1 : 1;
        }
    }
    if (left.size () != right.size ())
        return left.size () < right.size () ? -1 : 1;
    return 0;
}

struct Entry
{
    Account* account = nullptr;
    GNCAccountType type = ACCT_TYPE_INCOME;
    const TaxCode* info = nullptr;
    std::string form;
    std::string copy;
    std::string line;
    std::string code;
    std::string name;
    gnc_numeric total = gnc_numeric_zero ();
    gnc_numeric total_as_dr = gnc_numeric_zero ();
    std::vector<Split*> splits;
};

bool
entry_less (const Entry& left, const Entry& right)
{
    const std::string left_form = left.form + " " + left.copy;
    const std::string right_form = right.form + " " + right.copy;
    if (left_form != right_form)
        return left_form < right_form;
    const int line_order = compare_parts (line_parts (left.line),
                                          line_parts (right.line));
    if (line_order != 0)
        return line_order < 0;
    const std::string left_rest =
        left.line + " " + left.code + " " + left.name;
    const std::string right_rest =
        right.line + " " + right.code + " " + right.name;
    return left_rest < right_rest;
}

std::string
form_display (const Entry& entry)
{
    std::string form = entry.info != nullptr ? entry.info->form : "";
    if (entry.copy != "1")
        form += "(" + entry.copy + ")";
    return form;
}

/* One table heading row: a label spanning the six columns the Scheme table
 * uses. */
Cell
heading_cell (const std::string& text)
{
    Cell cell;
    cell.text = text;
    cell.css = "column-heading-left";
    cell.span = 6;
    return cell;
}

/* One total row: a right-aligned bold label and the amount.  The Scheme
 * renderer marks the label "column-heading-right", which is what the parity
 * extractor reads as a subtotal. */
std::vector<Cell>
total_row (const std::string& label, gnc_numeric amount,
           gnc_commodity* currency)
{
    Cell label_cell;
    label_cell.text = label;
    label_cell.css = "column-heading-right";
    label_cell.span = 5;
    Cell amount_cell;
    amount_cell.text = format_amount (amount, currency);
    amount_cell.css = gnc_numeric_negative_p (amount) ? "number-cell neg"
                                                      : "number-cell";
    return {label_cell, amount_cell};
}

/* The payer name the TXF record uses.  The account's own
 * tax-US/payer-name-source option decides it, exactly as
 * gnc:account-get-txf-payer-source does: "parent" means the parent
 * account's name, anything else the account's own name. */
std::string
payer_name (const Entry& entry)
{
    const char* source = xaccAccountGetTaxUSPayerNameSource (entry.account);
    if (source != nullptr && g_strcmp0 (source, "parent") == 0)
    {
        Account* parent = gnc_account_get_parent (entry.account);
        if (parent != nullptr && xaccAccountGetName (parent) != nullptr)
            return xaccAccountGetName (parent);
    }
    return xaccAccountGetName (entry.account) != nullptr
               ? xaccAccountGetName (entry.account)
               : "";
}

std::string
txf_action (const Entry& entry)
{
    if (entry.type == ACCT_TYPE_INCOME)
    {
        if (entry.code == "N286" || entry.code == "N488")
            return "ReinvD";
        return "Income";
    }
    if (entry.type == ACCT_TYPE_EXPENSE)
        return "Expense";
    if (entry.type == ACCT_TYPE_EQUITY)
        return "Equity";
    if (entry.type == ACCT_TYPE_CREDIT || entry.type == ACCT_TYPE_LIABILITY ||
        entry.type == ACCT_TYPE_PAYABLE)
        return "Liability";
    return "Asset";
}

std::string
fill_clamp (const std::string& text, size_t length)
{
    std::string padded = text;
    if (padded.size () < length)
        padded.append (length - padded.size (), ' ');
    return padded.substr (0, length);
}

/* taxtxf.scm's fill-clamp-sp: all but the last column, then a space. */
std::string
fill_clamp_sp (const std::string& text, size_t length)
{
    std::string padded = text;
    if (padded.size () < length)
        padded.append (length - padded.size (), ' ');
    if (length == 0)
        return padded;
    return padded.substr (0, length - 1) + " ";
}

std::string
txf_date (time64 when)
{
    char* text = gnc_print_time64 (when, "%m/%d/%Y");
    std::string result = text != nullptr ? text : "";
    g_free (text);
    return result;
}

std::string
txf_amount (gnc_numeric amount)
{
    char buffer[64] = {0};
    std::snprintf (buffer, sizeof buffer, "%.2f",
                   gnc_numeric_to_double (amount));
    return std::string ("$") + buffer;
}

std::string
td_ts (bool detail)
{
    return detail ? "TD" : "TS";
}

/* One TXF record.  `amount` is the value before taxtxf.scm's final negation,
 * i.e. the account's balance "as debit": income credits are negative here,
 * expenses debits are negative too, and render-txf-account writes -amount. */
void
append_txf_record (std::ostringstream& out, const Entry& entry, bool detail,
                   const std::string& value, int item,
                   const std::string& x_date, bool x_date_is_transaction,
                   time64 from_value, gnc_commodity* currency)
{
    const TaxCode& code = *entry.info;
    out << td_ts (detail) << crlf;
    out << code.code << crlf;
    out << "C" << entry.copy << crlf;
    out << "L" << item << crlf;

    const bool special_split =
        entry.code == "N521" || entry.code == "N522";
    /* Format 6 detail records carry the payment date; the report's N521
     * records come from each split, so the transaction date is that date. */
    if (detail && code.format == 6 && x_date_is_transaction)
        out << "D" << x_date << crlf;

    out << value << crlf;

    if (!detail && code.format == 3)
        out << "P" << payer_name (entry) << crlf;
    else if (detail && code.format == 6 && special_split)
    {
        /* Federal estimated tax has no state initials; N522 is a state
         * payment and names the state through the account name. */
        out << "P";
        if (entry.code != "N521")
            out << payer_name (entry);
        out << crlf;
    }

    if (detail)
    {
        std::string value_name;
        if (code.code == std::string ("N286") ||
            code.code == std::string ("N488"))
            value_name = format_amount (gnc_numeric_neg (entry.total_as_dr),
                                        currency) +
                         " " + payer_name (entry);
        else
            value_name = payer_name (entry);
        const std::string action = txf_action (entry);
        out << "X";
        out << (x_date_is_transaction ? x_date : txf_date (from_value));
        out << " ";
        out << fill_clamp_sp (payer_name (entry), 31);
        out << fill_clamp_sp (action, 7);
        out << fill_clamp_sp (value_name, 82);
        out << fill_clamp (code.category_key, 15);
        out << crlf;
    }
    out << "^" << crlf;
}

} // namespace

TaxScheduleReport
tax_schedule (Account* root, const TaxScheduleOptions& options)
{
    TaxScheduleReport report;
    report.title = "Taxable Income/Deductible Expenses";

    gnc_commodity* currency =
        options.currency != nullptr ? options.currency
                                    : gnc_default_report_currency ();
    if (currency == nullptr && root != nullptr)
        currency = xaccAccountGetCommodity (root);

    const std::string entity =
        gnc_get_current_book_tax_type () != nullptr
            ? gnc_get_current_book_tax_type ()
            : "";
    const char* tax_name = gnc_get_current_book_tax_name ();

    /* The period.  The Scheme report's "Alternate Period" presets override
     * the From/To dates; from-to is the default and uses them as given. */
    time64 from_value = options.start.value_or (default_start_date ());
    time64 to_value = options.end.value_or (default_end_date ());
    const std::string alternate =
        options.alternate_period.empty () ? "from-to"
                                          : options.alternate_period;
    if (alternate != "from-to")
    {
        GDate from_date = time64_to_gdate (from_value);
        const int year = g_date_get_year (&from_date);
        const bool last_year =
            alternate.find ("last") != std::string::npos;
        const int start_year = last_year ? year - 1 : year;
        int start_month = 1;
        int end_month = 12;
        int end_day = 31;
        if (alternate.find ("1st") != std::string::npos)
        {
            start_month = 1;
            end_month = 3;
        }
        else if (alternate.find ("2nd") != std::string::npos)
        {
            start_month = 4;
            end_month = 5;
        }
        else if (alternate.find ("3rd") != std::string::npos)
        {
            start_month = 6;
            end_month = 8;
        }
        else if (alternate.find ("4th") != std::string::npos)
        {
            start_month = 9;
            end_month = 12;
        }
        from_value = gnc_dmy2time64 (1, start_month, start_year);
        to_value = gnc_dmy2time64_end (end_day, end_month, start_year);
    }

    const GDate from_date = time64_to_gdate (from_value);
    const int tax_year = g_date_get_year (&from_date);

    report.from_date = from_value;
    report.to_date = to_value;
    report.tax_year = tax_year;
    report.entity = entity;

    std::ostringstream subtitle;
    if (tax_name != nullptr && tax_name[0] != '\0')
        subtitle << "Tax Name: " << tax_name << " | ";
    subtitle << "Period from " << txf_date (from_value) << " to "
             << txf_date (to_value) << " | Tax Year " << tax_year
             << " | Tax Entity Type: " << entity_description (entity);
    report.table.subtitle = subtitle.str ();

    /* The account scope: every tax-related account below the root, or the
     * caller's selection (already expanded to subtrees by the route), with
     * non-tax-related accounts filtered out, as validate() does. */
    std::vector<Account*> accounts = options.accounts;
    if (accounts.empty () && root != nullptr)
    {
        gnc_account_foreach_descendant (
            root,
            [&accounts] (Account* account) { accounts.push_back (account); });
    }

    std::vector<Entry> entries;
    for (Account* account : accounts)
    {
        if (account == nullptr || !xaccAccountGetTaxRelated (account))
            continue;
        const char* raw_code = xaccAccountGetTaxUSCode (account);
        if (raw_code == nullptr || raw_code[0] == '\0')
            continue;
        const std::string code_text = raw_code;
        const GNCAccountType type = xaccAccountGetType (account);
        const TaxCode* info =
            tax::lookup (class_for (type), entity.c_str (), code_text.c_str ());
        if (info == nullptr)
            continue;
        if (info->last_year != 0 && tax_year > info->last_year)
            continue;

        Entry entry;
        entry.account = account;
        entry.type = type;
        entry.info = info;
        entry.form = info->form;
        entry.copy = std::to_string (xaccAccountGetTaxUSCopyNumber (account));
        entry.line = line_for_year (*info, tax_year);
        entry.code = code_text;
        entry.name = account_full_name (account);

        gnc_numeric raw = gnc_numeric_zero ();
        gnc_account_foreach_split_between_dates (
            account, from_value, to_value, false,
            [&entry, &raw] (Split* split)
            {
                entry.splits.push_back (split);
                raw = numeric_add (raw, xaccSplitGetAmount (split));
            });
        entry.total_as_dr = raw;
        entry.total = reverses_balance (type) ? gnc_numeric_neg (raw) : raw;
        entries.push_back (entry);
    }

    std::sort (entries.begin (), entries.end (), entry_less);

    /* ------------------------------------------------------------ table */
    gnc_numeric line_total = gnc_numeric_zero ();
    gnc_numeric form_line_total = gnc_numeric_zero ();
    std::string current_form;
    std::string current_line;
    std::string current_code;
    std::string saved_code_text;
    bool have_group = false;
    bool code_heading_printed = false;
    /* The Scheme report prints a sub-total for each payer of a "format 3"
     * code (Schedule B interest, dividends, ...), before the code total. */
    gnc_numeric sub_item_total = gnc_numeric_zero ();
    std::string sub_item_payer;
    int sub_item_number = 0;
    int sub_item_next = 1;
    bool have_sub_item = false;

    auto flush_sub_item = [&] ()
    {
        if (!have_sub_item)
            return;
        if (!(options.suppress_zero && gnc_numeric_zero_p (sub_item_total)))
        {
            std::string label =
                "Total For " +
                std::string (current_line.empty () ? "Line (Code): " : "") +
                saved_code_text + ", Item " +
                std::to_string (sub_item_number) + ": " + sub_item_payer;
            report.table.rows.push_back (
                total_row (label, sub_item_total, currency));
        }
        sub_item_total = gnc_numeric_zero ();
        have_sub_item = false;
    };

    auto flush_code = [&] (bool final)
    {
        if (!have_group)
            return;
        const bool zero = gnc_numeric_zero_p (line_total);
        if (!(options.suppress_zero && zero))
        {
            std::string label;
            if (final || current_line.empty ())
                label = "Total For Line (Code): " + saved_code_text;
            else
                label = "Total For " + saved_code_text;
            report.table.rows.push_back (
                total_row (label, line_total, currency));
        }
        line_total = gnc_numeric_zero ();
    };

    auto flush_form_line = [&] ()
    {
        if (!have_group || current_form.empty () || current_line.empty ())
        {
            form_line_total = gnc_numeric_zero ();
            return;
        }
        const bool zero = gnc_numeric_zero_p (form_line_total);
        if (!(options.suppress_zero && zero))
            report.table.rows.push_back (total_row (
                "Total For " + current_form + " Line " + current_line,
                form_line_total, currency));
        form_line_total = gnc_numeric_zero ();
    };

    for (const Entry& entry : entries)
    {
        const std::string form = form_display (entry);
        const std::string line = entry.line;
        const std::string code = entry.code;
        const bool new_form = !have_group || form != current_form;
        const bool new_line = new_form || line != current_line;
        const bool new_code = new_line || code != current_code;

        if (new_code)
        {
            flush_sub_item ();
            flush_code (false);
            if (new_line)
                flush_form_line ();
        }

        if (new_form)
        {
            report.table.rows.push_back (
                {heading_cell ("Form or Schedule / Line (TXF Code) / "
                               "Account Name")});
            report.table.rows.push_back ({heading_cell (form)});
        }

        if (new_code)
        {
            saved_code_text =
                (line.empty () ? "" : "Line " + line + ": ") +
                (entry.info->description != nullptr
                     ? entry.info->description
                     : "") +
                " (" + code_digits (code) + ")";
            code_heading_printed = false;
            sub_item_next = 1;
        }

        const bool has_activity = !entry.splits.empty ();
        if (entry.info->format == 3 &&
            (entry.info->payer == tax::TaxPayer::Current ||
             entry.info->payer == tax::TaxPayer::Parent))
        {
            const std::string payer = payer_name (entry);
            if (have_sub_item && payer != sub_item_payer)
                flush_sub_item ();
            if (!have_sub_item)
            {
                sub_item_payer = payer;
                sub_item_number = sub_item_next++;
                have_sub_item = true;
            }
            sub_item_total = numeric_add (sub_item_total, entry.total);
        }
        if (has_activity && !code_heading_printed)
        {
            /* The Scheme report prints the line heading from inside the
             * transaction-detail path, so a code whose accounts have no
             * activity in the period gets only its zero total, no heading. */
            report.table.rows.push_back ({heading_cell (
                "    " + saved_code_text)});
            code_heading_printed = true;
        }
        if (has_activity)
        {
            report.table.rows.push_back ({heading_cell (entry.name)});
            report.table.rows.push_back (
                total_row ("Total For Account: " + entry.name, entry.total,
                           currency));
        }

        line_total = numeric_add (line_total, entry.total);
        form_line_total = numeric_add (form_line_total, entry.total);
        current_form = form;
        current_line = line;
        current_code = code;
        have_group = true;
    }

    if (have_group)
    {
        flush_sub_item ();
        flush_code (true);
        flush_form_line ();
    }

    if (report.table.rows.empty ())
    {
        report.table.subtitle =
            "No Tax Related accounts were found with your account selection. "
            "Change your selection or go to the Edit->Tax Report Options "
            "dialog to set up tax-related accounts.";
    }

    /* -------------------------------------------------------------- TXF */
    std::ostringstream txf;
    txf << "V042" << crlf << "AGnuCash " << gnc_version () << crlf;
    const time64 today = gnc_time64_get_today_start ();
    txf << "D" << txf_date (today) << crlf << "^" << crlf;

    /* The Scheme export writes one detail record per account (one per split
     * for the estimated-tax codes), then one summary record per tax code.
     * It implements formats 1, 3 and 6; a code with another format is left
     * out of the file rather than written with guessed fields. */
    const bool txf_entity = entity == "F1040";
    if (txf_entity && !entries.empty ())
    {
        auto write_detail = [&] (const Entry& entry, int item)
        {
            if (entry.code == "N521" || entry.code == "N522")
            {
                for (Split* split : entry.splits)
                {
                    const gnc_numeric split_value =
                        gnc_numeric_neg (xaccSplitGetAmount (split));
                    const time64 date =
                        xaccTransGetDate (xaccSplitGetParent (split));
                    append_txf_record (txf, entry, true,
                                       txf_amount (split_value), item,
                                       txf_date (date), true, from_value,
                                       currency);
                }
            }
            else
            {
                append_txf_record (
                    txf, entry, true,
                    txf_amount (gnc_numeric_neg (entry.total_as_dr)), item,
                    "", false, from_value, currency);
            }
        };
        auto write_summary = [&] (const Entry& entry, gnc_numeric total_as_dr,
                                  int item)
        {
            if (gnc_numeric_zero_p (total_as_dr))
                return;
            append_txf_record (txf, entry, false,
                               txf_amount (gnc_numeric_neg (total_as_dr)),
                               item, "", false, from_value, currency);
        };

        size_t index = 0;
        while (index < entries.size ())
        {
            size_t end = index + 1;
            while (end < entries.size () &&
                   entries[end].form == entries[index].form &&
                   entries[end].line == entries[index].line &&
                   entries[end].code == entries[index].code)
                ++end;

            const TaxCode& code = *entries[index].info;
            const bool supported =
                code.format == 1 || code.format == 3 || code.format == 6;
            if (!supported)
            {
                /* No faithful record layout for formats 4/5; omit them
                 * rather than write guessed capital-gains fields. */
                index = end;
                continue;
            }

            /* A format-3 code with a current/parent payer is written as one
             * detail + summary pair per payer, numbered from 1, exactly as
             * the Scheme report's sub-item loop does. */
            const bool per_payer =
                code.format == 3 &&
                (code.payer == tax::TaxPayer::Current ||
                 code.payer == tax::TaxPayer::Parent);
            if (per_payer)
            {
                size_t at = index;
                int item = 1;
                while (at < end)
                {
                    const std::string payer = payer_name (entries[at]);
                    size_t sub_end = at;
                    gnc_numeric payer_total = gnc_numeric_zero ();
                    while (sub_end < end &&
                           payer_name (entries[sub_end]) == payer)
                    {
                        const Entry& entry = entries[sub_end];
                        if (entry.code != "N000" &&
                            !gnc_numeric_zero_p (entry.total_as_dr))
                        {
                            write_detail (entry, item);
                            payer_total = numeric_add (payer_total,
                                                       entry.total_as_dr);
                        }
                        ++sub_end;
                    }
                    write_summary (entries[sub_end - 1], payer_total, item);
                    ++item;
                    at = sub_end;
                }
            }
            else
            {
                gnc_numeric group_total = gnc_numeric_zero ();
                bool wrote_detail = false;
                for (size_t at = index; at < end; ++at)
                {
                    const Entry& entry = entries[at];
                    group_total = numeric_add (group_total,
                                               entry.total_as_dr);
                    if (entry.code == "N000" ||
                        gnc_numeric_zero_p (entry.total_as_dr))
                        continue;
                    write_detail (entry, 1);
                    wrote_detail = true;
                }
                if (wrote_detail)
                    write_summary (entries[end - 1], group_total, 1);
            }
            index = end;
        }
    }
    /* The Scheme export file ends with a newline after the last record's
     * "^" line (gnucash-cli writes the export string plus a final line
     * feed); keep the same bytes so a file diff is exact. */
    txf << "\n";
    report.txf = txf.str ();

    return report;
}

bool
tax_txf (Account* root, const TaxScheduleOptions& options, std::string& out)
{
    out = tax_schedule (root, options).txf;
    return true;
}

} // namespace report
} // namespace gnc
