set(html_url_test_SOURCES
  gtest-gnc-html-url.cpp
  # Compile the unit under test directly: it must not need GTK, and this test
  # is the proof. Linking gnc-html instead would pull GTK back in.
  ${CMAKE_SOURCE_DIR}/gnucash/html/gnc-html-url.cpp
)

set(HTML_URL_TEST_INCLUDE_DIRS
  ${CMAKE_BINARY_DIR}/common          # for config.h
  ${CMAKE_SOURCE_DIR}/common          # for platform.h
  ${CMAKE_SOURCE_DIR}/gnucash/html
  ${CMAKE_SOURCE_DIR}/libgnucash/engine
  ${CMAKE_SOURCE_DIR}/common/test-core
)

set(HTML_URL_TEST_LIBS gnc-engine gtest)

gnc_add_test(test-gnc-html-url "${html_url_test_SOURCES}"
  HTML_URL_TEST_INCLUDE_DIRS HTML_URL_TEST_LIBS)

set_dist_list(test_gnc_html_DIST CMakeLists.txt gtest-gnc-html-url.cpp)
