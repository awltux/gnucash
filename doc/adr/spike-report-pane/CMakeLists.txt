cmake_minimum_required(VERSION 3.21)
project(gnc_report_spike LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets WebEngineWidgets)

add_executable(spike-report main.cpp)
target_link_libraries(spike-report PRIVATE Qt6::Widgets Qt6::WebEngineWidgets)
