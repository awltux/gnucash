/********************************************************************\
 * gnc-html-url.h -- URL registry, URL building and URL parsing      *
 * Copyright (C) 2026 GnuCash Developers                             *
 *                                                                  *
 * This program is free software; you can redistribute it and/or    *
 * modify it under the terms of the GNU General Public License as   *
 * published by the Free Software Foundation; either version 2 of   *
 * the License, or (at your option) any later version.              *
 *                                                                  *
 * This program is distributed in the hope that it will be useful,  *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of   *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the    *
 * GNU General Public License for more details.                     *
 *                                                                  *
 * You should have received a copy of the GNU General Public License*
 * along with this program; if not, contact:                        *
 *                                                                  *
 * Free Software Foundation           Voice:  +1-617-542-5942       *
 * 51 Franklin Street, Fifth Floor    Fax:    +1-617-542-2652       *
 * Boston, MA  02110-1301,  USA       gnu@gnu.org                   *
\********************************************************************/

/** @file gnc-html-url.h
 *  @brief URL type registry, URL building and URL parsing.
 *
 *  gnc-html.h declares the GtkBin-derived GncHtml widget, but turning a URL
 *  string into a (type, location, label) triple needs no widget at all, and a
 *  front end that is not GTK needs it just as much. This header is that part
 *  of the interface, with the base type and base location passed in explicitly
 *  instead of being read from widget-private state.
 *
 *  Nothing here includes GTK or WebKit.
 */

#ifndef GNC_HTML_URL_H
#define GNC_HTML_URL_H

#include <glib.h>

#include "gnc-html-extras.h"

#ifdef __cplusplus
extern "C"
{
#endif

/**
 * Registers a URLType and its protocol.
 *
 * @param type New URL type
 * @param protocol Protocol; empty string if the type has no protocol
 * @return TRUE if successful, FALSE if the type already exists or protocol is nullptr
 */
gboolean gnc_html_register_urltype(URLType type, const gchar* protocol) NOEXCEPT;

/**
 * Looks up the URL type registered for a protocol.
 *
 * @param protocol Protocol to resolve, for example "gnc-report"
 * @return The registered URLType, or URL_TYPE_OTHER when nothing matches
 */
URLType gnc_html_url_type_for_protocol(const gchar* protocol) NOEXCEPT;

/**
 * Classifies a URL and resolves its location against a base.
 *
 * @param url           URL to parse
 * @param base_type     URL type to assume when the URL carries no protocol
 * @param base_location Directory or document the URL is relative to; may be nullptr
 * @param url_location  Set to the resolved location; free with g_free()
 * @param url_label     Set to the anchor label, or nullptr; free with g_free()
 * @return The resolved URL type
 */
URLType gnc_html_url_parse(const gchar* url, URLType base_type,
                           const gchar* base_location,
                           gchar** url_location, gchar** url_label) NOEXCEPT;

#ifdef __cplusplus
}
#endif

#endif /* GNC_HTML_URL_H */
