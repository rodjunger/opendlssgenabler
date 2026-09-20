# LZ4, vendored as a submodule under third_party/lz4. Only the block format is
# needed, which is what nvcc compresses fatbin payloads with.

set(LZ4_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/lz4")

if(NOT EXISTS "${LZ4_DIR}/lib/lz4.h")
    message(FATAL_ERROR
        "third_party/lz4 is empty. Run: git submodule update --init --recursive")
endif()

add_library(lz4 STATIC "${LZ4_DIR}/lib/lz4.c")
target_include_directories(lz4 PUBLIC "${LZ4_DIR}/lib")
add_library(lz4::lz4 ALIAS lz4)
