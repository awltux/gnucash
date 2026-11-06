# Qt 6 presentation tier.
#
# Opt-in via -DWITH_QT=ON. The default build must not require Qt: the GTK
# application is what ships, and CI runs without Qt 6 Widgets or WebEngine.
# See doc/adr/0001-qt6-presentation-tier.md.

find_package(Qt6 REQUIRED COMPONENTS Widgets WebEngineWidgets)

set(qt_gnucash_SOURCES
  main.cpp
)

add_executable(gnucash-qt ${qt_gnucash_SOURCES})

target_link_libraries(gnucash-qt
  PRIVATE
    gnc-engine
    gnc-core-utils
    Qt6::Widgets
    Qt6::WebEngineWidgets
    ${CMAKE_DL_LIBS})

target_compile_definitions(gnucash-qt PRIVATE -DG_LOG_DOMAIN=\"gnc.qt\")

install(TARGETS gnucash-qt
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

set_dist_list(qt_DIST CMakeLists.txt ${qt_gnucash_SOURCES})
