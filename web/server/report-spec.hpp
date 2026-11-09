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
    std::vector<std::string> columns;
    std::string grouping = "none";
    std::string period_unit = "year";
    SpecQuery query;
    /** "", or a chart type ("bar"). */
    std::string chart;
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

std::string spec_to_json (const ReportSpec& spec);
std::string specs_to_json (const std::vector<ReportSpec>& specs);

/** Render a validated spec through the native report layer. */
bool render_spec_html (const ReportSpec& spec, Account* root,
                       std::string& html, std::string& error);

} // namespace web

#endif
