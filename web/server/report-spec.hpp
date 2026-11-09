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
    std::vector<std::string> columns;
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
/** The saved reports, plus the built-in specs under "builtin".  Built-ins are
 *  not stored: they are code, so they appear in every book and survive a
 *  deleted or missing store. */
std::string specs_to_json (const std::vector<ReportSpec>& specs);

/** The built-in report specs, in report-list order.  Their ids are stable
 *  ("builtin-...") and their scope is by account type, never by GUID, so the
 *  same spec works in every book. */
const std::vector<ReportSpec>& builtin_specs ();
std::vector<std::string> builtin_spec_names ();
bool builtin_spec_by_id (const std::string& id, ReportSpec& spec);

/** Render a validated spec through the native report layer. */
bool render_spec_html (const ReportSpec& spec, Account* root,
                       std::string& html, std::string& error);

} // namespace web

#endif
