cmake_minimum_required(VERSION 3.22.1)
project(logpulsex_mobile)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(logpulsex_mobile SHARED native-lib.cpp)
target_include_directories(logpulsex_mobile PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../../../../../include")
target_compile_options(logpulsex_mobile PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(logpulsex_mobile PRIVATE log)
