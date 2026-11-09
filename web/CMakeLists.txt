# Local web server for the GnuCash engine.
#
# This is the engine plus a browser UI; it needs no toolkit, so it builds
# together with the report subtree (WITH_REPORT) rather than the GTK app.
# Opt-in while it is new, like the Qt tier was.

set(webd_SOURCES
  server/main.cpp
  server/report-spec.cpp
)

set(webd_HEADERS
  server/json.hpp
  server/report-spec.hpp
)

add_executable(gnc-webd ${webd_SOURCES} ${webd_HEADERS})

# The report engine is the only Guile consumer in this tier. Without it the
# report routes are compiled out of main.cpp and the binary links neither
# gnc-report nor gnucash-guile; the engine and the rest of the UI still build.
set(webd_report_LIBRARIES)
set(webd_report_DEFINITIONS)
if (WITH_GUILE AND WITH_REPORT)
  list(APPEND webd_report_LIBRARIES gnc-report gnucash-guile ${GUILE_LDFLAGS})
  list(APPEND webd_report_DEFINITIONS GNC_WITH_REPORT)
endif()

target_link_libraries(gnc-webd
  ${webd_report_LIBRARIES}
  gnc-reports
  gnc-app-utils
  gnc-engine
  gnc-core-utils
  PkgConfig::GLIB2
  ${GIO_LIBRARIES}
  ${CMAKE_DL_LIBS}
)

target_compile_definitions(gnc-webd PRIVATE
  ${webd_report_DEFINITIONS}
  -DG_LOG_DOMAIN=\"gnc.webd\"
  -DGNC_WEBUI_DIR="${CMAKE_CURRENT_SOURCE_DIR}/ui"
)

set_source_files_properties(${webd_SOURCES} PROPERTIES OBJECT_DEPENDS ${CONFIG_H})

# GnuCash sets -mwindows in CMAKE_EXE_LINKER_FLAGS on Windows, which would make
# this a GUI-subsystem binary: its "listening on http://..." line would never
# reach stdout, and that line is how a user and the tests find the server.
if (WIN32)
  target_link_options(gnc-webd PRIVATE -mconsole)
endif()

target_include_directories(gnc-webd PRIVATE
  ${CMAKE_SOURCE_DIR}/common
  ${CMAKE_BINARY_DIR}/common
)

install(TARGETS gnc-webd
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

install(DIRECTORY ui/
  DESTINATION ${CMAKE_INSTALL_DATADIR}/gnucash/webui)

# The chart library is vendored once, under borrowed/.  The source tree is
# served from there; an installed build gets the same file next to the UI so
# the report route's vendor/chart.umd.js works without the source tree.
install(FILES ${CMAKE_SOURCE_DIR}/borrowed/chartjs-4/chart.umd.js
  DESTINATION ${CMAKE_INSTALL_DATADIR}/gnucash/webui/vendor)

# The distribution list lives at the top level: the web tier ships even when
# WITH_WEBUI is off, so the tarball is complete either way.

