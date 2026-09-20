# libhat: signature scanning (IDA syntax plus bit masks) and RIP-relative
# address resolution. Vendored as a submodule, pinned to a release.

set(LIBHAT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libhat")

if(NOT EXISTS "${LIBHAT_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "third_party/libhat is empty. Run: git submodule update --init --recursive")
endif()

set(LIBHAT_TESTING OFF CACHE BOOL "" FORCE)
set(LIBHAT_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory("${LIBHAT_DIR}" "${CMAKE_BINARY_DIR}/libhat" EXCLUDE_FROM_ALL)

# libhat's Windows sources include <Windows.h>. Windows filesystems ignore case,
# but a toolchain hosted on Linux ships the header as windows.h, so a shim with
# the capitalised name forwards to it.
if(CMAKE_HOST_UNIX)
    set(LIBHAT_CASE_SHIMS "${CMAKE_BINARY_DIR}/libhat_case_shims")
    file(WRITE "${LIBHAT_CASE_SHIMS}/Windows.h" "#include <windows.h>\n")
    target_include_directories(libhat PRIVATE "${LIBHAT_CASE_SHIMS}")
endif()
