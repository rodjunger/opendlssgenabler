add_library(odg_core STATIC
    core/log.cpp
    core/config.cpp
    core/pe.cpp
    core/paths.cpp
    core/text.cpp
    core/hooks.cpp
    core/x86.cpp
    app/settings.cpp
    app/runtime.cpp
    app/loader.cpp
    spoof/nvapi.cpp
    kernels/cubin.cpp
    kernels/fatbin.cpp
    kernels/device.cpp
    kernels/cubin_params.cpp
    kernels/substitute.cpp
    kernels/provider_index.cpp
    kernels/vulkan_hooks.cpp
    streamline/streamline.cpp
    streamline/plugin_patch.cpp
    provider/multi_frame.cpp
    provider/version_policy.cpp
    proxy/proxy.cpp)

target_include_directories(odg_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_include_directories(odg_core PRIVATE
    "${CMAKE_SOURCE_DIR}/third_party/streamline/include")
target_link_libraries(odg_core
    PUBLIC minhook::minhook libhat::libhat
    PRIVATE minhook::hde lz4::lz4 odg_warnings)

set(GENERATED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/proxy/generated")

# Builds one proxy DLL that loads under `dll_name`.dll and forwards the real
# system DLL's exports. The generated translation unit and its .def come from
# tools/gen_proxy.py.
function(add_proxy dll_name)
    set(target "proxy_${dll_name}")
    add_library(${target} SHARED
        dllmain.cpp
        "${GENERATED_DIR}/${dll_name}_proxy.gen.cpp")
    target_link_libraries(${target} PRIVATE odg_core odg_warnings)
    set_target_properties(${target} PROPERTIES PREFIX "" OUTPUT_NAME "${dll_name}")
    target_link_options(${target} PRIVATE "${GENERATED_DIR}/${dll_name}.def")
endfunction()

add_proxy(version)
add_proxy(winmm)
add_proxy(dinput8)
add_proxy(dxgi)
