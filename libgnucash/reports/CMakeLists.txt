# CMakeLists.txt for libgnucash/reports
#
# The native report layer.  It needs neither Guile nor a toolkit, so it is
# built in every configuration; gnc-webd renders native reports from it and
# falls back to the Scheme engine only for reports it does not know.

set (reports_SOURCES
  gnc-report.cpp
  gnc-invoice.cpp
  gnc-invoice-print.cpp
  gnc-invoice-receipt.cpp
  gnc-invoice-tax.cpp
  gnc-owner.cpp
  gnc-tax-codes.cpp
  gnc-tax.cpp
)

set (reports_HEADERS
  gnc-report.hpp
  gnc-invoice.hpp
  gnc-owner.hpp
  gnc-tax-codes.hpp
)

add_library (gnc-reports STATIC
  ${reports_SOURCES}
  ${reports_HEADERS}
)

target_link_libraries (gnc-reports
  PUBLIC
    gnc-engine
    gnc-app-utils
    gnc-core-utils
)

target_include_directories (gnc-reports
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
  PRIVATE
    ${CMAKE_SOURCE_DIR}/common
    ${CMAKE_BINARY_DIR}/common
)

set_source_files_properties (${reports_SOURCES}
  PROPERTIES OBJECT_DEPENDS ${CONFIG_H})

set_dist_list (reports_DIST
  CMakeLists.txt
  ${reports_SOURCES}
  ${reports_HEADERS}
)
