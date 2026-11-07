
add_subdirectory(reports)
add_subdirectory(stylesheets)
add_subdirectory(test)

set (report_HEADERS
  gnc-report.h
)

# Command to generate the swig-report.c wrapper file
gnc_add_swig_guile_command (swig-report-c
    SWIG_REPORT_C swig-report.c
    ${CMAKE_CURRENT_SOURCE_DIR}/report.i ${CMAKE_SOURCE_DIR}/gnucash/html ${report_HEADERS}
)

set (report_SOURCES
        gnc-report.cpp
)

add_library (gnc-report
  ${report_SOURCES}
  ${report_HEADERS}
  ${SWIG_REPORT_C}
)

add_dependencies(gnc-report swig-report-c)

target_compile_definitions(gnc-report PRIVATE -DG_LOG_DOMAIN=\"gnc.report.core\")

target_link_libraries(gnc-report
    gnc-app-utils
    gnucash-guile
    gnc-expressions-guile
    gnc-html-url
    ${GUILE_LDFLAGS})

target_include_directories (gnc-report
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE ${CMAKE_SOURCE_DIR}/gnucash/html
)


if (APPLE)
  set_target_properties (gnc-report PROPERTIES INSTALL_NAME_DIR "${CMAKE_INSTALL_FULL_LIBDIR}/gnucash")
endif()

if (COVERAGE)
  add_coverage_target(gnc-report)
endif()

install(TARGETS gnc-report
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/gnucash
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}/gnucash
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

install(FILES ${report_HEADERS} DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/gnucash)

# Scheme

set (report_SCHEME_1
    commodity-utilities.scm
    html-acct-table.scm
    html-chart.scm
    html-document.scm
    html-fonts.scm
    html-style-info.scm
    html-style-sheet.scm
    html-anytag.scm
    html-table.scm
    html-text.scm
    html-utilities.scm
    options-utilities.scm
    report-core.scm
    report-gui.scm
    report-utilities.scm
    report-register-hooks.scm
    trep-engine.scm
)

set (report_SCHEME_2
    report.scm
)

set (report_eguile_parts_SCHEME
    eguile-utilities.scm
    eguile-html-utilities.scm
)

set (report_eguile_SCHEME
    eguile.scm
)

set(GUILE_DEPENDS
  gnc-report
  scm-app-utils
  scm-engine
  guile-json
  )

# The GTK tree installs its own scheme modules and the report build only
# waits for them; when that tree is not built (WITH_GNUCASH=OFF) the
# ordering dependency does not exist.
foreach(_report_dep scm-gnc-html scm-gnome-utils)
  if (TARGET ${_report_dep})
    list(APPEND GUILE_DEPENDS ${_report_dep})
  endif()
endforeach()

gnc_add_scheme_targets(scm-report-1
    SOURCES "${report_SCHEME_1}"
    OUTPUT_DIR "gnucash/report"
    DEPENDS "${GUILE_DEPENDS}"
    MAKE_LINKS)

gnc_add_scheme_targets(scm-report-2
    SOURCES "${report_SCHEME_2}"
    OUTPUT_DIR "gnucash"
    DEPENDS scm-report-1
    MAKE_LINKS)

gnc_add_scheme_targets(scm-report-eguile-parts
    SOURCES "${report_eguile_parts_SCHEME}"
    OUTPUT_DIR "gnucash/eguile"
    DEPENDS scm-report-2
    MAKE_LINKS)

gnc_add_scheme_targets(scm-report-eguile
    SOURCES "${report_eguile_SCHEME}"
    OUTPUT_DIR "gnucash"
    DEPENDS scm-report-eguile-parts
    MAKE_LINKS)

add_custom_target(scm-report ALL DEPENDS scm-report-2 scm-report-eguile)

set_local_dist(report_DIST_local CMakeLists.txt
  report.i
  ${report_HEADERS} ${report_SOURCES}
  ${report_SCHEME} ${report_SCHEME_1} ${report_SCHEME_2}
  ${report_eguile_parts_SCHEME} ${report_eguile_SCHEME})

set(report_DIST ${report_DIST_local}
                ${reports_DIST} ${stylesheets_DIST} ${test_report_DIST} PARENT_SCOPE)
