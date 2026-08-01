set(media_session_include_dir "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(palmier_media_session STATIC presentation_video_buffer.cpp)
target_include_directories(
    palmier_media_session
    PUBLIC "${media_session_include_dir}"
)
target_compile_features(palmier_media_session PUBLIC cxx_std_20)
target_compile_options(
    palmier_media_session
    PRIVATE /W4 /WX /permissive- /Zc:__cplusplus /utf-8
)
target_link_libraries(palmier_media_session PUBLIC palmier_media_render)

if(BUILD_TESTING)
    add_executable(
        palmier_media_session_tests
        tests/presentation_video_buffer_tests.cpp
    )
    target_link_libraries(
        palmier_media_session_tests
        PRIVATE palmier_media_session
    )
    target_include_directories(
        palmier_media_session_tests
        PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}"
            "${CMAKE_CURRENT_SOURCE_DIR}/../media-render"
    )
    target_compile_options(
        palmier_media_session_tests
        PRIVATE /W4 /WX /permissive- /Zc:__cplusplus /utf-8
    )
    add_test(
        NAME media_session.presentation_video_buffer
        COMMAND palmier_media_session_tests
    )
    set_tests_properties(
        media_session.presentation_video_buffer
        PROPERTIES TIMEOUT 30
    )
endif()
