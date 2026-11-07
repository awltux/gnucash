set(register_core_test_SOURCES
  gtest-cell-decoupling.cpp
)

set(register_core_test_INCLUDE_DIRS
  ${CMAKE_SOURCE_DIR}/gnucash/register/register-core
  ${CMAKE_BINARY_DIR}/common # for config.h
)

set(register_core_test_LIBS
  gnc-register-core
  PkgConfig::GLIB2
  gtest
)

gnc_add_test(test-register-core-decoupling
  "${register_core_test_SOURCES}"
  register_core_test_INCLUDE_DIRS
  register_core_test_LIBS
)

set_dist_list(test_register_core_DIST CMakeLists.txt ${register_core_test_SOURCES})
