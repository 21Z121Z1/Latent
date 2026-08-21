cmake_minimum_required(VERSION 3.20)
project(latent VERSION 0.1.0 LANGUAGES CXX)

option(LATENT_BUILD_TESTS "Build Latent tests" ON)
option(LATENT_STRICT_WARNINGS "Treat compiler warnings as errors" ON)

add_library(latent_core
    src/graph/ImagingIR.cpp
    src/reference/RawNormalize.cpp
    src/reference/ReferenceReconstruct.cpp
    src/vulkan/DeviceCaps.cpp
)

add_library(latent::core ALIAS latent_core)

target_include_directories(latent_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

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
