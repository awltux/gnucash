/********************************************************************\
 * gnc-owner.cpp -- the owner reports, from the engine's own records *
 *                                                                  *
 * new-owner-report.scm registers one renderer four times, once for  *
 * each owner type; customer-summary.scm asks a different question   *
 * (profit per customer).  This file keeps that split: one owner     *
 * history walk shared by the four registrations, and one summary    *
 * over the income and expense accounts.  The engine is still the    *
 * source of every number: invoice amounts, payment splits and lot    *
 * balances.  The Scheme markup is not reproduced cell for cell, but *
 * the same cell classes are used so a parity test can read both.    *
 *                                                                  *
 * This program is free software; you can redistribute it and/or    *
 * modify it under the terms of the GNU General Public License as   *
 * published by the Free Software Foundation; either version 2 of    *
 * the License, or (at your option) any later version.               *
\********************************************************************/

#include <config.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <glib.h>

#include "gnc-owner.hpp"

extern "C"
{
#include <Account.h>
#include <Split.h>
#include <Transaction.h>
#include <engine-helpers.h>
#include <gnc-commodity.h>
#include <gnc-date.h>
#include <gnc-lot.h>
#include <gnc-numeric.h>
#include <gnc-unicode.h>
#include <gncBusiness.h>
#include <gncCustomer.h>
#include <gncEmployee.h>
#include <gncInvoice.h>
#include <gncJob.h>
#include <gncOwner.h>
#include <gncVendor.h>
#include <guid.h>
#include <qofbook.h>
}

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

gnc_numeric
numeric_sub (gnc_numeric left, gnc_numeric right)
{
    return gnc_numeric_sub (left, right, GNC_DENOM_AUTO, GNC_HOW_DENOM_LCD);
}

bool
zero_p (gnc_numeric value)
{
    return gnc_numeric_zero_p (value);
}

std::string
text_or_empty (const char* text)
{
    return text != nullptr ? text : "";
}

std::string
owner_guid_text (const GncOwner* owner)
{
    const GncGUID* guid = gncOwnerGetGUID (owner);
    if (guid == nullptr)
        return {};
    gchar* text = guid_to_string (guid);
    std::string result = text_or_empty (text);
    g_free (text);
    return result;
}

/* The distinct commodities of a set of accounts, in account order.  This is
 * gnc:accounts-get-commodities(all-accounts, #f): the commodities the
 * Customer Summary's rows are keyed by. */
std::vector<gnc_commodity*>
account_commodities (const std::vector<Account*>& accounts)
{
    std::vector<gnc_commodity*> commodities;
    std::set<intptr_t> seen;
    for (Account* account : accounts)
    {
        gnc_commodity* commodity = xaccAccountGetCommodity (account);
        if (commodity != nullptr &&
            seen.insert (reinterpret_cast<intptr_t> (commodity)).second)
            commodities.push_back (commodity);
    }
    return commodities;
}

std::vector<Account*>
descendants (Account* root)
{
    std::vector<Account*> accounts;
    if (root == nullptr)
        return accounts;
    GList* list = gnc_account_get_descendants_sorted (root);
    for (GList* node = list; node != nullptr; node = node->next)
        accounts.push_back (static_cast<Account*> (node->data));
    g_list_free (list);
    return accounts;
}

/* ------------------------------------------------------------------- HTML */

std::string
html_escape (const std::string& text)
{
    std::string out;
    out.reserve (text.size ());
    for (const char ch : text)
    {
        switch (ch)
        {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        default: out += ch; break;
        }
    }
    return out;
}

const char*
report_css ()
{
    return "table { border-collapse: collapse; } "
           "th { text-align: left; } "
           "td.number-cell, td.total-number-cell { text-align: right; "
           "white-space: nowrap; } "
           "td.total-label-cell, td.total-number-cell { font-weight: bold; }";
}

Cell
text_cell (const std::string& text, const char* css = "")
{
    Cell cell;
    cell.text = text;
    cell.css = css;
    return cell;
}

Cell
blank_cell (const char* css = "")
{
    Cell cell;
    cell.text = " ";
    cell.css = css;
    return cell;
}

Cell
number_cell (gnc_numeric amount, gnc_commodity* currency,
             const char* css = "number-cell")
{
    Cell cell;
    cell.text = format_money (amount, currency);
    if (cell.text.empty ())
        cell.text = " ";
    cell.css = css;
    return cell;
}

void
append_table (std::ostringstream& out, const CellTable& table)
{
    out << "<table cellspacing='0' cellpadding='4' border='1'>";
    if (!table.headings.empty ())
    {
        out << "<thead><tr>";
        for (const std::string& heading : table.headings)
            out << "<th>" << html_escape (heading) << "</th>";
        out << "</tr></thead>";
    }
    out << "<tbody>";
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
        out << "</tr>";
    }
    out << "</tbody></table>";
}

void
append_warning (std::ostringstream& out, const std::string& headline,
                const std::string& explanation)
{
    out << "<h3>" << html_escape (headline) << "</h3>";
    if (!explanation.empty ())
        out << "<p>" << html_escape (explanation) << "</p>";
}

/* ------------------------------------------------------------ owner kinds */

const char*
qof_id_for_kind (OwnerKind kind)
{
    switch (kind)
    {
    case OwnerKind::Customer: return GNC_ID_CUSTOMER;
    case OwnerKind::Vendor: return GNC_ID_VENDOR;
    case OwnerKind::Employee: return GNC_ID_EMPLOYEE;
    case OwnerKind::Job: return GNC_ID_JOB;
    case OwnerKind::None: break;
    }
    return nullptr;
}

bool
job_report (OwnerKind kind)
{
    return kind == OwnerKind::Job;
}

bool
receivable_report (OwnerKind kind)
{
    /* new-owner-report.scm: the end owner of a job is its customer, so the
     * Job Report reads A/Receivable too. */
    return kind == OwnerKind::Customer || kind == OwnerKind::Job;
}

/* The owner of a document or lot, compared the way setup-query() does: the
 * end owner for the three party reports, the job itself for the Job Report. */
const GncGUID*
matching_guid (const GncOwner* owner, bool job)
{
    return job ? gncOwnerGetGUID (owner) : gncOwnerGetEndGUID (owner);
}

bool
owner_matches (const GncOwner* candidate, const GncOwner* selected, bool job)
{
    if (candidate == nullptr || !gncOwnerIsValid (candidate))
        return false;
    const GncGUID* left = matching_guid (candidate, job);
    const GncGUID* right = matching_guid (selected, job);
    return left != nullptr && right != nullptr && guid_equal (left, right);
}

/* ---------------------------------------------------------- owner history */

/* The Scheme report's num-buckets and the aging bucket machinery, shared with
 * the aging reports. */
constexpr int bucket_count = 6;
constexpr int prepayment_bucket = bucket_count - 1;

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
        if (when < boundaries[index])
            return static_cast<int> (index);
    return static_cast<int> (boundaries.size ());
}

bool
sign_equal (gnc_numeric left, gnc_numeric right)
{
    if (gnc_numeric_zero_p (left) || gnc_numeric_zero_p (right))
        return gnc_numeric_zero_p (left) && gnc_numeric_zero_p (right);
    return gnc_numeric_positive_p (left) == gnc_numeric_positive_p (right);
}

/* The part of a payment that is not applied to an invoice, exactly as
 * gnc:payment-txn->payment-info() computes it. */
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

/* One account's aging vector, in the Scheme's bucket order:
 * 0 = 91+, 1 = 61-90, 2 = 31-60, 3 = 0-30, 4 = Current, 5 = Pre-Payment. */
std::vector<gnc_numeric>
owner_aging (const std::vector<Split*>& splits, time64 as_of, bool receivable)
{
    const std::vector<time64> boundaries = aging_boundaries (as_of);
    std::vector<gnc_numeric> buckets (bucket_count, gnc_numeric_zero ());
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
                    for (GList* node = gnc_lot_get_split_list (lot);
                         node != nullptr; node = node->next)
                    {
                        auto* lot_split = static_cast<Split*> (node->data);
                        if (xaccTransGetDate (xaccSplitGetParent (lot_split)) <=
                            as_of)
                            balance = numeric_add (
                                balance,
                                xaccSplitConvertAmount (lot_split, posted));
                    }
                }
                if (!receivable)
                    balance = gnc_numeric_neg (balance);
                const time64 when = gncInvoiceGetDateDue (invoice);
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
            buckets[prepayment_bucket] = numeric_add (
                buckets[prepayment_bucket], overpayment);
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
            ++index;
        }
    }
    return buckets;
}

/* The split's owner for the owner report's query: an invoice owns its
 * transaction; a payment owns the lot it sits in, or the invoice posted in
 * that lot. */
bool
split_owner (Split* split, GncOwner& owner)
{
    Transaction* txn = xaccSplitGetParent (split);
    const char type = xaccTransGetTxnType (txn);
    owner.type = GNC_OWNER_NONE;
    owner.owner.undefined = nullptr;
    owner.qof_temp = nullptr;

    if (type == TXN_TYPE_INVOICE)
    {
        GncInvoice* invoice = gncInvoiceGetInvoiceFromTxn (txn);
        if (invoice == nullptr)
            return false;
        gncOwnerCopy (gncInvoiceGetOwner (invoice), &owner);
        return true;
    }
    if (type == TXN_TYPE_PAYMENT)
    {
        GNCLot* lot = xaccSplitGetLot (split);
        if (lot == nullptr)
            return false;
        if (gncOwnerGetOwnerFromLot (lot, &owner))
            return true;
        GncInvoice* invoice = gncInvoiceGetInvoiceFromLot (lot);
        if (invoice == nullptr)
            return false;
        gncOwnerCopy (gncInvoiceGetOwner (invoice), &owner);
        return true;
    }
    return false;
}

/* The splits of one A/R or A/P account that belong to the selected owner,
 * oldest posted date first and one per transaction.  Only invoice and payment
 * transactions are part of the report, which is what the Scheme walk's
 * txn-is-invoice?/txn-is-payment? guards do. */
std::vector<Split*>
owner_splits (Account* account, const GncOwner& selected, bool job,
              std::optional<time64> end)
{
    std::vector<Split*> splits;
    for (GList* node = xaccAccountGetSplitList (account); node != nullptr;
         node = node->next)
    {
        auto* split = static_cast<Split*> (node->data);
        Transaction* txn = xaccSplitGetParent (split);
        if (end.has_value () && xaccTransGetDate (txn) > *end)
            continue;
        const char type = xaccTransGetTxnType (txn);
        if (type != TXN_TYPE_INVOICE && type != TXN_TYPE_PAYMENT)
            continue;
        GncOwner owner;
        if (!split_owner (split, owner) ||
            !owner_matches (&owner, &selected, job))
            continue;
        splits.push_back (split);
    }

    std::stable_sort (splits.begin (), splits.end (),
                      [] (Split* left, Split* right)
                      {
                          return xaccTransGetDate (xaccSplitGetParent (left)) <
                                 xaccTransGetDate (xaccSplitGetParent (right));
                      });

    std::vector<Split*> unique;
    std::set<Transaction*> seen;
    for (Split* split : splits)
        if (seen.insert (xaccSplitGetParent (split)).second)
            unique.push_back (split);
    return unique;
}

/* The payment's description is the memo of the A/Payable or A/Receivable
 * splits, empty ones skipped, with the <br> separators the Scheme list->cell
 * helper emits flattened to spaces. */
std::string
payment_description (Transaction* txn)
{
    std::string text;
    GList* splits = xaccTransGetAPARAcctSplitList (txn, TRUE);
    for (GList* node = splits; node != nullptr; node = node->next)
    {
        auto* split = static_cast<Split*> (node->data);
        const char* memo = xaccSplitGetMemo (split);
        if (memo == nullptr || memo[0] == '\0')
            continue;
        if (!text.empty ())
            text += " ";
        text += memo;
    }
    g_list_free (splits);
    return text;
}

/* The invoice type the Scheme report's type column prints. */
std::string
invoice_type (GncInvoice* invoice)
{
    return text_or_empty (gncInvoiceGetTypeString (invoice));
}

/* gnc:get-num-action(txn, split): the transaction number, or the split
 * action when the book uses split actions for the number field. */
std::string
num_action (Transaction* txn, Split* split, Account* account)
{
    QofBook* book = gnc_account_get_book (account);
    if (book != nullptr && qof_book_use_split_action_for_num_field (book))
        return text_or_empty (xaccSplitGetAction (split));
    return text_or_empty (xaccTransGetNum (txn));
}

/* The job the invoice belongs to, for the optional Job column. */
std::string
invoice_job (GncInvoice* invoice)
{
    const GncOwner* owner = gncInvoiceGetOwner (invoice);
    if (owner != nullptr && gncOwnerGetType (owner) == GNC_OWNER_JOB)
        return text_or_empty (gncJobGetID (gncOwnerGetJob (owner)));
    return {};
}

/* ------------------------------------------------------------- one report */

enum class OwnerColumn
{
    Date,
    DueDate,
    Reference,
    Type,
    Description,
    Job,
    Sale,
    Tax,
    Debits,
    Credits,
    Balance,
};

std::vector<OwnerColumn> owner_columns (const OwnerReportOptions& options)
{
    std::vector<OwnerColumn> columns;
    if (options.show_date) columns.push_back (OwnerColumn::Date);
    if (options.show_due_date) columns.push_back (OwnerColumn::DueDate);
    if (options.show_reference) columns.push_back (OwnerColumn::Reference);
    if (options.show_type) columns.push_back (OwnerColumn::Type);
    if (options.show_description)
        columns.push_back (OwnerColumn::Description);
    if (options.show_job) columns.push_back (OwnerColumn::Job);
    if (options.show_sale) columns.push_back (OwnerColumn::Sale);
    if (options.show_tax) columns.push_back (OwnerColumn::Tax);
    if (options.show_debits) columns.push_back (OwnerColumn::Debits);
    if (options.show_credits) columns.push_back (OwnerColumn::Credits);
    if (options.show_balance) columns.push_back (OwnerColumn::Balance);
    return columns;
}

} // namespace

/* ---------------------------------------------------------- public model */

const char*
owner_kind_name (OwnerKind kind)
{
    switch (kind)
    {
    case OwnerKind::Customer: return "Customer";
    case OwnerKind::Vendor: return "Vendor";
    case OwnerKind::Employee: return "Employee";
    case OwnerKind::Job: return "Job";
    case OwnerKind::None: break;
    }
    return "";
}

bool
owner_report_kind (const std::string& name, OwnerKind& kind)
{
    if (name == "Customer Report")
        kind = OwnerKind::Customer;
    else if (name == "Vendor Report")
        kind = OwnerKind::Vendor;
    else if (name == "Employee Report")
        kind = OwnerKind::Employee;
    else if (name == "Job Report")
        kind = OwnerKind::Job;
    else
        return false;
    return true;
}

bool
is_owner_report (const std::string& name)
{
    OwnerKind kind;
    return owner_report_kind (name, kind);
}

bool
owner_lookup (Account* root, OwnerKind kind, const std::string& text,
              GncOwner& owner)
{
    if (root == nullptr || text.empty ())
        return false;
    QofBook* book = gnc_account_get_book (root);
    const char* type = qof_id_for_kind (kind);
    if (book == nullptr || type == nullptr)
        return false;

    GncGUID guid;
    if (string_to_guid (text.c_str (), &guid))
        return gncOwnerGetOwnerFromTypeGuid (
                   book, &owner, type, &guid) != FALSE;

    /* A convenience the route documents: an ambiguous name is not accepted,
     * an id or name that names exactly one owner is. */
    GList* candidates = gncBusinessGetOwnerList (
        book, type, TRUE);
    GncOwner match;
    match.type = GNC_OWNER_NONE;
    match.owner.undefined = nullptr;
    match.qof_temp = nullptr;
    int found = 0;
    for (GList* node = candidates; node != nullptr; node = node->next)
    {
        auto* candidate = static_cast<GncOwner*> (node->data);
        const std::string id = text_or_empty (gncOwnerGetID (candidate));
        const std::string name = text_or_empty (gncOwnerGetName (candidate));
        if (id == text || name == text)
        {
            gncOwnerCopy (candidate, &match);
            ++found;
        }
    }
    for (GList* node = candidates; node != nullptr; node = node->next)
        gncOwnerFree (static_cast<GncOwner*> (node->data));
    g_list_free (candidates);
    if (found != 1)
        return false;
    gncOwnerCopy (&match, &owner);
    return true;
}

bool
set_owner_report_option (OwnerReportOptions& options, const std::string& name,
                         bool value)
{
    if (name == "show_date") options.show_date = value;
    else if (name == "show_due_date") options.show_due_date = value;
    else if (name == "show_reference") options.show_reference = value;
    else if (name == "show_type") options.show_type = value;
    else if (name == "show_description") options.show_description = value;
    else if (name == "show_job") options.show_job = value;
    else if (name == "show_sale") options.show_sale = value;
    else if (name == "show_tax") options.show_tax = value;
    else if (name == "show_debits") options.show_debits = value;
    else if (name == "show_credits") options.show_credits = value;
    else if (name == "show_balance") options.show_balance = value;
    else
        return false;
    return true;
}

bool
set_customer_summary_option (CustomerSummaryOptions& options,
                             const std::string& name, bool value)
{
    if (name == "show_expense") options.show_expense = value;
    else if (name == "show_zero_lines") options.show_zero_lines = value;
    else if (name == "show_inactive") options.show_inactive = value;
    else
        return false;
    return true;
}

OwnerReport
owner_report (Account* root, const OwnerReportOptions& options)
{
    OwnerReport report;
    const std::string noun = owner_kind_name (options.kind);
    std::string lower_noun = noun;
    if (!lower_noun.empty ())
        lower_noun[0] = static_cast<char> (
            g_ascii_tolower (static_cast<gchar> (lower_noun[0])));
    report.title = noun + " Report";

    if (root == nullptr)
    {
        report.warning = "No book is open";
        return report;
    }
    if (options.owner_guid.empty ())
    {
        report.warning = "No valid " + lower_noun + " selected.";
        report.subtitle =
            "This report requires a " + lower_noun + " to be selected.";
        return report;
    }

    GncOwner owner;
    if (!owner_lookup (root, options.kind, options.owner_guid, owner))
    {
        report.warning = "No valid " + lower_noun + " selected.";
        report.subtitle =
            "This report requires a " + lower_noun + " to be selected.";
        return report;
    }
    report.title += ": " + text_or_empty (gncOwnerGetName (&owner));

    const std::vector<OwnerColumn> columns = owner_columns (options);
    const bool receivable = receivable_report (options.kind);
    const bool job = job_report (options.kind);

    /* Every A/Receivable (or A/Payable) account below the root, which is the
     * Scheme report's account selection. */
    std::vector<Account*> accounts;
    for (Account* account : descendants (root))
    {
        const GNCAccountType type = xaccAccountGetType (account);
        if (type == (receivable ? ACCT_TYPE_RECEIVABLE : ACCT_TYPE_PAYABLE))
            accounts.push_back (account);
    }
    if (accounts.empty ())
    {
        report.warning = "No valid account found.";
        report.subtitle =
            "This report requires a valid AP/AR account to be available.";
        return report;
    }

    const time64 aging_date = options.end.value_or (default_aging_date ());
    bool any_splits = false;
    int account_blocks = 0;

    for (Account* account : accounts)
    {
        std::vector<Split*> splits =
            owner_splits (account, owner, job, options.end);
        if (splits.empty ())
            continue;
        any_splits = true;

        if (account_blocks > 0)
        {
            /* More than one account: the Scheme report prints the account
             * name above its block. */
            Cell heading;
            heading.text = "Account: " + text_or_empty (xaccAccountGetName (
                                                    account));
            heading.css = "total-label-cell";
            heading.span = static_cast<int> (columns.size ());
            report.table.rows.push_back ({heading});
        }

        gnc_commodity* currency = xaccAccountGetCommodity (account);
        gnc_numeric total = gnc_numeric_zero ();
        gnc_numeric debit_total = gnc_numeric_zero ();
        gnc_numeric credit_total = gnc_numeric_zero ();
        gnc_numeric sale_total = gnc_numeric_zero ();
        gnc_numeric tax_total = gnc_numeric_zero ();
        bool printed = false;

        auto add_balance_row = [&] (gnc_numeric amount)
        {
            std::vector<Cell> row;
            for (OwnerColumn column : columns)
            {
                if (column == OwnerColumn::Balance)
                    row.push_back (number_cell (amount, currency));
                else if (column == OwnerColumn::Job)
                    /* The Scheme add-row call passes "Balance" as the job
                     * argument and an empty description, so the row is a
                     * blank description with the start date and the amount;
                     * the word only shows when the job column does. */
                    row.push_back (text_cell ("Balance"));
                else if (column == OwnerColumn::Date && options.start.has_value ())
                    row.push_back (text_cell (format_date (*options.start)));
                else
                    row.push_back (blank_cell (
                        column == OwnerColumn::Sale || column == OwnerColumn::Tax ||
                                column == OwnerColumn::Debits ||
                                column == OwnerColumn::Credits ||
                                column == OwnerColumn::Balance
                            ? "number-cell"
                            : ""));
            }
            report.table.rows.push_back (std::move (row));
        };

        for (Split* split : splits)
        {
            Transaction* txn = xaccSplitGetParent (split);
            const time64 date = xaccTransGetDate (txn);
            const gnc_numeric orig =
                xaccTransGetAccountAmount (txn, account);
            const gnc_numeric value =
                receivable ? orig : gnc_numeric_neg (orig);

            /* Transactions before the report's start date are the balance
             * brought forward, not rows. */
            if (options.start.has_value () && date < *options.start)
            {
                total = numeric_add (total, value);
                continue;
            }

            if (!printed)
            {
                if (options.show_balance && !zero_p (total))
                    add_balance_row (total);
                printed = true;
            }

            std::string reference;
            std::string type_text;
            std::string description;
            std::string job_text;
            std::optional<time64> due;
            gnc_numeric sale = gnc_numeric_zero ();
            gnc_numeric tax = gnc_numeric_zero ();

            if (xaccTransGetTxnType (txn) == TXN_TYPE_INVOICE)
            {
                GncInvoice* invoice = gncInvoiceGetInvoiceFromTxn (txn);
                reference = text_or_empty (gncInvoiceGetID (invoice));
                type_text = invoice_type (invoice);
                description = text_or_empty (xaccSplitGetMemo (split));
                job_text = invoice_job (invoice);
                const time64 due_date = gncInvoiceGetDateDue (invoice);
                if (due_date != 0)
                    due = due_date;
                const gnc_numeric subtotal =
                    gncInvoiceGetTotalSubtotal (invoice);
                const gnc_numeric tax_amount =
                    gncInvoiceGetTotalTax (invoice);
                if (gncInvoiceGetIsCreditNote (invoice))
                {
                    sale = gnc_numeric_neg (subtotal);
                    tax = gnc_numeric_neg (tax_amount);
                }
                else
                {
                    sale = subtotal;
                    tax = tax_amount;
                }
            }
            else
            {
                reference = num_action (txn, split, account);
                const bool refund =
                    receivable ? gnc_numeric_positive_p (orig)
                               : gnc_numeric_negative_p (orig);
                type_text = refund ? "Refund" : "Payment";
                description = payment_description (txn);
            }

            const gnc_numeric balance = numeric_add (total, value);

            std::vector<Cell> row;
            for (OwnerColumn column : columns)
            {
                switch (column)
                {
                case OwnerColumn::Date:
                    row.push_back (text_cell (format_date (date)));
                    break;
                case OwnerColumn::DueDate:
                    row.push_back (text_cell (due.has_value ()
                                                  ? format_date (*due)
                                                  : ""));
                    break;
                case OwnerColumn::Reference:
                    row.push_back (text_cell (reference));
                    break;
                case OwnerColumn::Type:
                    row.push_back (text_cell (type_text));
                    break;
                case OwnerColumn::Description:
                    row.push_back (text_cell (description));
                    break;
                case OwnerColumn::Job:
                    row.push_back (text_cell (job_text));
                    break;
                case OwnerColumn::Sale:
                    row.push_back (number_cell (sale, currency));
                    break;
                case OwnerColumn::Tax:
                    row.push_back (number_cell (tax, currency));
                    break;
                case OwnerColumn::Debits:
                    row.push_back (gnc_numeric_positive_p (orig)
                                       ? number_cell (orig, currency)
                                       : blank_cell ("number-cell"));
                    break;
                case OwnerColumn::Credits:
                    row.push_back (gnc_numeric_negative_p (orig)
                                       ? number_cell (gnc_numeric_neg (orig),
                                                      currency)
                                       : blank_cell ("number-cell"));
                    break;
                case OwnerColumn::Balance:
                    row.push_back (number_cell (balance, currency));
                    break;
                }
            }
            report.table.rows.push_back (std::move (row));

            total = numeric_add (total, value);
            if (gnc_numeric_positive_p (orig))
                debit_total = numeric_add (debit_total, orig);
            if (gnc_numeric_negative_p (orig))
                credit_total =
                    numeric_add (credit_total, gnc_numeric_neg (orig));
            if (xaccTransGetTxnType (txn) == TXN_TYPE_INVOICE)
            {
                sale_total = numeric_add (sale_total, sale);
                tax_total = numeric_add (tax_total, tax);
            }
        }

        /* A book whose only rows are before the start date still shows the
         * balance it brought forward. */
        if (!printed && options.show_balance && !zero_p (total))
            add_balance_row (total);

        const int prefix_count =
            (options.show_date ? 1 : 0) + (options.show_due_date ? 1 : 0) +
            (options.show_reference ? 1 : 0) + (options.show_type ? 1 : 0) +
            (options.show_job ? 1 : 0) + (options.show_description ? 1 : 0);
        const int left_count =
            prefix_count + (options.show_sale ? 1 : 0) +
            (options.show_tax ? 1 : 0) + (options.show_debits ? 1 : 0) +
            (options.show_credits ? 1 : 0);

        if (options.show_sale || options.show_tax || options.show_debits ||
            options.show_credits)
        {
            std::vector<Cell> row;
            row.push_back (text_cell ("Period Totals", "total-label-cell"));
            if (prefix_count > 1)
            {
                Cell filler = blank_cell ();
                filler.span = prefix_count - 1;
                row.push_back (filler);
            }
            if (options.show_sale)
                row.push_back (
                    number_cell (sale_total, currency, "total-number-cell"));
            if (options.show_tax)
                row.push_back (
                    number_cell (tax_total, currency, "total-number-cell"));
            if (options.show_debits)
                row.push_back (
                    number_cell (debit_total, currency, "total-number-cell"));
            if (options.show_credits)
                row.push_back (
                    number_cell (credit_total, currency, "total-number-cell"));
            if (options.show_balance)
                row.push_back (
                    number_cell (total, currency, "total-number-cell"));
            report.table.rows.push_back (std::move (row));
        }

        if (options.show_balance)
        {
            std::vector<Cell> row;
            row.push_back (text_cell (
                gnc_numeric_negative_p (total) ? "Total Credit" : "Total Due",
                "total-label-cell"));
            Cell amount = number_cell (total, currency, "total-number-cell");
            amount.span = left_count > 1 ? left_count - 1 : 1;
            row.push_back (amount);
            report.table.rows.push_back (std::move (row));
        }

        /* The aging row, in the Scheme bucket order: Pre-Payment, Current,
         * 0-30, 31-60, 61-90, 91+, Total.  It is flattened into the history
         * table right where the Scheme document nests its aging table, so a
         * multi-account report keeps each account's aging next to its rows. */
        std::vector<gnc_numeric> buckets =
            owner_aging (splits, aging_date, receivable);
        std::vector<Cell> aging_head;
        for (const char* heading :
             {"Pre-Payment", "Current", "0-30 days", "31-60 days",
              "61-90 days", "91+ days", "Total"})
            aging_head.push_back (text_cell (heading, "column-heading-left"));
        report.table.rows.push_back (std::move (aging_head));
        std::vector<Cell> aging_row;
        gnc_numeric aging_total = gnc_numeric_zero ();
        for (int bucket = prepayment_bucket; bucket >= 0; --bucket)
        {
            aging_row.push_back (number_cell (buckets[bucket], currency));
            aging_total = numeric_add (aging_total, buckets[bucket]);
        }
        aging_row.push_back (number_cell (aging_total, currency));
        report.table.rows.push_back (std::move (aging_row));
        ++account_blocks;
    }

    if (!any_splits)
    {
        report.warning = "No transactions found.";
        report.subtitle = "No matching transactions found";
        return report;
    }

    report.table.headings.clear ();
    for (OwnerColumn column : columns)
    {
        switch (column)
        {
        case OwnerColumn::Date: report.table.headings.push_back ("Date"); break;
        case OwnerColumn::DueDate:
            report.table.headings.push_back ("Due Date"); break;
        case OwnerColumn::Reference:
            report.table.headings.push_back ("Reference"); break;
        case OwnerColumn::Type: report.table.headings.push_back ("Type"); break;
        case OwnerColumn::Description:
            report.table.headings.push_back ("Description"); break;
        case OwnerColumn::Job: report.table.headings.push_back ("Job"); break;
        case OwnerColumn::Sale: report.table.headings.push_back ("Sale"); break;
        case OwnerColumn::Tax: report.table.headings.push_back ("Tax"); break;
        case OwnerColumn::Debits: report.table.headings.push_back ("Debits"); break;
        case OwnerColumn::Credits:
            report.table.headings.push_back ("Credits"); break;
        case OwnerColumn::Balance:
            report.table.headings.push_back ("Balance"); break;
        }
    }
    report.subtitle = "Date Range: " + format_date (
                          options.start.value_or (default_start_date ())) +
                      " - " +
                      format_date (options.end.value_or (aging_date));
    return report;
}

std::string
render_owner_report (const OwnerReport& report)
{
    std::ostringstream out;
    out << "<!DOCTYPE html>\n<html><head><meta charset='utf-8'><title>"
        << html_escape (report.title) << "</title><style>" << report_css ()
        << "</style></head><body>";
    if (!report.warning.empty ())
    {
        append_warning (out, report.title, report.subtitle);
        out << "<h3>" << html_escape (report.warning) << "</h3>";
        out << "</body></html>\n";
        return out.str ();
    }
    out << "<h3>" << html_escape (report.title) << "</h3>";
    if (!report.subtitle.empty ())
        out << "<h3>" << html_escape (report.subtitle) << "</h3>";
    append_table (out, report.table);
    out << "</body></html>\n";
    return out.str ();
}

/* -------------------------------------------------------- customer summary */

namespace
{

struct Amounts
{
    gnc_commodity* commodity = nullptr;
    gnc_numeric sales = gnc_numeric_zero ();
    gnc_numeric expense = gnc_numeric_zero ();
};

using AmountMap = std::map<std::string, Amounts>;

void
add_amount (AmountMap& map, gnc_commodity* commodity, gnc_numeric sales,
            gnc_numeric expense)
{
    const std::string key = text_or_empty (
        commodity != nullptr ? gnc_commodity_get_mnemonic (commodity) : "");
    Amounts& amounts = map[key];
    amounts.commodity = commodity;
    amounts.sales = numeric_add (amounts.sales, sales);
    amounts.expense = numeric_add (amounts.expense, expense);
}

/* The customer a split's document is billed to, through the end owner of the
 * invoice owner or the invoice's bill-to owner, as customer-summary.scm's two
 * query clauses do. */
bool
split_customer (Split* split, std::string& guid)
{
    GncInvoice* invoice =
        gncInvoiceGetInvoiceFromTxn (xaccSplitGetParent (split));
    if (invoice == nullptr)
        return false;

    const GncOwner* owner = gncInvoiceGetOwner (invoice);
    if (owner != nullptr &&
        gncOwnerGetType (gncOwnerGetEndOwner (owner)) == GNC_OWNER_CUSTOMER)
    {
        guid = owner_guid_text (gncOwnerGetEndOwner (owner));
        return !guid.empty ();
    }

    const GncOwner* billto = gncInvoiceGetBillTo (invoice);
    if (billto != nullptr && gncOwnerIsValid (billto) &&
        gncOwnerGetType (gncOwnerGetEndOwner (billto)) == GNC_OWNER_CUSTOMER)
    {
        guid = owner_guid_text (gncOwnerGetEndOwner (billto));
        return !guid.empty ();
    }
    return false;
}

struct SummaryRow
{
    std::string name;
    int commodity_index = 0;
    gnc_numeric profit = gnc_numeric_zero ();
    gnc_numeric sales = gnc_numeric_zero ();
    gnc_numeric expense = gnc_numeric_zero ();
    bool has_owner = false;
};

long
markup_percent (gnc_numeric profit, gnc_numeric sales)
{
    if (gnc_numeric_zero_p (sales))
        return 0;
    return std::lround (100.0 * gnc_numeric_to_double (profit) /
                        gnc_numeric_to_double (sales));
}

} // namespace

CustomerSummary
customer_summary (Account* root, const CustomerSummaryOptions& options)
{
    CustomerSummary report;
    report.title = "Customer Summary";
    if (root == nullptr)
    {
        report.warning = "No book is open";
        return report;
    }

    std::vector<Account*> income_accounts;
    std::vector<Account*> expense_accounts;
    std::vector<Account*> all_accounts;
    for (Account* account : descendants (root))
    {
        const GNCAccountType type = xaccAccountGetType (account);
        if (type == ACCT_TYPE_INCOME)
            income_accounts.push_back (account);
        else if (type == ACCT_TYPE_EXPENSE)
            expense_accounts.push_back (account);
    }
    all_accounts.reserve (income_accounts.size () + expense_accounts.size ());
    all_accounts.insert (all_accounts.end (), income_accounts.begin (),
                         income_accounts.end ());
    all_accounts.insert (all_accounts.end (), expense_accounts.begin (),
                         expense_accounts.end ());

    if (income_accounts.empty ())
    {
        report.warning = "No accounts selected";
        return report;
    }

    const std::optional<time64> start = options.start;
    const std::optional<time64> end = options.end;
    if (start.has_value () && end.has_value ())
        report.title += " " + format_date (*start) + " - " + format_date (*end);

    AmountMap other_amounts;
    std::map<std::string, AmountMap> customer_amounts;

    /* Every split of every income and expense account in the period,
     * excluding closing entries (the Scheme query's closing match). */
    for (Account* account : all_accounts)
    {
        const bool income = xaccAccountGetType (account) == ACCT_TYPE_INCOME;
        for (GList* node = xaccAccountGetSplitList (account); node != nullptr;
             node = node->next)
        {
            auto* split = static_cast<Split*> (node->data);
            Transaction* txn = xaccSplitGetParent (split);
            if (xaccTransGetIsClosingTxn (txn))
                continue;
            gnc_commodity* commodity = xaccTransGetCurrency (txn);
            const time64 date = xaccTransGetDate (txn);
            if ((start.has_value () && date < *start) ||
                (end.has_value () && date > *end))
                continue;
            const gnc_numeric value = xaccSplitGetValue (split);
            const gnc_numeric sales =
                income ? gnc_numeric_neg (value) : gnc_numeric_zero ();
            const gnc_numeric expense =
                income ? gnc_numeric_zero () : value;

            std::string guid;
            if (split_customer (split, guid))
                add_amount (customer_amounts[guid], commodity, sales, expense);
            else
                add_amount (other_amounts, commodity, sales, expense);
        }
    }

    /* The report's commodity columns are the accounts' commodities, as
     * gnc:accounts-get-commodities(all-accounts, #f) returns them. */
    std::vector<gnc_commodity*> commodities =
        account_commodities (all_accounts);
    auto commodity_index = [&commodities] (gnc_commodity* commodity)
    {
        for (size_t index = 0; index < commodities.size (); ++index)
            if (commodities[index] == commodity)
                return static_cast<int> (index);
        return -1;
    };
    for (const auto& customer : customer_amounts)
        for (const auto& entry : customer.second)
            if (commodity_index (entry.second.commodity) < 0)
                commodities.push_back (entry.second.commodity);
    for (const auto& entry : other_amounts)
        if (commodity_index (entry.second.commodity) < 0)
            commodities.push_back (entry.second.commodity);

    QofBook* book = gnc_account_get_book (root);
    GList* owners = gncBusinessGetOwnerList (book, GNC_ID_CUSTOMER,
                                             options.show_inactive ? TRUE
                                                                   : FALSE);

    /* customer-summary.scm's second branch: with no customer in the book the
     * report prints "No valid customer found." and no table at all -- not a
     * summary whose totals are zero. */
    if (owners == nullptr)
    {
        report.warning = "No valid customer found.";
        return report;
    }

    std::vector<SummaryRow> rows;
    for (GList* node = owners; node != nullptr; node = node->next)
    {
        auto* owner = static_cast<GncOwner*> (node->data);
        const std::string guid = owner_guid_text (owner);
        const std::string name = text_or_empty (gncOwnerGetName (owner));
        const AmountMap* map = nullptr;
        auto found = customer_amounts.find (guid);
        if (found != customer_amounts.end ())
            map = &found->second;

        for (size_t index = 0; index < commodities.size (); ++index)
        {
            Amounts amounts;
            amounts.commodity = commodities[index];
            if (map != nullptr)
            {
                const std::string key = text_or_empty (
                    gnc_commodity_get_mnemonic (commodities[index]));
                auto entry = map->find (key);
                if (entry != map->end ())
                    amounts = entry->second;
            }
            const gnc_numeric profit =
                numeric_sub (amounts.sales, amounts.expense);
            if (!options.show_zero_lines && zero_p (profit) &&
                zero_p (amounts.sales))
                continue;
            SummaryRow row;
            row.name = name;
            row.commodity_index = static_cast<int> (index);
            row.profit = profit;
            row.sales = amounts.sales;
            row.expense = amounts.expense;
            row.has_owner = true;
            rows.push_back (std::move (row));
        }
    }

    for (GList* node = owners; node != nullptr; node = node->next)
        gncOwnerFree (static_cast<GncOwner*> (node->data));
    g_list_free (owners);

    /* The "No Customer" rows: everything in the period that no customer on
     * the owner list owns. */
    for (size_t index = 0; index < commodities.size (); ++index)
    {
        const std::string key = text_or_empty (
            gnc_commodity_get_mnemonic (commodities[index]));
        auto entry = other_amounts.find (key);
        if (entry == other_amounts.end ())
            continue;
        const gnc_numeric profit =
            numeric_sub (entry->second.sales, entry->second.expense);
        if (zero_p (profit) && zero_p (entry->second.sales))
            continue;
        SummaryRow row;
        row.name = "No Customer";
        row.commodity_index = static_cast<int> (index);
        row.profit = profit;
        row.sales = entry->second.sales;
        row.expense = entry->second.expense;
        rows.push_back (std::move (row));
    }

    /* The Scheme default sort: customer name ascending, with "No Customer"
     * last because it has no owner report to link to. */
    std::stable_sort (rows.begin (), rows.end (),
                      [] (const SummaryRow& left, const SummaryRow& right)
                      {
                          if (!left.has_owner || !right.has_owner)
                          {
                              if (left.has_owner != right.has_owner)
                                  return left.has_owner;
                              return false;
                          }
                          const int comparison =
                              gnc_unicode_compare_accented_case_sensitive (
                                  left.name.c_str (), right.name.c_str ());
                          if (comparison != 0)
                              return comparison < 0;
                          return left.commodity_index < right.commodity_index;
                      });

    /* Total rows: every split in the period, which is the Scheme report's
     * toplevel-total-sales/toplevel-total-expense pair; the "No Customer"
     * line is part of the total even though it is not a customer. */
    std::vector<gnc_numeric> total_sales (commodities.size (),
                                          gnc_numeric_zero ());
    std::vector<gnc_numeric> total_expense (commodities.size (),
                                            gnc_numeric_zero ());
    for (const SummaryRow& row : rows)
    {
        total_sales[row.commodity_index] = numeric_add (
            total_sales[row.commodity_index], row.sales);
        total_expense[row.commodity_index] = numeric_add (
            total_expense[row.commodity_index], row.expense);
    }

    report.table.headings = {"Customer", "Profit", "Markup", "Sales"};
    if (options.show_expense)
        report.table.headings.push_back ("Expense");

    auto add_row = [&report, &options, &commodities] (const std::string& name,
                                                      int commodity_index,
                                                      gnc_numeric profit,
                                                      gnc_numeric sales,
                                                      gnc_numeric expense)
    {
        gnc_commodity* commodity = commodities[commodity_index];
        std::vector<Cell> row;
        row.push_back (text_cell (name));
        row.push_back (number_cell (profit, commodity));
        row.push_back (text_cell (
            std::to_string (markup_percent (profit, sales)) + "%",
            "number-cell"));
        row.push_back (number_cell (sales, commodity));
        if (options.show_expense)
            row.push_back (number_cell (expense, commodity));
        report.table.rows.push_back (std::move (row));
    };

    for (const SummaryRow& row : rows)
        add_row (row.name, row.commodity_index, row.profit, row.sales,
                 row.expense);

    const bool multiple_commodities = commodities.size () > 1;
    for (size_t index = 0; index < commodities.size (); ++index)
    {
        const gnc_numeric profit =
            numeric_sub (total_sales[index], total_expense[index]);
        std::string label = "Total";
        if (multiple_commodities)
            label += " (" + text_or_empty (gnc_commodity_get_mnemonic (
                                 commodities[index])) + ")";
        add_row (label, static_cast<int> (index), profit, total_sales[index],
                 total_expense[index]);
    }

    return report;
}

} // namespace report
} // namespace gnc
