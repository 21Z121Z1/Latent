# Temporal operations extend the existing semantic/reference core and backend.
target_sources(latent_core PRIVATE
    src/runtime/TemporalPipeline.cpp
    src/runtime/CapturePlan.cpp
    src/runtime/CfaTransport.cpp
    src/reference/TemporalReconstruct.cpp
    src/reference/TemporalAlignment.cpp)

if(LATENT_ENABLE_VULKAN_RUNTIME)
    target_compile_definitions(latent_core PRIVATE LATENT_ENABLE_VULKAN_RUNTIME)
    target_sources(latent_core PRIVATE src/vulkan/TemporalFusion.cpp)
    add_custom_command(
        OUTPUT "${GENERATED_SHADER_DIR}/temporal_fusion_spv.h"
        COMMAND $<TARGET_FILE:glslang-standalone> -V --target-env vulkan1.1
                --vn temporal_fusion_spv -o "${GENERATED_SHADER_DIR}/temporal_fusion_spv.h"
                "${CMAKE_CURRENT_SOURCE_DIR}/shaders/temporal_fusion.comp"
        DEPENDS "shaders/temporal_fusion.comp" glslang-standalone
        VERBATIM)
    add_custom_target(latent_temporal_shaders DEPENDS "${GENERATED_SHADER_DIR}/temporal_fusion_spv.h")
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
    latent_temporal_test(latent_temporal_tests tests/test_temporal.cpp)
    latent_temporal_test(latent_capture_plan_tests tests/test_capture_plan.cpp)
    latent_temporal_test(latent_cfa_transport_tests tests/test_cfa_transport.cpp)
    if(LATENT_ENABLE_VULKAN_RUNTIME)
        latent_temporal_test(latent_temporal_vulkan_tests tests/test_temporal_vulkan.cpp)
    endif()
endif()
