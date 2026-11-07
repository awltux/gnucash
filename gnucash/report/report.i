/********************************************************************\
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

%module sw_report
%{
/* Includes the header in the wrapper code */
#include <config.h>
#include <stdio.h>
#include <libguile.h>
#include <gnc-report.h>
#include <gnc-html-extras.h>
%}
#if defined(SWIGGUILE)
%{
#include "guile-mappings.h"

SCM scm_init_sw_report_module (void);
%}

/* The wrapper is compiled as C, and the freearg typemap would pass a
 * const-qualified pointer straight to free(). */
%typemap (freearg) const char* "if (must_free$argnum && $1) SWIG_free((char*)$1);";
#endif

%import "base-typemaps.i"

SCM gnc_report_find(gint id);
gint gnc_report_add(SCM report);

%newobject gnc_get_default_report_font_family;
gchar* gnc_get_default_report_font_family();

/* Reports build GnuCash URLs into their HTML. The builder is toolkit-free
 * (gnc-html-url), so this module wraps it rather than borrowing the GTK
 * html module's wrapper. The CSS probe belongs to the rendering engine, not
 * to report generation, so it is not wrapped. */
%ignore gnc_html_engine_supports_css;
%newobject gnc_build_url;
%include "gnc-html-extras.h"

%init {
  {
    char tmp[100];

#define SET_ENUM(e) snprintf(tmp, 100, "(set! %s (%s))", (e), (e));  \
    scm_c_eval_string(tmp);

    SET_ENUM("URL-TYPE-FILE");
    SET_ENUM("URL-TYPE-JUMP");
    SET_ENUM("URL-TYPE-HTTP");
    SET_ENUM("URL-TYPE-FTP");
    SET_ENUM("URL-TYPE-SECURE");
    SET_ENUM("URL-TYPE-REGISTER");
    SET_ENUM("URL-TYPE-ACCTTREE");
    SET_ENUM("URL-TYPE-REPORT");
    SET_ENUM("URL-TYPE-OPTIONS");
    SET_ENUM("URL-TYPE-SCHEME");
    SET_ENUM("URL-TYPE-HELP");
    SET_ENUM("URL-TYPE-XMLDATA");
    SET_ENUM("URL-TYPE-PRICE");
    SET_ENUM("URL-TYPE-OTHER");
    SET_ENUM("URL-TYPE-BUDGET");

#undef SET_ENUM
  }
}

void gnc_saved_reports_backup (void);
gboolean gnc_saved_reports_write_to_file (const gchar* report_def, gboolean overwrite);
