add_library(aurora_core STATIC
        lib/aurora.cpp
        lib/webgpu/gpu.cpp
        lib/imgui.cpp
        lib/input.cpp
        lib/window.cpp
        lib/logging.cpp
)
add_library(aurora::core ALIAS aurora_core)

target_compile_definitions(aurora_core PUBLIC AURORA TARGET_PC)
target_include_directories(aurora_core PUBLIC include)
target_link_libraries(aurora_core PUBLIC SDL3::SDL3-static fmt::fmt imgui xxhash)
if (EMSCRIPTEN)
    target_link_options(aurora_core PUBLIC -sUSE_WEBGPU=1 -sASYNCIFY -sEXIT_RUNTIME)
    target_compile_definitions(aurora_core PRIVATE ENABLE_BACKEND_WEBGPU)
else ()
    target_link_libraries(aurora_core PRIVATE dawn::dawn_native dawn::dawn_proc)
    target_sources(aurora_core PRIVATE lib/dawn/BackendBinding.cpp)
    target_compile_definitions(aurora_core PRIVATE WEBGPU_DAWN)
endif ()
target_link_libraries(aurora_core PRIVATE absl::btree absl::flat_hash_map)
if (DAWN_ENABLE_VULKAN)
    target_compile_definitions(aurora_core PRIVATE DAWN_ENABLE_BACKEND_VULKAN)
    target_link_libraries(aurora_core PRIVATE Vulkan::Headers)
endif ()
if (DAWN_ENABLE_METAL)
    target_compile_definitions(aurora_core PRIVATE DAWN_ENABLE_BACKEND_METAL)
    target_sources(aurora_core PRIVATE lib/dawn/MetalBinding.mm)
    set_source_files_properties(lib/dawn/MetalBinding.mm PROPERTIES COMPILE_FLAGS -fobjc-arc)
endif ()
if (DAWN_ENABLE_D3D11)
    target_compile_definitions(aurora_core PRIVATE DAWN_ENABLE_BACKEND_D3D11)
endif ()
if (DAWN_ENABLE_D3D12)
    target_compile_definitions(aurora_core PRIVATE DAWN_ENABLE_BACKEND_D3D12)
endif ()
if (DAWN_ENABLE_DESKTOP_GL OR DAWN_ENABLE_OPENGLES)
    target_compile_definitions(aurora_core PRIVATE DAWN_ENABLE_BACKEND_OPENGL)
    if (DAWN_ENABLE_DESKTOP_GL)
        target_compile_definitions(aurora_core PRIVATE DAWN_ENABLE_BACKEND_DESKTOP_GL)
    endif ()
    if (DAWN_ENABLE_OPENGLES)
        target_compile_definitions(aurora_core PRIVATE DAWN_ENABLE_BACKEND_OPENGLES)
    endif ()
endif ()
if (DAWN_ENABLE_NULL)
    target_compile_definitions(aurora_core PRIVATE DAWN_ENABLE_BACKEND_NULL)
endif ()