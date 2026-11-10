/********************************************************************\
 * report-spec.hpp -- designer reports, stored outside the book      *
 *                                                                   *
 * A saved report is a JSON document (query, columns, grouping and    *
 * options) kept beside the web UI's data, never in the .gnucash     *
 * file: the web tier opens the book read-only.  This module owns    *
 * the schema, the validation and the file; the native report layer  *
 * owns the rendering.                                               *
 *                                                                   *
 * This program is free software; you can redistribute it and/or     *
 * modify it under the terms of the GNU General Public License as    *
 * published by the Free Software Foundation; either version 2 of    *
 * the License, or (at your option) any later version.               *
\********************************************************************/

#ifndef GNC_WEB_REPORT_SPEC_HPP
#define GNC_WEB_REPORT_SPEC_HPP

#include <string>
#include <vector>

#include <Account.h>

namespace web
{

/* The query half of a spec.  Dates are ISO strings so the file stays
 * human-readable and stable; an empty string means "not set". */
struct SpecQuery
{
    std::string start;
    std::string end;
    /** "", "this-year", "last-year" or "all".  Mutually exclusive with the
     *  literal dates, exactly like the /report route's period option. */
    std::string date_preset;
    std::string currency;
    /** Account GUIDs; empty means every account in the book. */
    std::vector<std::string> accounts;
};

struct ReportSpec
{
    std::string id;
    std::string name;
    std::string title;
    /** Optional one-line explanation shown in the report list.  Built-ins
     *  use it to say which accounts they are scoped to. */
    std::string description;
    /** Lowercase slugs ([a-z0-9-], 1-24 characters) used to group reports.
     *  Stored with the spec, so they are shared by every browser on the
     *  book, unlike the localStorage saved views. */
    std::vector<std::string> tags;
    /** "accounts" (the default, and what a spec without the field means),
     *  "splits", "balance-sheet", "invoices" or "lots".  A split spec's
     *  `columns` are split columns, its rows are the book's individual splits,
     *  and `split_grouping` decides their order and sections.  An invoices
     *  spec's rows are the aging reports' owners or the documents they age, and
     *  `invoice_owner`, `invoice_grouping` and `bucket_basis` say which.  A
     *  lots spec is the Lot Viewer's table: one row per transaction, one
     *  column per lot the transactions touch. */
    std::string source = "accounts";
    std::vector<std::string> columns;
    /** "none" (the default), "account", "account-code" or "transaction"; only
     *  meaningful for `source` "splits". */
    std::string split_grouping = "none";
    /** "owner" (the default) or "document"; only meaningful for `source`
     *  "invoices".  The owner rows are the aging report's own table -- one row
     *  per owner with the six buckets and the row total -- and the document
     *  rows are the invoices, credit notes and payments whose amounts make
     *  those buckets. */
    std::string invoice_grouping = "owner";
    /** "receivable" (the default) or "payable"; only meaningful for `source`
     *  "invoices".  It chooses the report: the Receivable Aging or the Payable
     *  Aging. */
    std::string invoice_owner = "receivable";
    /** "due" (the default) or "posted"; only meaningful for `source`
     *  "invoices".  Which invoice date decides the bucket, the report's "Due
     *  or Post Date" option. */
    std::string bucket_basis = "due";
    /** The Lot Viewer's "Description Filter"; only meaningful for `source`
     *  "lots".  Only transactions whose description contains this string are
     *  listed, and empty lists every transaction in the period. */
    std::string description_filter;
    std::string grouping = "none";
    std::string period_unit = "year";
    /** "activity" (the default) or "cumulative"; cumulative turns a period
     *  grouping into balances as of each period's end. */
    std::string period_mode = "activity";
    SpecQuery query;
    /** Account type names (account_type_vocabulary()) that further scope the
     *  query.  Empty means every type. */
    std::vector<std::string> account_types;
    /** "", or a chart type ("bar" or "line"). */
    std::string chart;
    /** One or two amount column names the chart plots, in series order. */
    std::vector<std::string> chart_series;
};

/** The two columns a balance-sheet spec always has, in the report's order
 *  ("account-name", "balance").  They are the report's columns, not a choice:
 *  spec_from_json() accepts only these on a balance-sheet spec and
 *  /api/spec/options publishes them, so both read this one list. */
const std::vector<std::string>& balance_sheet_columns ();

/** The columns an invoices spec has, in display order, for each of its two row
 *  sets: the aging report's own owner rows (Company, the six buckets, Total)
 *  and the document rows (the document's fields and the six buckets).  Like
 *  balance_sheet_columns(), one list serves validation, serialization and
 *  /api/spec/options, so the three cannot disagree. */
const std::vector<std::string>& invoice_owner_columns ();
const std::vector<std::string>& invoice_document_columns ();

/** The columns a lots spec has, in display order: the Lot Viewer's row labels,
 *  the middle column that stands for one column per lot the transactions
 *  touch, and the transaction's remaining splits.  The middle entry is a
 *  placeholder, not a column: the report writes one column per lot, so the
 *  list is fixed and the lot columns are the book's.  Like the other lists,
 *  one definition serves validation, serialization and /api/spec/options. */
const std::vector<std::string>& lots_columns ();

/** Where saved reports live: $GNC_WEBUI_REPORTS when set (the tests use a
 *  temporary file), else $XDG_DATA_HOME/gnucash/webui/reports.json, else
 *  ~/.local/share/gnucash/webui/reports.json. */
std::string spec_store_path ();

/** Read the store.  A missing file is an empty list, not an error; a
 *  malformed file is an error and never silently discarded. */
bool load_specs (std::vector<ReportSpec>& specs, std::string& error);

/** Write the store atomically (temporary file + rename). */
bool save_specs (const std::vector<ReportSpec>& specs, std::string& error);

/** Validate a create/preview body against the open book.  The spec's id is
 *  left untouched (the caller assigns one when saving).  Every failure names
 *  the field and the problem, ready to be a 400 body. */
bool spec_from_json (const std::string& body, Account* root, ReportSpec& spec,
                     std::string& error);

std::string spec_to_json (const ReportSpec& spec, bool builtin = false);

/** True when the spec passes both list filters.  "tag" is an exact,
 *  case-insensitive tag match; "text" is a case-insensitive substring of the
 *  name, the description or any tag.  An empty filter always matches, and the
 *  two compose (AND). */
bool spec_matches (const ReportSpec& spec, const std::string& tag,
                   const std::string& text);

/** The saved reports, plus the built-in specs under "builtin", filtered by
 *  the optional tag/text arguments.  Built-ins are not stored: they are code,
 *  so they appear in every book and survive a deleted or missing store. */
std::string specs_to_json (const std::vector<ReportSpec>& specs,
                           const std::string& tag = std::string (),
                           const std::string& text = std::string ());

/** The built-in report specs, in report-list order.  Their ids are stable
 *  ("builtin-...") and their scope is by account type, never by GUID, so the
 *  same spec works in every book. */
const std::vector<ReportSpec>& builtin_specs ();
std::vector<std::string> builtin_spec_names ();
bool builtin_spec_by_id (const std::string& id, ReportSpec& spec);

/** What the designer can honestly say about a native (C++) report.
 *
 *  A native report has no spec: its query and columns are written in C++.
 *  This describes where that implementation lives, whether a spec can stand in
 *  for the report, and the starting-point spec when one can.  It never invents
 *  a definition: `note` says plainly why there is no spec when there is none,
 *  and when a starting point exists `equivalence` says whether its cells match
 *  the report or only approximate it. */
struct NativeDesignerInfo
{
    std::string report;
    /** The C++ source file and the function that builds the report. */
    std::string source_file;
    std::string source_symbol;
    /** "exact", "approximate" or "none".  "exact" is only used where the
     *  starting spec renders the report's own cells; "approximate" always
     *  comes with a note saying how it differs. */
    std::string equivalence;
    /** What the implementation is, or why a spec cannot stand in for it. */
    std::string note;
    /** True when `spec` below is a starting point the designer may populate. */
    bool has_spec = false;
    ReportSpec spec;
};

/** Describe a native report for the designer.  False when the native layer
 *  does not know the report (there is nothing honest to say about it). */
bool native_designer_info (const std::string& report, NativeDesignerInfo& info);

/** The JSON body of /api/report/<name>/designer. */
std::string native_designer_json (const NativeDesignerInfo& info);

/** Render a validated spec through the native report layer. */
bool render_spec_html (const ReportSpec& spec, Account* root,
                       std::string& html, std::string& error);

} // namespace web

#endif
