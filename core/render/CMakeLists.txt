set(render_include_dir "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(palmier_render STATIC
    render_plan.cpp
    cpu_renderer.cpp
)
target_include_directories(palmier_render PUBLIC "${render_include_dir}")
target_compile_features(palmier_render PUBLIC cxx_std_20)
target_compile_options(
    palmier_render
    PRIVATE /W4 /WX /permissive- /Zc:__cplusplus /utf-8
)

if(BUILD_TESTING)
    add_executable(palmier_render_plan_tests tests/render_plan_tests.cpp)
    target_link_libraries(palmier_render_plan_tests PRIVATE palmier_render)
    target_compile_options(
        palmier_render_plan_tests
        PRIVATE /W4 /WX /permissive- /Zc:__cplusplus /utf-8
    )
    add_test(NAME render_plan.direct_api COMMAND palmier_render_plan_tests)
endif()
