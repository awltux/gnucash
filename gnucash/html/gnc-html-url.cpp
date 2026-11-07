/********************************************************************\
 * gnc-html-url.cpp -- URL type table, without any toolkit           *
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

#include <config.h>

#include <string.h>

#include "gnc-html-url.h"

/* hashes for URLType -> protocol and protocol -> URLType */
static GHashTable * gnc_html_type_to_proto_hash = nullptr;
GHashTable * gnc_html_proto_to_type_hash = nullptr;

/* Register the URLType if it doesn't already exist.
 * Returns TRUE if successful, FALSE if the type already exists.
 */
gboolean
gnc_html_register_urltype( URLType type, const char *protocol ) noexcept
{
    if (!protocol) return FALSE;

    if (!gnc_html_type_to_proto_hash)
    {
        gnc_html_type_to_proto_hash = g_hash_table_new (g_str_hash, g_str_equal);
        gnc_html_proto_to_type_hash = g_hash_table_new (g_str_hash, g_str_equal);
    }

    char *lc_type  = g_ascii_strdown (type, -1);
    if (g_hash_table_lookup (gnc_html_type_to_proto_hash, lc_type))
    {
        g_free (lc_type);
        return FALSE;
    }

    char *lc_proto = g_ascii_strdown (protocol, -1);
    g_hash_table_insert (gnc_html_type_to_proto_hash, lc_type, static_cast<gpointer>(lc_proto));
    if (*lc_proto)
        g_hash_table_insert (gnc_html_proto_to_type_hash, static_cast<gpointer>(lc_proto), lc_type);

    return TRUE;
}

void
gnc_html_initialize( void ) noexcept
{
    static struct
    {
        URLType	type;
        const char *protocol;
    } types[] =
    {
        { URL_TYPE_FILE, "file" },
        { URL_TYPE_JUMP, "" },
        { URL_TYPE_HTTP, "http" },
        { URL_TYPE_FTP, "ftp" },
        { URL_TYPE_SECURE, "https" },
        { URL_TYPE_REGISTER, "gnc-register" },
        { URL_TYPE_ACCTTREE, "gnc-acct-tree" },
        { URL_TYPE_REPORT, "gnc-report" },
        { URL_TYPE_OPTIONS, "gnc-options" },
        { URL_TYPE_SCHEME, "gnc-scm" },
        { URL_TYPE_HELP, "gnc-help" },
        { URL_TYPE_XMLDATA, "gnc-xml" },
        { URL_TYPE_PRICE, "gnc-price" },
        { URL_TYPE_BUDGET, "gnc-budget" },
        { URL_TYPE_OTHER, "" }
    };

    for (const auto& elem : types)
    {
        (void) gnc_html_register_urltype (elem.type, elem.protocol);
    }
}

/**
 * Creates a new HTML url.
 *
 * @param type URL type
 * @param location URL location
 * @param label URL label (optional)
 * @return Newly created URL.  This string must be freed by the caller.
 */
gchar*
gnc_build_url( URLType type, const gchar* location, const gchar* label ) noexcept
{
    char *lc_type = g_ascii_strdown (type, -1);
    const gpointer p = gnc_html_type_to_proto_hash
        ? g_hash_table_lookup (gnc_html_type_to_proto_hash, lc_type)
        : nullptr;
    const char *type_name = static_cast<const char *>(p);
    g_free (static_cast<gpointer>(lc_type));
    if (!type_name)
        type_name = "";

    if (label)
    {
        return g_strdup_printf("%s%s%s#%s", type_name, (*type_name ? ":" : ""),
                               (location ? location : ""),
                               label ? label : "");
    }
    else
    {
        return g_strdup_printf("%s%s%s", type_name, (*type_name ? ":" : ""),
                               (location ? location : ""));
    }
}
