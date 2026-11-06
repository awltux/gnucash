/********************************************************************
 * gtest-gnc-html-url.cpp -- tests for the widget-free URL layer     *
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
 ********************************************************************/

#include <gtest/gtest.h>

#include <glib.h>

#include "gnc-html-url.h"

namespace
{

class GncHtmlUrlTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        // Registering an existing type is a no-op, so this stays safe when
        // several tests run in one process.
        gnc_html_register_urltype(URL_TYPE_FILE, "file");
        gnc_html_register_urltype(URL_TYPE_JUMP, "");
        gnc_html_register_urltype("test-type", "test-proto");
    }
};

TEST_F(GncHtmlUrlTest, registering_twice_reports_the_duplicate)
{
    EXPECT_FALSE(gnc_html_register_urltype("test-type", "another-proto"));
}

TEST_F(GncHtmlUrlTest, protocol_lookup_returns_the_registered_type)
{
    EXPECT_STREQ("test-type", gnc_html_url_type_for_protocol("test-proto"));
}

TEST_F(GncHtmlUrlTest, unknown_protocol_maps_to_other)
{
    EXPECT_STREQ(URL_TYPE_OTHER, gnc_html_url_type_for_protocol("no-such-protocol"));
}

TEST_F(GncHtmlUrlTest, null_protocol_maps_to_other)
{
    EXPECT_STREQ(URL_TYPE_OTHER, gnc_html_url_type_for_protocol(nullptr));
}

TEST_F(GncHtmlUrlTest, unknown_protocol_in_a_url_maps_to_other)
{
    gchar* location = nullptr;
    gchar* label = nullptr;

    const URLType type =
        gnc_html_url_parse("no-such-protocol:thing", nullptr, nullptr,
                           &location, &label);

    EXPECT_STREQ(URL_TYPE_OTHER, type);
    EXPECT_STREQ("thing", location);
    EXPECT_EQ(nullptr, label);
    g_free(location);
}

TEST_F(GncHtmlUrlTest, label_without_a_path_is_a_jump)
{
    gchar* location = nullptr;
    gchar* label = nullptr;

    const URLType type =
        gnc_html_url_parse("#section", nullptr, nullptr, &location, &label);

    EXPECT_STREQ(URL_TYPE_JUMP, type);
    EXPECT_EQ(nullptr, location);
    EXPECT_STREQ("section", label);
    g_free(label);
}

TEST_F(GncHtmlUrlTest, relative_file_resolves_against_the_base)
{
    gchar* location = nullptr;
    gchar* label = nullptr;

    const URLType type =
        gnc_html_url_parse("child.html", URL_TYPE_FILE, "/base",
                           &location, &label);

    EXPECT_STREQ(URL_TYPE_FILE, type);
    EXPECT_STREQ("/base/child.html", location);
    EXPECT_EQ(nullptr, label);
    g_free(location);
}

TEST_F(GncHtmlUrlTest, absolute_file_ignores_the_base)
{
    gchar* location = nullptr;
    gchar* label = nullptr;

    const URLType type =
        gnc_html_url_parse("/elsewhere/abs.html", URL_TYPE_FILE, "/base",
                           &location, &label);

    EXPECT_STREQ(URL_TYPE_FILE, type);
    EXPECT_STREQ("/elsewhere/abs.html", location);
    g_free(location);
}

TEST_F(GncHtmlUrlTest, no_base_means_file_and_no_resolution)
{
    gchar* location = nullptr;
    gchar* label = nullptr;

    const URLType type =
        gnc_html_url_parse("child.html", nullptr, nullptr, &location, &label);

    EXPECT_STREQ(URL_TYPE_FILE, type);
    EXPECT_STREQ("child.html", location);
    g_free(location);
}

TEST_F(GncHtmlUrlTest, build_url_emits_the_protocol_prefix)
{
    gchar* without_label = gnc_build_url("test-type", "widgets", nullptr);
    EXPECT_STREQ("test-proto:widgets", without_label);
    g_free(without_label);

    gchar* with_label = gnc_build_url("test-type", "widgets", "here");
    EXPECT_STREQ("test-proto:widgets#here", with_label);
    g_free(with_label);
}

TEST_F(GncHtmlUrlTest, build_url_of_an_unregistered_type_has_no_prefix)
{
    gchar* url = gnc_build_url("never-registered", "location", nullptr);
    EXPECT_STREQ("location", url);
    g_free(url);
}

} // namespace
