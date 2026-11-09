/********************************************************************\
 * gnc-owner.hpp -- the owner reports' model (no Guile)             *
 *                                                                  *
 * Customer Report, Vendor Report, Employee Report and Job Report   *
 * are one Scheme renderer with four registrations: an owner, a     *
 * date range and the A/R or A/P account's transaction history.     *
 * Customer Summary is a different question (profit per customer    *
 * over the income and expense accounts), so it has its own model.  *
 * Both read the engine here, as ordinary C++ values, and the HTML  *
 * lives in gnc-owner.cpp.  Nothing in this file needs Guile or a   *
 * toolkit.                                                         *
 *                                                                  *
 * This program is free software; you can redistribute it and/or    *
 * modify it under the terms of the GNU General Public License as   *
 * published by the Free Software Foundation; either version 2 of    *
 * the License, or (at your option) any later version.              *
\********************************************************************/

#ifndef GNC_OWNER_HPP
#define GNC_OWNER_HPP

#include <optional>
#include <string>
#include <vector>

#include <gnc-numeric.h>
#include <gncOwner.h>

#include "gnc-report.hpp"

namespace gnc
{
namespace report
{

/* ------------------------------------------------------------ owner report */

/** The five Scheme names this file implements.  Customer Report, Vendor
 *  Report, Employee Report and Job Report are one renderer registered four
 *  times (new-owner-report.scm); Customer Summary is customer-summary.scm.
 *
 *  The owner report covers one owner's documents and payments in the A/Payable
 *  or A/Receivable accounts.  The owner type picks both the account type
 *  (customers and jobs use A/Receivable, vendors and employees A/Payable) and
 *  the matching rule: a customer/vendor/employee report matches an invoice
 *  owner's *end* owner, so a job's invoice appears on its customer's report
 *  too; the Job Report matches the invoice's own job GUID. */
bool is_owner_report (const std::string& name);

/** The owner kind a report name selects.  False for any other report. */
bool owner_report_kind (const std::string& name, OwnerKind& kind);

/** The Scheme option label / report noun: "Customer", "Vendor", ... */
const char* owner_kind_name (OwnerKind kind);

/** Resolve an owner by GUID (or, as a convenience, by its id or name) in the
 *  book below `root`.  False when nothing matches.  `text` is what the
 *  "owner" option carries: the Scheme option serialises a GncOwner as the
 *  owner object's GUID, and the native route takes that same GUID. */
bool owner_lookup (Account* root, OwnerKind kind, const std::string& text,
                   GncOwner& owner);

/** A rendered owner report: the history table and the document title/subtitle.
 *  The aging buckets are rows at the end of the history table (the Scheme
 *  document nests them in a cell there; flattening them keeps multiple account
 *  blocks in the same order).  A warning means the Scheme report would render
 *  an explanation instead. */
struct OwnerReport
{
    CellTable table;
    std::string title;
    std::string subtitle;
    std::string warning;
};

/** Build the report for one owner.  `options.owner_guid` empty is the Scheme
 *  "no valid owner selected" case, not an error; `options.start` and
 *  `options.end` are the report's From/To bounds (unset means no bound).
 *  The table is the Scheme history table: Date, Due Date, Reference, Type,
 *  Description, Debits, Credits, Balance, with a "Balance" row for the
 *  brought-forward total, a "Period Totals" row and a "Total Due"/"Total
 *  Credit" row.  The aging tables print the Scheme bucket row from the same
 *  lot-based walk the aging reports use. */
OwnerReport owner_report (Account* root, const OwnerReportOptions& options);

/** Render an OwnerReport as a standalone HTML document. */
std::string render_owner_report (const OwnerReport& report);

/* -------------------------------------------------------- customer summary */

/** A rendered Customer Summary: one row per customer (per commodity) with
 *  Profit, Markup, Sales and Expense, the "No Customer" row for income or
 *  expense splits that no customer owns, a rule and the Total row. */
struct CustomerSummary
{
    CellTable table;
    std::string title;
    std::string warning;
};

/** Build the Customer Summary.  `start`/`end` bound the period (unset means
 *  no bound).  The default scope is every INCOME and EXPENSE account below
 *  `root`, which is the Scheme option's default selection. */
CustomerSummary customer_summary (Account* root,
                                  const CustomerSummaryOptions& options);

/* --------------------------------------------------------- option plumbing */

/** Apply one of the owner report's boolean display options by descriptor
 *  name.  False for a name the report does not have, so the route can reject
 *  it rather than ignore it. */
bool set_owner_report_option (OwnerReportOptions& options,
                              const std::string& name, bool value);

/** Apply one of the Customer Summary's boolean options by descriptor name. */
bool set_customer_summary_option (CustomerSummaryOptions& options,
                                  const std::string& name, bool value);

} // namespace report
} // namespace gnc

#endif
