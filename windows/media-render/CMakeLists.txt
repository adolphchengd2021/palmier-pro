set(media_render_include_dir "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(palmier_media_render STATIC render_source_adapter.cpp)
target_include_directories(
    palmier_media_render
    PUBLIC "${media_render_include_dir}"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
)
target_compile_features(palmier_media_render PUBLIC cxx_std_20)
target_compile_options(
    palmier_media_render
    PRIVATE /W4 /WX /permissive- /Zc:__cplusplus /utf-8
)
target_link_libraries(
    palmier_media_render
    PUBLIC palmier_media_ffmpeg palmier_render
)

if(BUILD_TESTING)
    add_executable(
        palmier_media_render_tests
        tests/render_source_adapter_tests.cpp
    )
    target_link_libraries(
        palmier_media_render_tests
        PRIVATE palmier_media_render palmier_render_d3d11
    )
    target_include_directories(
        palmier_media_render_tests
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_compile_options(
        palmier_media_render_tests
        PRIVATE /W4 /WX /permissive- /Zc:__cplusplus /utf-8
    )
    add_test(NAME media_render.adapter_parity COMMAND palmier_media_render_tests)
    set_tests_properties(
        media_render.adapter_parity
        PROPERTIES RUN_SERIAL TRUE TIMEOUT 60
    )

    target_link_libraries(
        palmier_media_ffmpeg_tests
        PRIVATE palmier_media_render
    )
endif()
