# Temporal operations extend the existing semantic/reference core and backend.
target_sources(latent_core PRIVATE
    src/imaging/SensorSampling.cpp
    src/runtime/RawTileSource.cpp
    src/runtime/Camera2Sampling.cpp
    src/runtime/TiledReconstruction.cpp
    src/reference/DirectReconstruct.cpp
    src/runtime/TemporalPipeline.cpp
    src/runtime/CapturePlan.cpp
    src/runtime/CfaTransport.cpp
    src/reference/TemporalReconstruct.cpp
    src/reference/TemporalAlignment.cpp)

if(LATENT_ENABLE_VULKAN_RUNTIME)
    target_compile_definitions(latent_core PRIVATE LATENT_ENABLE_VULKAN_RUNTIME)
    target_sources(latent_core PRIVATE src/vulkan/TemporalFusion.cpp src/vulkan/DirectReconstruction.cpp)
    add_custom_command(
        OUTPUT "${GENERATED_SHADER_DIR}/temporal_fusion_spv.h"
        COMMAND $<TARGET_FILE:glslang-standalone> -V --target-env vulkan1.1
                --vn temporal_fusion_spv -o "${GENERATED_SHADER_DIR}/temporal_fusion_spv.h"
                "${CMAKE_CURRENT_SOURCE_DIR}/shaders/temporal_fusion.comp"
        DEPENDS "shaders/temporal_fusion.comp" glslang-standalone
        VERBATIM)
    add_custom_command(
        OUTPUT "${GENERATED_SHADER_DIR}/direct_reconstruction_spv.h"
        COMMAND $<TARGET_FILE:glslang-standalone> -V --target-env vulkan1.1
                --vn direct_reconstruction_spv -o "${GENERATED_SHADER_DIR}/direct_reconstruction_spv.h"
                "${CMAKE_CURRENT_SOURCE_DIR}/shaders/direct_reconstruction.comp"
        DEPENDS "shaders/direct_reconstruction.comp" glslang-standalone VERBATIM)
    add_custom_target(latent_temporal_shaders DEPENDS "${GENERATED_SHADER_DIR}/direct_reconstruction_spv.h" "${GENERATED_SHADER_DIR}/temporal_fusion_spv.h")
    add_dependencies(latent_core latent_temporal_shaders)
endif()

function(latent_temporal_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE latent::core)
    if(MSVC)
        target_compile_options(${name} PRIVATE /W4)
        if(LATENT_STRICT_WARNINGS)
            target_compile_options(${name} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${name} PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow -ffp-contract=off)
        if(LATENT_STRICT_WARNINGS)
            target_compile_options(${name} PRIVATE -Werror)
        endif()
    endif()
    add_test(NAME ${name} COMMAND ${name})
endfunction()
if(LATENT_BUILD_TESTS)
    add_executable(latent_temporal_benchmark tools/benchmark_temporal.cpp)
    target_link_libraries(latent_temporal_benchmark PRIVATE latent::core)
    if(MSVC)
        target_compile_options(latent_temporal_benchmark PRIVATE /W4)
        if(LATENT_STRICT_WARNINGS)
            target_compile_options(latent_temporal_benchmark PRIVATE /WX)
        endif()
    else()
        target_compile_options(latent_temporal_benchmark PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow -ffp-contract=off)
        if(LATENT_STRICT_WARNINGS)
            target_compile_options(latent_temporal_benchmark PRIVATE -Werror)
        endif()
    endif()
    add_executable(latent_highres_benchmark tools/benchmark_highres.cpp)
    target_link_libraries(latent_highres_benchmark PRIVATE latent::core)
    if(NOT MSVC)
        target_compile_options(latent_highres_benchmark PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow -ffp-contract=off)
        if(LATENT_STRICT_WARNINGS)
            target_compile_options(latent_highres_benchmark PRIVATE -Werror)
        endif()
    endif()
    latent_temporal_test(latent_highres_tests tests/test_highres.cpp)
    latent_temporal_test(latent_camera2_sampling_tests tests/test_camera2_sampling.cpp)
    latent_temporal_test(latent_temporal_tests tests/test_temporal.cpp)
    latent_temporal_test(latent_registration_tests tests/test_registration.cpp)
    latent_temporal_test(latent_registration_quality_tests tests/test_registration_quality.cpp)
    latent_temporal_test(latent_capture_plan_tests tests/test_capture_plan.cpp)
    latent_temporal_test(latent_cfa_transport_tests tests/test_cfa_transport.cpp)
    if(LATENT_ENABLE_VULKAN_RUNTIME)
        latent_temporal_test(latent_highres_vulkan_tests tests/test_highres_vulkan.cpp)
        latent_temporal_test(latent_temporal_vulkan_tests tests/test_temporal_vulkan.cpp)
    endif()
endif()
