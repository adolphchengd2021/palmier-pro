set(project_render_include_dir "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(palmier_project_render STATIC
    project_render_compiler.cpp
)
target_include_directories(palmier_project_render PUBLIC "${project_render_include_dir}")
target_compile_features(palmier_project_render PUBLIC cxx_std_20)
target_link_libraries(
    palmier_project_render
    PUBLIC palmier_project_reader palmier_render
)
target_compile_options(
    palmier_project_render
    PRIVATE /W4 /WX /permissive- /Zc:__cplusplus /utf-8
)

if(BUILD_TESTING)
    add_executable(
        palmier_project_render_compiler_tests
        tests/project_render_compiler_tests.cpp
    )
    target_link_libraries(
        palmier_project_render_compiler_tests
        PRIVATE palmier_project_render
    )
    target_compile_options(
        palmier_project_render_compiler_tests
        PRIVATE /W4 /WX /permissive- /Zc:__cplusplus /utf-8
    )
    add_test(
        NAME project_render.static_video_layer
        COMMAND palmier_project_render_compiler_tests "${PROJECT_SOURCE_DIR}"
    )
endif()
