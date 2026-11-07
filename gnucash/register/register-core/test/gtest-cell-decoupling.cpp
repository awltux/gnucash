/********************************************************************\
 * gtest-cell-decoupling.cpp -- test the cell layer's view seams     *
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
 *                                                                  *
\********************************************************************/

/** @file gtest-cell-decoupling.cpp
 *  @brief The two seams that let register-core build without a toolkit.
 *
 *  A cell cannot present a dialog and cannot measure text, so it reports a
 *  validation failure to its caller and asks the view whether a glyph can
 *  be rendered.  These tests exercise both seams without a GUI, which also
 *  demonstrates that the library they live in links no toolkit.
 */

#include <config.h>

#include <cstring>
#include <string>

#include <gtest/gtest.h>

extern "C"
{
#include "basiccell.h"
#include "doclinkcell.h"
}

/* A cell reports a failure, its caller takes it exactly once, and the second
 * take is empty: leaving a cell twice must not raise a stale message twice. */
TEST(BasicCellError, ReportedOnceThenTaken)
{
    BasicCell *cell = gnc_basic_cell_new ();

    EXPECT_EQ (nullptr, gnc_basic_cell_get_error (cell));

    gnc_basic_cell_set_error (cell, "first");
    EXPECT_STREQ ("first", gnc_basic_cell_get_error (cell));

    /* A second failure replaces the first rather than queueing. */
    gnc_basic_cell_set_error (cell, "second");
    EXPECT_STREQ ("second", gnc_basic_cell_get_error (cell));

    gchar *message = gnc_basic_cell_take_error (cell);
    EXPECT_STREQ ("second", message);
    g_free (message);

    EXPECT_EQ (nullptr, gnc_basic_cell_get_error (cell));
    EXPECT_EQ (nullptr, gnc_basic_cell_take_error (cell));

    gnc_basic_cell_destroy (cell);
}

namespace
{
struct GlyphProbe
{
    bool called = false;
    std::string text;
    bool available = true;
};

gboolean
probe_glyphs (const char *text, gpointer user_data)
{
    GlyphProbe *probe = static_cast<GlyphProbe *> (user_data);

    probe->called = true;
    probe->text = text;

    return probe->available ? TRUE : FALSE;
}
} // namespace

/* The cell asks the view, and asks about the glyphs it would display. */
TEST(DoclinkCell, GlyphSupportComesFromTheView)
{
    Doclinkcell *cell = (Doclinkcell *) gnc_doclink_cell_new ();

    /* No view installed: the glyphs are assumed to be available. */
    gnc_doclink_cell_set_glyph_support_func (nullptr, nullptr);
    gnc_doclink_cell_set_use_glyphs (cell);
    EXPECT_TRUE (gnc_doclink_get_use_glyphs (cell));

    /* A view that has the glyphs keeps them. */
    GlyphProbe probe;
    gnc_doclink_cell_set_glyph_support_func (probe_glyphs, &probe);
    gnc_doclink_cell_set_use_glyphs (cell);
    EXPECT_TRUE (probe.called);
    EXPECT_EQ (std::string (GLYPH_LINK "," GLYPH_PAPERCLIP), probe.text);
    EXPECT_TRUE (gnc_doclink_get_use_glyphs (cell));

    /* A view whose font lacks them falls back to letters. */
    probe.called = false;
    probe.available = false;
    gnc_doclink_cell_set_use_glyphs (cell);
    EXPECT_TRUE (probe.called);
    EXPECT_FALSE (gnc_doclink_get_use_glyphs (cell));

    /* The hook is process-wide; leave it clean for other tests. */
    gnc_doclink_cell_set_glyph_support_func (nullptr, nullptr);
    gnc_basic_cell_destroy (&cell->cell);
}
