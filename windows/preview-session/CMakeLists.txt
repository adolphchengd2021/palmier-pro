set(preview_session_include_dir "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(
    palmier_preview_session STATIC
    preview_presentation_session.cpp
)
target_include_directories(
    palmier_preview_session
    PUBLIC "${preview_session_include_dir}"
)
target_compile_features(palmier_preview_session PUBLIC cxx_std_20)
target_compile_options(
    palmier_preview_session
    PRIVATE /W4 /WX /permissive- /Zc:__cplusplus /utf-8
)
target_link_libraries(
    palmier_preview_session
    PUBLIC palmier_media_session palmier_project_render palmier_render_d3d11
)

if(BUILD_TESTING)
    add_executable(
        palmier_preview_presentation_session_tests
        tests/preview_presentation_session_tests.cpp
    )
    target_link_libraries(
        palmier_preview_presentation_session_tests
        PRIVATE palmier_preview_session
    )
    target_compile_options(
        palmier_preview_presentation_session_tests
        PRIVATE /W4 /WX /permissive- /Zc:__cplusplus /utf-8
    )
    add_test(
        NAME preview_session.presentation_coordinator
        COMMAND palmier_preview_presentation_session_tests
    )
    set_tests_properties(
        preview_session.presentation_coordinator
        PROPERTIES TIMEOUT 30
    )
endif()
