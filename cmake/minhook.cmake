# MinHook, vendored as a submodule under third_party/minhook and built from
# source so no external package is required.

set(MINHOOK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/minhook")

if(NOT EXISTS "${MINHOOK_DIR}/include/MinHook.h")
    message(FATAL_ERROR
        "third_party/minhook is empty. Run: git submodule update --init --recursive")
endif()

add_library(minhook STATIC
    "${MINHOOK_DIR}/src/buffer.c"
    "${MINHOOK_DIR}/src/hook.c"
    "${MINHOOK_DIR}/src/trampoline.c"
    "${MINHOOK_DIR}/src/hde/hde64.c")

target_include_directories(minhook PUBLIC "${MINHOOK_DIR}/include")
target_include_directories(minhook PRIVATE "${MINHOOK_DIR}/src")
target_compile_definitions(minhook PRIVATE WIN32_LEAN_AND_MEAN)
add_library(minhook::minhook ALIAS minhook)

# HDE64, the x86-64 instruction decoder MinHook is built on, for code that has
# to read instructions rather than only hook them.
add_library(minhook_hde INTERFACE)
target_include_directories(minhook_hde INTERFACE "${MINHOOK_DIR}/src")
add_library(minhook::hde ALIAS minhook_hde)
