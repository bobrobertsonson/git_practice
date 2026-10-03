cmake_minimum_required(VERSION 3.24)
project(Sawblade VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(SAWBLADE_BUILD_TESTS "Build the Catch2 test suite" ON)

# Warnings-as-errors apply to Sawblade targets only: link this interface target into them.
# Third-party targets never link it and expose their headers as SYSTEM.
add_library(sawblade_warnings INTERFACE)
if(MSVC)
  target_compile_options(sawblade_warnings INTERFACE /W4 /WX)
else()
  target_compile_options(sawblade_warnings INTERFACE -Wall -Wextra -Wpedantic -Werror)
endif()

include(cmake/Dependencies.cmake)

add_subdirectory(core)
# Slots for later tasks (T4): add_subdirectory(cli)

if(SAWBLADE_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
