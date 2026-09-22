cmake_minimum_required(VERSION 3.21)

project(opendlssg-fg
    VERSION 0.1.0
    DESCRIPTION "Open source DLSS Frame Generation for RTX 30"
    LANGUAGES C CXX)

if(NOT WIN32)
    message(FATAL_ERROR
        "opendlssg-fg targets Windows x64. Configure with the MinGW-w64 toolchain "
        "(-DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw-w64.cmake).")
endif()
# The generated proxy stubs are GCC and Clang inline assembly, and the build is
# tested with LLVM-MinGW only. Clang and GCC targeting MinGW are what CI builds.
if(MSVC)
    message(FATAL_ERROR "MSVC is not supported. Build with LLVM-MinGW; see docs/BUILD.md.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

add_compile_definitions(
    UNICODE _UNICODE
    WIN32_LEAN_AND_MEAN NOMINMAX
    ODG_VERSION="${PROJECT_VERSION}")

add_library(odg_warnings INTERFACE)
target_compile_options(odg_warnings INTERFACE
    -Wall -Wextra -Wno-unknown-pragmas -Wno-cast-function-type)

include(cmake/minhook.cmake)
include(cmake/libhat.cmake)
include(cmake/lz4.cmake)

add_subdirectory(src)

option(ODG_BUILD_TESTS "Build the unit tests" OFF)
if(ODG_BUILD_TESTS)
    add_executable(odg_unit_tests tests/unit_tests.cpp)
    target_link_libraries(odg_unit_tests PRIVATE odg_core lz4::lz4 odg_warnings)
    # The tests check the shipped configuration file, next to the executable.
    add_custom_command(TARGET odg_unit_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/assets/opendlssg.ini" "$<TARGET_FILE_DIR:odg_unit_tests>")
endif()

option(ODG_BUILD_TOOLS "Build the developer tools" OFF)
if(ODG_BUILD_TOOLS)
    add_executable(ptxprobe tools/ptxprobe/main.cpp)
    target_link_libraries(ptxprobe PRIVATE odg_core odg_warnings)
    add_executable(patchprobe tools/patchprobe/main.cpp)
    target_link_libraries(patchprobe PRIVATE odg_core odg_warnings)
endif()
