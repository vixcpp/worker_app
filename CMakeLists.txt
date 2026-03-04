cmake_minimum_required(VERSION 3.20)
project(worker_app LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(worker_app INTERFACE)
add_library(worker_app::worker_app ALIAS worker_app)

target_include_directories(worker_app INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

if (MSVC)
  target_compile_options(worker_app INTERFACE /W4 /permissive-)
else()
  target_compile_options(worker_app INTERFACE -Wall -Wextra -Wpedantic)
endif()

include(.vix/vix_deps.cmake)
target_link_libraries(worker_app INTERFACE vix::app)

include(CTest)
enable_testing()

add_executable(worker_app_basic_test tests/test_basic.cpp)
target_link_libraries(worker_app_basic_test PRIVATE worker_app::worker_app)
add_test(NAME worker_app.basic COMMAND worker_app_basic_test)
