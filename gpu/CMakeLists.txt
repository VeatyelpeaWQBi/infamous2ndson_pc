# bbport GPU library: shadPS4 video core + shader recompiler + Gnm/VideoOut
# drivers, adapted to the native_probe runtime through shims in shim/.
cmake_minimum_required(VERSION 3.24)
project(bbgpu LANGUAGES C CXX)
if (NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "This fork supports Windows x86-64 only; use build.bat with MSYS2 CLANG64.")
endif()
if (NOT MINGW OR NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "Use the existing MSYS2 CLANG64 Clang/libc++/LLD toolchain.")
endif()
option(BB_ALLOW_DOWNLOADS "Allow missing FetchContent source downloads (explicit opt-in)" OFF)
option(BB_BUILD_TESTS "Register the Windows C++ test suite" OFF)
include(FetchContent)
set(FETCHCONTENT_UPDATES_DISCONNECTED ON)
if (BB_ALLOW_DOWNLOADS)
    set(FETCHCONTENT_FULLY_DISCONNECTED OFF CACHE BOOL "Offline source population" FORCE)
else()
    set(FETCHCONTENT_FULLY_DISCONNECTED ON CACHE BOOL "Offline source population" FORCE)
endif()
function(bb_require_local_source name)
    string(TOUPPER "${name}" upper_name)
    set(local_source "${FETCHCONTENT_SOURCE_DIR_${upper_name}}")
    if (NOT local_source)
        set(local_source "${FETCHCONTENT_BASE_DIR}/${name}-src")
        if (NOT EXISTS "${local_source}/CMakeLists.txt" AND
                EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../out/dependencies/${name}/CMakeLists.txt")
            set(local_source "${CMAKE_CURRENT_SOURCE_DIR}/../out/dependencies/${name}")
        endif()
    endif()
    if (NOT EXISTS "${local_source}/CMakeLists.txt")
        if (BB_ALLOW_DOWNLOADS)
            return()
        endif()
        message(FATAL_ERROR "Missing ${name} source. No downloads started. Approve downloads and use build.bat --allow-downloads, or supply FETCHCONTENT_SOURCE_DIR_${upper_name}.")
    endif()
    # A local override prevents even a partially populated cache from starting a download.
    set(FETCHCONTENT_SOURCE_DIR_${upper_name} "${local_source}" CACHE PATH "Existing ${name} source" FORCE)
endfunction()
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
# Frame pointers keep the loader's thread dumps (watchdog, SIGUSR2 sampling) walkable.
add_compile_options(-fno-omit-frame-pointer)

# bbport: link-time optimization (BB_LTO, default on) and profile-guided optimization
# (BB_PGO: off, generate = instrumented build writing into BB_PGO_DIR, use = build with that
# profile). Covers bbgpu and the static libraries linked into it. No -march: the same build
# targets Windows x86-64 CPUs.
option(BB_LTO "Link-time optimization of bbgpu" ON)
set(BB_PGO "off" CACHE STRING "Profile-guided optimization: off, generate, use")
set(BB_PGO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../pgo" CACHE PATH "Profile directory")
if (NOT BB_PGO STREQUAL "off")
    message(FATAL_ERROR "Windows PGO is not validated. Use BB_PGO=off; do not reuse GCC/Linux profiles.")
endif()
if (BB_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT bb_ipo_ok OUTPUT bb_ipo_error)
    if (bb_ipo_ok)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
    else()
        message(WARNING "LTO unavailable: ${bb_ipo_error}")
    endif()
endif()
if (BB_PGO STREQUAL "generate")
    # Atomic counters: the GPU, recording, preparation and copy threads run the same code.
    add_compile_options(-fprofile-generate=${BB_PGO_DIR} -fprofile-update=atomic)
    add_link_options(-fprofile-generate=${BB_PGO_DIR})
    add_compile_definitions(BB_PGO_GENERATE)
elseif (BB_PGO STREQUAL "use")
    # Partial training keeps unprofiled code optimized normally; counters from concurrent
    # threads can be slightly inconsistent (correction). Functions changed since the profile
    # (or built differently for it, bbgpu_init) are compiled without it.
    add_compile_options(-fprofile-use=${BB_PGO_DIR} -fprofile-partial-training
                        -fprofile-correction -Wno-missing-profile -Wno-coverage-mismatch)
endif()

find_package(Vulkan REQUIRED)
find_package(fmt REQUIRED)
find_package(Boost REQUIRED)
find_package(tsl-robin-map REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(XXHASH REQUIRED IMPORTED_TARGET libxxhash)
pkg_check_modules(SDL3 REQUIRED IMPORTED_TARGET sdl3)
find_package(Zydis REQUIRED)
if (WIN32)
    # Windows (MSYS2 CLANG64). bbgpu is a static library linked into bb-probe.exe
    # (a DLL cannot leave the runtime_* symbols undefined). Packages MSYS2 does not ship
    # (magic_enum, miniz, xbyak) are fetched; VMA is header-only there.
    include(FetchContent)
    find_package(magic_enum QUIET)
    if (NOT TARGET magic_enum::magic_enum)
        FetchContent_Declare(magic_enum GIT_REPOSITORY https://github.com/Neargye/magic_enum.git
            GIT_TAG v0.9.7 GIT_SHALLOW TRUE)
        bb_require_local_source(magic_enum)
        FetchContent_MakeAvailable(magic_enum)
    endif()
    find_package(miniz QUIET)
    if (NOT TARGET miniz::miniz)
        set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(BUILD_FUZZERS OFF CACHE BOOL "" FORCE)
        set(BUILD_NO_STDIO OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(miniz GIT_REPOSITORY https://github.com/richgel999/miniz.git
            GIT_TAG 3.1.0 GIT_SHALLOW TRUE)
        bb_require_local_source(miniz)
        FetchContent_MakeAvailable(miniz)
        if (NOT TARGET miniz::miniz)
            add_library(miniz::miniz ALIAS miniz)
        endif()
    endif()
    find_package(xbyak QUIET)
    if (NOT TARGET xbyak::xbyak)
        FetchContent_Declare(xbyak GIT_REPOSITORY https://github.com/herumi/xbyak.git
            GIT_TAG v7.24.2 GIT_SHALLOW TRUE)
        bb_require_local_source(xbyak)
        FetchContent_MakeAvailable(xbyak)
    endif()
    find_package(VulkanMemoryAllocator CONFIG QUIET)
    if (NOT TARGET GPUOpen::VulkanMemoryAllocator)
        find_path(VMA_INCLUDE_DIR vk_mem_alloc.h PATH_SUFFIXES vma REQUIRED)
        add_library(GPUOpen::VulkanMemoryAllocator INTERFACE IMPORTED)
        set_target_properties(GPUOpen::VulkanMemoryAllocator PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES ${VMA_INCLUDE_DIR})
    endif()
endif()
find_program(GLSLANG_COMPILER NAMES glslang glslangValidator REQUIRED)

set(SIRIT_USE_SYSTEM_SPIRV_HEADERS OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/sirit EXCLUDE_FROM_ALL)

# bbport: FSR 3.1 temporal upscaling / frame generation, native Vulkan (MIT, FireBurn/FSR-Vulkan).
set(FFX_VK_PORTABLE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(FFX_VK_PORTABLE_BUILD_PROBE OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/fsr-vulkan EXCLUDE_FROM_ALL)
if (WIN32)
    # AMD's FidelityFX sources rely on MSVC's transitive includes (memset, floor, placement new,
    # swprintf_s) on _WIN32.
    foreach(ffx_target IN ITEMS ffx_vulkan_portable ffx_vulkan_fsr3_vk_backend_1_1_4
            ffx_vulkan_fsr3_host_1_1_4 ffx_vulkan_fsr4_v07_vulkan ffx_vulkan_fsr4_v07_assets)
        if (TARGET ${ffx_target})
            target_compile_options(${ffx_target} PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:SHELL:-include cstdio>"
                "$<$<COMPILE_LANGUAGE:CXX>:SHELL:-include cwchar>" "$<$<COMPILE_LANGUAGE:CXX>:SHELL:-include cstring>"
                "$<$<COMPILE_LANGUAGE:CXX>:SHELL:-include cmath>" "$<$<COMPILE_LANGUAGE:CXX>:SHELL:-include new>"
                "$<$<COMPILE_LANGUAGE:CXX>:SHELL:-include mutex>")
        endif()
    endforeach()
endif()

# Host shaders -> headers (same scheme as shadPS4's host_shaders/CMakeLists.txt).
set(SHADER_SRC ${CMAKE_CURRENT_SOURCE_DIR}/shadps4/video_core/host_shaders)
set(SHADER_INCLUDE ${CMAKE_CURRENT_BINARY_DIR}/include)
set(SHADER_DIR ${SHADER_INCLUDE}/video_core/host_shaders)
set(SHADER_HEADERS "")
function(add_host_shader FILENAME SHADER_NAME)
    set(OUT ${SHADER_DIR}/${SHADER_NAME}.h)
    string(TOUPPER ${SHADER_NAME} SYMBOL)
    set(DEFINES ${ARGN})
    list(TRANSFORM DEFINES PREPEND "-D")
    add_custom_command(OUTPUT ${OUT}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${SHADER_DIR}
        COMMAND ${GLSLANG_COMPILER} --target-env vulkan1.3 -o ${OUT} --vn ${SYMBOL} ${DEFINES} ${SHADER_SRC}/${FILENAME}
        COMMAND ${CMAKE_COMMAND} "-DHEADER_FILE=${OUT}" -P ${SHADER_SRC}/AddShaderHeaderInclude.cmake
        DEPENDS ${SHADER_SRC}/${FILENAME} VERBATIM)
    set(SHADER_HEADERS ${SHADER_HEADERS} ${OUT} PARENT_SCOPE)
endfunction()
add_host_shader(color_to_ms_depth.frag color_to_ms_depth_frag)
add_host_shader(ms_image_blit.frag ms_image_blit_frag)
add_host_shader(ms_image_blit.frag ms_image_blit_msaa_frag SRC_MSAA=1)
add_host_shader(fault_buffer_process.comp fault_buffer_process_comp)
add_host_shader(fs_tri.vert fs_tri_vert)
add_host_shader(fsr.comp fsr_easu_comp SAMPLE_EASU=1)
add_host_shader(fsr.comp fsr_rcas_comp SAMPLE_RCAS=1)
add_host_shader(post_process.frag post_process_frag)
add_host_shader(camera_motion.comp camera_motion_comp)
add_host_shader(upscale_merge.comp upscale_merge_comp)
add_host_shader(upscale_debug.comp upscale_debug_comp)
add_host_shader(upscale_reactive.comp upscale_reactive_comp)
add_host_shader(taa.comp taa_comp)
add_host_shader(taa_sharpen.comp taa_sharpen_comp)
add_host_shader(taa_sharpen.comp taa_sharpen_ldr_comp LDR_OUTPUT=1)
add_host_shader(buffer_multi_copy.comp buffer_multi_copy_comp)
add_host_shader(depth_resample.frag depth_resample_frag)
add_host_shader(depth_resample.frag depth_stencil_resample_frag HAS_STENCIL=1)
add_host_shader(depth_resample.frag depth_stencil_bits_frag STENCIL_BITS=1)
add_host_shader(camera_motion.comp camera_motion_debug_comp DEBUG_OVERLAY=1)
foreach(BPP IN ITEMS 8 16 32 64 96 128)
    add_host_shader(tiling.comp tiling_micro_${BPP}_comp BITS_PER_PIXEL=${BPP} IS_MACRO_TILED=0)
    add_host_shader(tiling.comp tiling_macro_${BPP}_comp BITS_PER_PIXEL=${BPP} IS_MACRO_TILED=1)
endforeach()
add_custom_target(host_shaders DEPENDS ${SHADER_HEADERS})

file(GLOB_RECURSE VIDEO_CORE CONFIGURE_DEPENDS shadps4/video_core/*.cpp)
file(GLOB_RECURSE RECOMPILER CONFIGURE_DEPENDS shadps4/shader_recompiler/*.cpp)
list(FILTER VIDEO_CORE EXCLUDE REGEX "renderdoc\\.cpp$")
set(COMMON
    shadps4/common/decoder.cpp shadps4/common/error.cpp
    shadps4/common/io_file.cpp shadps4/common/native_clock.cpp shadps4/common/number_utils.cpp
    shadps4/common/path_util.cpp shadps4/common/rdtsc.cpp shadps4/common/spin_lock.cpp shadps4/common/recursive_lock.cpp
    shadps4/common/string_util.cpp shadps4/common/thread.cpp shadps4/common/signal_context.cpp
    shim/common/logging/log.cpp)
file(GLOB AVPLAYER CONFIGURE_DEPENDS shadps4/core/libraries/avplayer/*.cpp)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavformat libavcodec libavutil libswscale libswresample)
file(GLOB SHIM_SOURCES CONFIGURE_DEPENDS shim/*.cpp shim/*/*.cpp)
add_library(bbgpu STATIC ${VIDEO_CORE} ${RECOMPILER} ${COMMON} ${SHIM_SOURCES}
    shadps4/core/libraries/gnmdriver/gnmdriver.cpp
    shadps4/core/libraries/videoout/driver.cpp shadps4/core/libraries/videoout/video_out.cpp
    shadps4/core/libraries/kernel/equeue.cpp ${AVPLAYER}
    shadps4/core/libraries/videodec/video_utils.cpp)
add_dependencies(bbgpu host_shaders)
# In-game settings menu: Dear ImGui (third_party/imgui, MIT) with its Vulkan backend, and an
# embedded Cyrillic font (third_party/fonts).
target_sources(bbgpu PRIVATE third_party/imgui/imgui.cpp third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp third_party/imgui/imgui_widgets.cpp
    third_party/imgui/backends/imgui_impl_vulkan.cpp)
target_include_directories(bbgpu PRIVATE third_party/imgui third_party/imgui/backends)
target_compile_definitions(bbgpu PRIVATE IMGUI_IMPL_VULKAN_NO_PROTOTYPES
    BB_FONT_PATH="${CMAKE_CURRENT_SOURCE_DIR}/third_party/fonts/DejaVuSans.ttf")
set_source_files_properties(shim/bbport_overlay.cpp PROPERTIES
    OBJECT_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/fonts/DejaVuSans.ttf)
target_include_directories(bbgpu PRIVATE shim shadps4 ${SHADER_INCLUDE} third_party/gcn/include third_party/half/include)
target_compile_definitions(bbgpu PRIVATE BOOST_ASIO_STANDALONE NDEBUG)
target_compile_options(bbgpu PRIVATE -Wno-deprecated-declarations -Wno-missing-field-initializers)
target_link_libraries(bbgpu PRIVATE ffx-vulkan::portable ffx-vulkan::fsr4-v07-vulkan)
target_link_libraries(bbgpu PUBLIC Vulkan::Headers fmt::fmt Boost::headers magic_enum::magic_enum
    tsl::robin_map GPUOpen::VulkanMemoryAllocator PkgConfig::XXHASH PkgConfig::SDL3 PkgConfig::FFMPEG miniz::miniz Zydis::Zydis sirit)
if (WIN32)
    # Windows headers: no min/max macros, no winsock 1 clash; Windows 10+ APIs (SetThreadDescription).
    target_compile_definitions(bbgpu PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
    target_link_libraries(bbgpu PUBLIC xbyak::xbyak)
    target_link_libraries(bbgpu PUBLIC winmm ws2_32 bcrypt ole32 shell32 user32 psapi kernel32 onecore)
    # bb-probe.exe links libbbgpu.a with these (out/gpu/bbgpu_link.txt, one argument per line,
    # in link order): the static library's transitive dependencies.
    include(cmake/LinkClosure.cmake)
    bb_write_link_closure(bbgpu ${CMAKE_CURRENT_BINARY_DIR}/bbgpu_link.txt)
endif()

# Explicit Windows tests: unit targets need no GPU; gpu targets need a Vulkan device.
add_executable(motion-history-test EXCLUDE_FROM_ALL ../tests/test_motion_history.cpp)
target_compile_options(motion-history-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(motion-history-test PRIVATE ..)
add_executable(motion-shader-test EXCLUDE_FROM_ALL ../tests/test_motion_shaders.cpp ../tests/test_gpu_runtime_stubs.cpp)
target_include_directories(motion-shader-test PRIVATE shim shadps4 third_party/gcn/include)
target_link_libraries(motion-shader-test PRIVATE bbgpu)
add_executable(ui-composition-test EXCLUDE_FROM_ALL ../tests/test_ui_composition.cpp)
target_compile_options(ui-composition-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(ui-composition-test PRIVATE ..)

add_executable(scene-resolution-test EXCLUDE_FROM_ALL ../tests/test_scene_resolution.cpp ../tests/test_gpu_runtime_stubs.cpp)
target_include_directories(scene-resolution-test PRIVATE shim shadps4 third_party/gcn/include)
# The test's Vulkan-Hpp inline methods have a different set of platform extensions
# from vk_platform.cpp. Keep them local and use an explicit dispatcher in the test.
target_compile_options(scene-resolution-test PRIVATE -UNDEBUG -fPIC)
set_source_files_properties(../tests/test_scene_resolution.cpp PROPERTIES
    COMPILE_OPTIONS "-fvisibility=hidden;-fvisibility-inlines-hidden")
target_link_libraries(scene-resolution-test PRIVATE bbgpu)
add_executable(taa-shader-test EXCLUDE_FROM_ALL ../tests/test_taa_shader.cpp ../tests/test_gpu_runtime_stubs.cpp)
target_include_directories(taa-shader-test PRIVATE shim shadps4 ${SHADER_INCLUDE} third_party/gcn/include)
target_compile_options(taa-shader-test PRIVATE -UNDEBUG -fPIC)
set_source_files_properties(../tests/test_taa_shader.cpp PROPERTIES
    COMPILE_OPTIONS "-fvisibility=hidden;-fvisibility-inlines-hidden")
target_link_libraries(taa-shader-test PRIVATE bbgpu)
add_dependencies(taa-shader-test host_shaders)

add_executable(camera-motion-test EXCLUDE_FROM_ALL ../tests/test_camera_motion.cpp ../tests/test_gpu_runtime_stubs.cpp)
target_include_directories(camera-motion-test PRIVATE shim shadps4 ${SHADER_INCLUDE} third_party/gcn/include)
target_compile_options(camera-motion-test PRIVATE -UNDEBUG -fPIC)
target_link_libraries(camera-motion-test PRIVATE bbgpu)
add_dependencies(camera-motion-test host_shaders)

# bbport: FSR 4 benchmark outside the game (tools/fsr4_bench.cpp): ninja -C out/gpu fsr4-bench.
add_executable(upscaler-support-test EXCLUDE_FROM_ALL ../tests/test_upscaler_support.cpp
    shim/bbport_settings.cpp)
target_include_directories(upscaler-support-test PRIVATE shim)
target_compile_options(upscaler-support-test PRIVATE -UNDEBUG)
add_executable(videodec-test EXCLUDE_FROM_ALL ../tests/test_videodec.cpp ../tests/test_gpu_runtime_stubs.cpp)
target_include_directories(videodec-test PRIVATE shim shadps4 third_party/gcn/include)
target_link_libraries(videodec-test PRIVATE bbgpu)
add_executable(image-compat-test EXCLUDE_FROM_ALL ../tests/test_image_compat.cpp ../tests/test_gpu_runtime_stubs.cpp)
target_include_directories(image-compat-test PRIVATE shim shadps4 third_party/gcn/include)
target_compile_options(image-compat-test PRIVATE -fvisibility=hidden -fvisibility-inlines-hidden)
target_link_libraries(image-compat-test PRIVATE bbgpu)

add_executable(performance-sim-test EXCLUDE_FROM_ALL ../tests/performance_sim.cpp ../tests/test_gpu_runtime_stubs.cpp)
target_include_directories(performance-sim-test PRIVATE shim shadps4 third_party/gcn/include)
target_compile_options(performance-sim-test PRIVATE -fvisibility=hidden -fvisibility-inlines-hidden)
target_link_libraries(performance-sim-test PRIVATE bbgpu)

# RelWithDebInfo normally defines NDEBUG. Assertions must stay active in every test.
foreach(test_target IN ITEMS motion-history-test motion-shader-test ui-composition-test
        scene-resolution-test taa-shader-test camera-motion-test upscaler-support-test videodec-test image-compat-test performance-sim-test)
    target_compile_options(${test_target} PRIVATE -UNDEBUG)
    target_compile_definitions(${test_target} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
endforeach()
if (BB_BUILD_TESTS)
    enable_testing()
    # CTest/Test Explorer starts native targets directly, outside test.bat.
    # Preserve the existing MSYS2 CLANG64 runtime search path for those
    # processes without copying DLLs or changing the user's global PATH.
    if (WIN32)
        get_filename_component(BBPORT_CLANG_BIN "${CMAKE_CXX_COMPILER}" DIRECTORY)
    endif()
    foreach(test_target IN ITEMS motion-history-test ui-composition-test upscaler-support-test videodec-test)
        add_test(NAME ${test_target} COMMAND $<TARGET_FILE:${test_target}>)
        set_tests_properties(${test_target} PROPERTIES LABELS "unit" TIMEOUT 30)
    endforeach()
    add_test(NAME motion-shader-test COMMAND $<TARGET_FILE:motion-shader-test>
        "${CMAKE_CURRENT_BINARY_DIR}/test-shaders")
    set_tests_properties(motion-shader-test PROPERTIES LABELS "unit" TIMEOUT 30
        FIXTURES_SETUP motion_spirv)
    add_test(NAME image-compat-shaders COMMAND $<TARGET_FILE:image-compat-test>
        --shaders "${CMAKE_CURRENT_BINARY_DIR}/test-image-shaders")
    set_tests_properties(image-compat-shaders PROPERTIES LABELS "unit" TIMEOUT 30 FIXTURES_SETUP image_spirv)
    add_test(NAME diagnostics-ring COMMAND $<TARGET_FILE:image-compat-test>
        --diagnostics "${CMAKE_CURRENT_BINARY_DIR}/test-diagnostics")
    set_tests_properties(diagnostics-ring PROPERTIES LABELS "unit" TIMEOUT 30)
    add_test(NAME diagnostics-frame COMMAND $<TARGET_FILE:image-compat-test>
        --frame-capture "${CMAKE_CURRENT_BINARY_DIR}/test-frame-capture")
    set_tests_properties(diagnostics-frame PROPERTIES LABELS "unit" TIMEOUT 30)
    add_test(NAME irq-controller-concurrency COMMAND $<TARGET_FILE:image-compat-test> --irq)
    set_tests_properties(irq-controller-concurrency PROPERTIES LABELS "unit" TIMEOUT 30)
    add_test(NAME stencil-readonly-reference COMMAND $<TARGET_FILE:image-compat-test> --stencil)
    set_tests_properties(stencil-readonly-reference PROPERTIES LABELS "unit" TIMEOUT 30)
    add_test(NAME mouse-motion-input COMMAND $<TARGET_FILE:image-compat-test> --mouse-motion)
    set_tests_properties(mouse-motion-input PROPERTIES LABELS "unit" TIMEOUT 30)
    add_test(NAME runtime-shader-bundle COMMAND $<TARGET_FILE:image-compat-test>
        --shader-bundle "${CMAKE_CURRENT_BINARY_DIR}/test-shader-bundle")
    set_tests_properties(runtime-shader-bundle PROPERTIES LABELS "unit" TIMEOUT 30)
    add_test(NAME runtime-cache-guards COMMAND $<TARGET_FILE:image-compat-test> --cache-guards)
    set_tests_properties(runtime-cache-guards PROPERTIES LABELS "unit" TIMEOUT 30)
    add_test(NAME scoped-texture-registry COMMAND $<TARGET_FILE:image-compat-test> --registry-changes)
    set_tests_properties(scoped-texture-registry PROPERTIES LABELS "unit" TIMEOUT 30)
    add_test(NAME snapshot-pixel-conversion COMMAND $<TARGET_FILE:image-compat-test>
        --snapshot-pixels "${CMAKE_CURRENT_BINARY_DIR}/test-snapshot-pixels")
    set_tests_properties(snapshot-pixel-conversion PROPERTIES LABELS "unit" TIMEOUT 30)
    add_test(NAME texture-set-slot-distribution COMMAND $<TARGET_FILE:image-compat-test> --texture-set-distribution)
    set_tests_properties(texture-set-slot-distribution PROPERTIES LABELS "unit" TIMEOUT 30)
    find_program(SPIRV_VALIDATOR NAMES spirv-val)
    if (SPIRV_VALIDATOR)
        foreach(image IN ITEMS 0 1 2)
            add_test(NAME validate-image-${image} COMMAND ${SPIRV_VALIDATOR} --target-env vulkan1.3
                "${CMAKE_CURRENT_BINARY_DIR}/test-image-shaders/image-${image}.spv")
            set_tests_properties(validate-image-${image} PROPERTIES LABELS "unit" TIMEOUT 30 FIXTURES_REQUIRED image_spirv)
        endforeach()
        foreach(shader IN ITEMS vertex-motion vertex-plain fragment-motion fragment-plain)
            add_test(NAME validate-${shader} COMMAND ${SPIRV_VALIDATOR} --target-env vulkan1.3
                "${CMAKE_CURRENT_BINARY_DIR}/test-shaders/${shader}.spv")
            set_tests_properties(validate-${shader} PROPERTIES LABELS "unit" TIMEOUT 30
                FIXTURES_REQUIRED motion_spirv)
        endforeach()
    else()
        message(STATUS "spirv-val not installed: SPIR-V validation is unavailable; no tools installed.")
    endif()
    foreach(test_target IN ITEMS scene-resolution-test taa-shader-test camera-motion-test)
        add_test(NAME ${test_target} COMMAND $<TARGET_FILE:${test_target}>)
        set_tests_properties(${test_target} PROPERTIES LABELS "gpu" TIMEOUT 120)
    endforeach()
    add_test(NAME image-compat-textures COMMAND $<TARGET_FILE:image-compat-test>)
    set_tests_properties(image-compat-textures PROPERTIES LABELS "gpu" TIMEOUT 120)
    add_test(NAME driver-pipeline-cache COMMAND $<TARGET_FILE:image-compat-test>
        --driver-cache "${CMAKE_CURRENT_BINARY_DIR}/test-driver-cache")
    set_tests_properties(driver-pipeline-cache PROPERTIES LABELS "gpu" TIMEOUT 120)
    add_test(NAME runtime-pipeline-prewarm COMMAND $<TARGET_FILE:image-compat-test>
        --runtime-cache "${CMAKE_CURRENT_BINARY_DIR}/test-runtime-cache")
    set_tests_properties(runtime-pipeline-prewarm PROPERTIES LABELS "gpu" TIMEOUT 120)
    add_test(NAME image-read-barrier-memo COMMAND $<TARGET_FILE:image-compat-test> --image-read-memo)
    set_tests_properties(image-read-barrier-memo PROPERTIES LABELS "gpu" TIMEOUT 120)
    add_test(NAME async-frame-snapshot COMMAND $<TARGET_FILE:image-compat-test>
        --snapshot-worker "${CMAKE_CURRENT_BINARY_DIR}/test-snapshot-worker")
    set_tests_properties(async-frame-snapshot PROPERTIES LABELS "gpu" TIMEOUT 120)
    add_test(NAME performance-sim-correctness-cpu COMMAND $<TARGET_FILE:performance-sim-test> --self-test --cpu-only)
    set_tests_properties(performance-sim-correctness-cpu PROPERTIES LABELS "unit" TIMEOUT 30)
    add_test(NAME flat-data-memo-correctness COMMAND $<TARGET_FILE:image-compat-test> --flat-data-memo)
    set_tests_properties(flat-data-memo-correctness PROPERTIES LABELS "unit" TIMEOUT 30)
    add_test(NAME flat-data-memo-gpu-visibility COMMAND $<TARGET_FILE:image-compat-test> --flat-data-visibility)
    set_tests_properties(flat-data-memo-gpu-visibility PROPERTIES LABELS "gpu" TIMEOUT 120)
    add_test(NAME performance-sim-correctness-images COMMAND $<TARGET_FILE:performance-sim-test> --self-test)
    set_tests_properties(performance-sim-correctness-images PROPERTIES LABELS "gpu" TIMEOUT 120)
    if (WIN32)
        get_property(BBPORT_TESTS DIRECTORY PROPERTY TESTS)
        foreach(BBPORT_TEST IN LISTS BBPORT_TESTS)
            set_tests_properties(${BBPORT_TEST} PROPERTIES
                ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${BBPORT_CLANG_BIN}"
                ENVIRONMENT "BB_GPU_USER_DIR=${CMAKE_CURRENT_BINARY_DIR}/test-user")
        endforeach()
    endif()
    set_tests_properties(performance-sim-correctness-cpu performance-sim-correctness-images PROPERTIES
        ENVIRONMENT "BB_GPU_USER_DIR=${CMAKE_CURRENT_BINARY_DIR}/test-user;BB_PERF_STATS=1;BB_IMAGE_READ_MEMO=1")
endif()

add_executable(fsr4-bench EXCLUDE_FROM_ALL tools/fsr4_bench.cpp
    shadps4/video_core/renderer_vulkan/fsr411/fsr411.cpp)
target_include_directories(fsr4-bench PRIVATE shadps4/video_core/renderer_vulkan/fsr411)
target_link_libraries(fsr4-bench PRIVATE ffx-vulkan::fsr4-v07-vulkan ffx-vulkan::fsr4-v07-assets Vulkan::Vulkan)
