cmake_minimum_required(VERSION 3.20)
project(latent VERSION 0.1.0 LANGUAGES CXX)

option(LATENT_BUILD_TESTS "Build Latent tests" ON)
option(LATENT_STRICT_WARNINGS "Treat compiler warnings as errors" ON)
option(LATENT_ENABLE_VULKAN_RUNTIME
    "Enable the Vulkan runtime capability adapter (FetchContent volk + headers)" ON)

if(LATENT_ENABLE_VULKAN_RUNTIME)
    include(FetchContent)
    FetchContent_Declare(vulkan_headers
        GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
        GIT_TAG v1.3.299
    )
    # We only consume the C headers through volk; skip the C++ module target,
    # whose toolchain support is uneven across compilers.
    set(VULKAN_HEADERS_ENABLE_MODULE OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(volk
        GIT_REPOSITORY https://github.com/zeux/volk.git
        GIT_TAG 1.3.295
    )
    FetchContent_MakeAvailable(vulkan_headers)
    # Pin volk to the fetched headers (checked before FindVulkan by volk's
    # own CMake), keeping macro availability identical across platforms.
    set(VULKAN_HEADERS_INSTALL_DIR "${vulkan_headers_SOURCE_DIR}" CACHE PATH ""
        FORCE)
    FetchContent_MakeAvailable(volk)
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
    src/vulkan/DeviceCaps.cpp
    src/vulkan/IngressPlan.cpp
)

if(LATENT_ENABLE_VULKAN_RUNTIME)
    list(APPEND LATENT_CORE_SOURCES src/vulkan/VulkanRuntime.cpp)
endif()

add_library(latent_core ${LATENT_CORE_SOURCES})

add_library(latent::core ALIAS latent_core)

target_include_directories(latent_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

if(LATENT_ENABLE_VULKAN_RUNTIME)
    target_link_libraries(latent_core PUBLIC volk::volk)
endif()

target_compile_features(latent_core PUBLIC cxx_std_20)

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
endif()
