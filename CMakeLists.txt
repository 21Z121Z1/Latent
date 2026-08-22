cmake_minimum_required(VERSION 3.20)
project(latent VERSION 0.1.0 LANGUAGES CXX)

option(LATENT_BUILD_TESTS "Build Latent tests" ON)
option(LATENT_STRICT_WARNINGS "Treat compiler warnings as errors" ON)
option(LATENT_ENABLE_VULKAN_RUNTIME
    "Enable the Vulkan runtime capability adapter (FetchContent volk + headers)" ON)

if(LATENT_ENABLE_VULKAN_RUNTIME)
    include(FetchContent)
    FetchContent_Declare(vulkan_headers
        GIT_SHALLOW TRUE
        GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
        GIT_TAG v1.3.299
    )
    # We only consume the C headers through volk; skip the C++ module target,
    # whose toolchain support is uneven across compilers.
    set(VULKAN_HEADERS_ENABLE_MODULE OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(volk
        GIT_SHALLOW TRUE
        GIT_REPOSITORY https://github.com/zeux/volk.git
        GIT_TAG 1.3.295
    )
    FetchContent_MakeAvailable(vulkan_headers)
    # Pin volk to the fetched headers (checked before FindVulkan by volk's
    # own CMake), keeping macro availability identical across platforms.
    set(VULKAN_HEADERS_INSTALL_DIR "${vulkan_headers_SOURCE_DIR}" CACHE PATH ""
        FORCE)
    FetchContent_MakeAvailable(volk)

    # Build-time SPIR-V compilation. ENABLE_OPT=OFF drops the SPIRV-Tools
    # dependency; our per-target warning flags never reach glslang sources.
    set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
    set(ENABLE_HLSL OFF CACHE BOOL "" FORCE)
    set(ENABLE_GLSLANG_BINARIES ON CACHE BOOL "" FORCE)
    set(SKIP_GLSLANG_INSTALL ON CACHE BOOL "" FORCE)
    FetchContent_Declare(glslang
        GIT_SHALLOW TRUE
        GIT_REPOSITORY https://github.com/KhronosGroup/glslang.git
        GIT_TAG 16.5.0
    )
    FetchContent_MakeAvailable(glslang)

    set(GENERATED_SHADER_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated_shaders")
    file(MAKE_DIRECTORY "${GENERATED_SHADER_DIR}")
    add_custom_command(
        OUTPUT "${GENERATED_SHADER_DIR}/sensor_preprocess_spv.h"
        COMMAND $<TARGET_FILE:glslang-standalone> -V --target-env vulkan1.1
                --vn sensor_preprocess_spv
                -o "${GENERATED_SHADER_DIR}/sensor_preprocess_spv.h"
                "${CMAKE_CURRENT_SOURCE_DIR}/shaders/sensor_preprocess.comp"
        DEPENDS "shaders/sensor_preprocess.comp" glslang-standalone
        COMMENT "Compiling sensor_preprocess.comp to SPIR-V header"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${GENERATED_SHADER_DIR}/demosaic_color_spv.h"
        COMMAND $<TARGET_FILE:glslang-standalone> -V --target-env vulkan1.1
                --vn demosaic_color_spv
                -o "${GENERATED_SHADER_DIR}/demosaic_color_spv.h"
                "${CMAKE_CURRENT_SOURCE_DIR}/shaders/demosaic_color.comp"
        DEPENDS "shaders/demosaic_color.comp" glslang-standalone
        COMMENT "Compiling demosaic_color.comp to SPIR-V header"
        VERBATIM
    )
    add_custom_target(latent_shaders
        DEPENDS "${GENERATED_SHADER_DIR}/sensor_preprocess_spv.h"
                "${GENERATED_SHADER_DIR}/demosaic_color_spv.h")
endif()

set(LATENT_CORE_SOURCES
    src/graph/ImagingIR.cpp
    src/imaging/ColorScience.cpp
    src/imaging/RawPacking.cpp
    src/reference/DngColor.cpp
    src/reference/Demosaic.cpp
    src/reference/NoisePropagation.cpp
    src/reference/RawNormalize.cpp
    src/reference/ReferenceReconstruct.cpp
    src/reference/SensorLinearOps.cpp
    src/render/AcesToneScale.cpp
    src/render/OutputEncoding.cpp
    src/render/SceneAnalysis.cpp
    src/vulkan/DeviceCaps.cpp
    src/vulkan/IngressPlan.cpp
)

if(LATENT_ENABLE_VULKAN_RUNTIME)
    list(APPEND LATENT_CORE_SOURCES
        src/vulkan/VulkanRuntime.cpp
        src/vulkan/ComputeRunner.cpp
        src/vulkan/SensorPreprocessKernel.cpp
        src/vulkan/DemosaicColorKernel.cpp
    )
endif()

add_library(latent_core ${LATENT_CORE_SOURCES})

if(LATENT_ENABLE_VULKAN_RUNTIME)
    add_dependencies(latent_core latent_shaders)
    target_include_directories(latent_core PRIVATE
        "${GENERATED_SHADER_DIR}")
endif()

add_library(latent::core ALIAS latent_core)

target_include_directories(latent_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

if(LATENT_ENABLE_VULKAN_RUNTIME)
    target_link_libraries(latent_core PUBLIC volk::volk)
endif()

target_compile_features(latent_core PUBLIC cxx_std_20)

# The reference backend must be bit-reproducible across compilers; GNU
# contracts FP expressions into FMA by default, which shifts last bits.
target_compile_options(latent_core PRIVATE
    $<$<CXX_COMPILER_ID:GNU>:-ffp-contract=off>)

if(MSVC)
    target_compile_options(latent_core PRIVATE /W4)
    if(LATENT_STRICT_WARNINGS)
        target_compile_options(latent_core PRIVATE /WX)
    endif()
else()
    target_compile_options(latent_core PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    if(LATENT_STRICT_WARNINGS)
        target_compile_options(latent_core PRIVATE -Werror)
    endif()
endif()

if(LATENT_BUILD_TESTS)
    enable_testing()
    add_executable(latent_tests tests/test_main.cpp)
    target_link_libraries(latent_tests PRIVATE latent::core)
    target_compile_features(latent_tests PRIVATE cxx_std_20)
    if(LATENT_ENABLE_VULKAN_RUNTIME)
        target_compile_definitions(latent_tests PRIVATE LATENT_ENABLE_VULKAN_RUNTIME)
    endif()
    target_compile_options(latent_tests PRIVATE
        $<$<CXX_COMPILER_ID:GNU>:-ffp-contract=off>)
    if(MSVC)
        target_compile_options(latent_tests PRIVATE /W4)
        if(LATENT_STRICT_WARNINGS)
            target_compile_options(latent_tests PRIVATE /WX)
        endif()
    else()
        target_compile_options(latent_tests PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
        if(LATENT_STRICT_WARNINGS)
            target_compile_options(latent_tests PRIVATE -Werror)
        endif()
    endif()
    add_test(NAME latent_tests COMMAND latent_tests)

    add_executable(latent_render_tests tests/test_render.cpp)
    target_link_libraries(latent_render_tests PRIVATE latent::core)
    target_compile_features(latent_render_tests PRIVATE cxx_std_20)
    target_compile_options(latent_render_tests PRIVATE
        $<$<CXX_COMPILER_ID:GNU>:-ffp-contract=off>)
    if(MSVC)
        target_compile_options(latent_render_tests PRIVATE /W4)
        if(LATENT_STRICT_WARNINGS)
            target_compile_options(latent_render_tests PRIVATE /WX)
        endif()
    else()
        target_compile_options(latent_render_tests PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
        if(LATENT_STRICT_WARNINGS)
            target_compile_options(latent_render_tests PRIVATE -Werror)
        endif()
    endif()
    add_test(NAME latent_render_tests COMMAND latent_render_tests)

    if(LATENT_ENABLE_VULKAN_RUNTIME)
        add_executable(latent_compute_runner_lifetime_tests
            tests/test_compute_runner_lifetime.cpp)
        target_link_libraries(latent_compute_runner_lifetime_tests PRIVATE latent::core)
        target_compile_features(latent_compute_runner_lifetime_tests PRIVATE cxx_std_20)
        target_compile_options(latent_compute_runner_lifetime_tests PRIVATE
            $<$<CXX_COMPILER_ID:GNU>:-ffp-contract=off>)
        if(MSVC)
            target_compile_options(latent_compute_runner_lifetime_tests PRIVATE /W4)
            if(LATENT_STRICT_WARNINGS)
                target_compile_options(latent_compute_runner_lifetime_tests PRIVATE /WX)
            endif()
        else()
            target_compile_options(latent_compute_runner_lifetime_tests PRIVATE
                -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
            if(LATENT_STRICT_WARNINGS)
                target_compile_options(latent_compute_runner_lifetime_tests PRIVATE -Werror)
            endif()
        endif()
        add_test(NAME latent_compute_runner_lifetime_tests
            COMMAND latent_compute_runner_lifetime_tests)
    endif()
endif()
