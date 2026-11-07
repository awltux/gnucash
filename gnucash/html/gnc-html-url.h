/********************************************************************\
 * gnc-html-url.h -- URL type table, without any toolkit             *
 *                                                                   *
 * This program is free software; you can redistribute it and/or     *
 * modify it under the terms of the GNU General Public License as    *
 * published by the Free Software Foundation; either version 2 of    *
 * the License, or (at your option) any later version.               *
 *                                                                   *
 * This program is distributed in the hope that it will be useful,   *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of    *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the     *
 * GNU General Public License for more details.                      *
 *                                                                   *
 * You should have received a copy of the GNU General Public License *
 * along with this program; if not, contact:                         *
 *                                                                   *
 * Free Software Foundation           Voice:  +1-617-542-5942        *
 * 51 Franklin Street, Fifth Floor    Fax:    +1-617-542-2652        *
 * Boston, MA  02110-1301,  USA       gnu@gnu.org                    *
 *                                                                   *
\********************************************************************/

/** @file gnc-html-url.h
 *  @brief The URL type table and gnc_build_url(), with no toolkit behind them.
 *
 *  Reports build links into their HTML (accounts, splits, prices) and those
 *  links are URLs of GnuCash's own types. The table that maps a type to its
 *  protocol used to sit inside the GTK/WebKit library, which meant a headless
 *  report engine could not build a URL at all. It lives here instead, so the
 *  report library can link it and a GTK-free build still produces the same
 *  documents.
 */

#ifndef GNC_HTML_URL_H
#define GNC_HTML_URL_H

#include <glib.h>

#include "gnc-html-extras.h"

#ifdef __cplusplus
#define NOEXCEPT noexcept
extern "C"
{
#else
#define NOEXCEPT
#endif

/** Protocol to URL type map, filled by gnc_html_initialize().
 *  NULL until then; every lookup tolerates that. */
extern GHashTable* gnc_html_proto_to_type_hash;

/** Registers a type/protocol pair. Returns FALSE if the type already exists. */
gboolean gnc_html_register_urltype( URLType type, const gchar* protocol ) NOEXCEPT;

/** Fills the URL type table with GnuCash's own types. */
void gnc_html_initialize( void ) NOEXCEPT;

#ifdef __cplusplus
}
#endif

#endif // GNC_HTML_URL_H
