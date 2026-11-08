# Local web server for the GnuCash engine.
#
# This is the engine plus a browser UI; it needs no toolkit, so it builds
# together with the report subtree (WITH_REPORT) rather than the GTK app.
# Opt-in while it is new, like the Qt tier was.

set(webd_SOURCES
  server/main.cpp
)

add_executable(gnc-webd ${webd_SOURCES})

target_link_libraries(gnc-webd
  gnc-report
  gnc-app-utils
  gnc-engine
  gnc-core-utils
  gnucash-guile
  PkgConfig::GLIB2
  ${GIO_LIBRARIES}
  ${GUILE_LDFLAGS}
  ${CMAKE_DL_LIBS}
)

target_compile_definitions(gnc-webd PRIVATE
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

# The distribution list lives at the top level: the web tier ships even when
# WITH_WEBUI is off, so the tarball is complete either way.

