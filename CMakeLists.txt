cmake_minimum_required(VERSION 3.20)
project(stratadb VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_library(stratadb_core
  src/storage/key_value_store.cpp
  src/http/http_server.cpp
)
target_include_directories(stratadb_core PUBLIC src)
target_compile_options(stratadb_core PRIVATE -Wall -Wextra -Wpedantic)

add_executable(stratadb src/main.cpp)
target_link_libraries(stratadb PRIVATE stratadb_core)

enable_testing()
add_executable(stratadb_tests tests/key_value_store_test.cpp)
target_link_libraries(stratadb_tests PRIVATE stratadb_core)
add_test(NAME stratadb_tests COMMAND stratadb_tests)
