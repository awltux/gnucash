/********************************************************************\
 * gnc-html-url.cpp -- URL registry, URL building and URL parsing    *
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

/** @file gnc-html-url.cpp
 *  @brief URL type registry, URL building and URL parsing.
 *
 *  Moved out of gnc-html.cpp so that the URL bookkeeping can be shared with a
 *  presentation layer that is not GTK. Behaviour is unchanged; the only
 *  difference is that base type and base location arrive as parameters.
 */

#include <regex.h>
#include <cstring>

#include <glib.h>

#include "gnc-engine.h"
#include "qoflog.h"
#include "gnc-html-extras.h"
#include "gnc-html-url.h"

static QofLogModule log_module = GNC_MOD_HTML;

/* hashes for URLType -> protocol and protocol -> URLType */
static GHashTable* gnc_html_type_to_proto_hash = nullptr;
static GHashTable* gnc_html_proto_to_type_hash = nullptr;

static void
ensure_urltype_tables()
{
    if (gnc_html_type_to_proto_hash == nullptr)
    {
        gnc_html_type_to_proto_hash = g_hash_table_new(g_str_hash, g_str_equal);
        gnc_html_proto_to_type_hash = g_hash_table_new(g_str_hash, g_str_equal);
    }
}

/* Register the URLType if it doesn't already exist.
 * Returns TRUE if successful, FALSE if the type already exists.
 */
gboolean
gnc_html_register_urltype(URLType type, const char* protocol) noexcept
{
    if (!protocol) return FALSE;

    ensure_urltype_tables();

    char* lc_type = g_ascii_strdown(type, -1);
    if (g_hash_table_lookup(gnc_html_type_to_proto_hash, lc_type))
    {
        g_free(lc_type);
        return FALSE;
    }

    char* lc_proto = g_ascii_strdown(protocol, -1);
    g_hash_table_insert(gnc_html_type_to_proto_hash, lc_type,
                        static_cast<gpointer>(lc_proto));
    if (*lc_proto)
        g_hash_table_insert(gnc_html_proto_to_type_hash,
                            static_cast<gpointer>(lc_proto), lc_type);

    return TRUE;
}

URLType
gnc_html_url_type_for_protocol(const gchar* protocol) noexcept
{
    if (protocol == nullptr)
        return URL_TYPE_OTHER;

    ensure_urltype_tables();

    const gpointer p =
        g_hash_table_lookup(gnc_html_proto_to_type_hash, protocol);
    URLType type = static_cast<const char*>(p);
    return type != nullptr ? type : URL_TYPE_OTHER;
}

static char*
extract_machine_name(const gchar* path)
{
    constexpr gchar machine_rexp[] = "^(//[^/]*)/*(.*)?$";
    regex_t compiled_m;
    constexpr size_t MATCH_LEN = 4;
    regmatch_t match[MATCH_LEN];
    gchar* machine = nullptr;

    if (path == nullptr) return nullptr;

    regcomp(&compiled_m, machine_rexp, REG_EXTENDED);

    /* step 1: split the machine name away from the path
     * components */
    if (!regexec(&compiled_m, path, MATCH_LEN, match, 0))
    {
        /* $1 is the machine name */
        if (match[1].rm_so != -1)
        {
            machine = g_strndup(path + match[1].rm_so,
                                match[1].rm_eo - match[1].rm_so);
        }
    }
    regfree(&compiled_m);
    return machine;
}

/********************************************************************
 * gnc_html_url_parse
 * this takes a URL and determines the protocol type, location, and
 * possible anchor name from the URL.
 ********************************************************************/

URLType
gnc_html_url_parse(const gchar* url, URLType base_type,
                   const gchar* base_location,
                   gchar** url_location, gchar** url_label) noexcept
{
    constexpr gchar uri_rexp[] = "^(([^:][^:]+):)?([^#]+)?(#(.*))?$";
    regex_t compiled;
    constexpr size_t MATCH_LEN = 6;
    regmatch_t match[MATCH_LEN];
    gchar* protocol = nullptr;
    gchar* path = nullptr;
    gchar* label = nullptr;
    bool found_protocol = false;
    bool found_path = false;
    bool found_label = false;
    URLType retval;

    g_return_val_if_fail(url != nullptr, URL_TYPE_OTHER);

    DEBUG("parsing %s, base_location %s", url,
          base_location ? base_location : "(null base_location)");

    regcomp(&compiled, uri_rexp, REG_EXTENDED);

    if (!regexec(&compiled, url, MATCH_LEN, match, 0))
    {
        if (match[2].rm_so != -1)
        {
            protocol = g_new0(gchar, match[2].rm_eo - match[2].rm_so + 1);
            strncpy(protocol, url + match[2].rm_so, match[2].rm_eo - match[2].rm_so);
            protocol[match[2].rm_eo - match[2].rm_so] = 0;
            found_protocol = true;
        }
        if (match[3].rm_so != -1)
        {
            path = g_new0(gchar, match[3].rm_eo - match[3].rm_so + 1);
            strncpy(path, url + match[3].rm_so, match[3].rm_eo - match[3].rm_so);
            path[match[3].rm_eo - match[3].rm_so] = 0;
            found_path = true;
        }
        if (match[5].rm_so != -1)
        {
            label = g_new0(gchar, match[5].rm_eo - match[5].rm_so + 1);
            strncpy(label, url + match[5].rm_so, match[5].rm_eo - match[5].rm_so);
            label[match[5].rm_eo - match[5].rm_so] = 0;
            found_label = true;
        }
    }

    regfree(&compiled);

    if (found_protocol)
    {
        retval = gnc_html_url_type_for_protocol(protocol);
        if (!g_strcmp0(retval, URL_TYPE_OTHER))
            PWARN("unhandled URL type for '%s'", url);
    }
    else if (found_label && !found_path)
    {
        retval = URL_TYPE_JUMP;
    }
    else
    {
        retval = base_type ? base_type : URL_TYPE_FILE;
    }

    g_free(protocol);

    if (!g_strcmp0(retval, URL_TYPE_FILE))
    {
        if (!found_protocol && path && base_location)
        {
            if (g_path_is_absolute(path))
            {
                *url_location = g_strdup(path);
            }
            else
            {
                *url_location = g_build_filename(base_location, path, nullptr);
            }
            g_free(path);
        }
        else
        {
            *url_location = g_strdup(path);
            g_free(path);
        }
    }
    else if (!g_strcmp0(retval, URL_TYPE_JUMP))
    {
        *url_location = nullptr;
        g_free(path);
    }
    else
    {
        /* case URL_TYPE_OTHER: */

        if (!found_protocol && path && base_location)
        {
            if (g_path_is_absolute(path))
            {
                *url_location = g_build_filename(extract_machine_name(base_location),
                                                 path, nullptr);
            }
            else
            {
                *url_location = g_build_filename(base_location, path, nullptr);
            }
            g_free(path);
        }
        else
        {
            *url_location = g_strdup(path);
            g_free(path);
        }
    }

    *url_label = label;
    return retval;
}

/**
 * Creates a new HMTL url.
 *
 * @param type URL type
 * @param location URL location
 * @param label URL label (optional)
 * @return Newly created URL.  This string must be freed by the caller.
 */
gchar*
gnc_build_url(URLType type, const gchar* location, const gchar* label) noexcept
{
    DEBUG(" ");

    ensure_urltype_tables();

    char* lc_type = g_ascii_strdown(type, -1);
    const gpointer p = g_hash_table_lookup(gnc_html_type_to_proto_hash, lc_type);
    const char* type_name = static_cast<const char*>(p);
    g_free(static_cast<gpointer>(lc_type));
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
