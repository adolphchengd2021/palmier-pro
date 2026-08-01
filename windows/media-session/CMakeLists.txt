set(media_session_include_dir "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(
    palmier_media_session STATIC
    audio_playback_session.cpp
    presentation_video_buffer.cpp
    presentation_video_decode_pump.cpp
    presentation_audio_decode_pump.cpp
)
target_include_directories(
    palmier_media_session
    PUBLIC "${media_session_include_dir}"
)
target_compile_features(palmier_media_session PUBLIC cxx_std_20)
target_compile_options(
    palmier_media_session
    PRIVATE /W4 /WX /permissive- /Zc:__cplusplus /utf-8
)
target_link_libraries(
    palmier_media_session
    PUBLIC palmier_audio_wasapi palmier_media_render palmier_media_ffmpeg
)

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

    add_executable(
        palmier_ffmpeg_presentation_pipeline_tests
        tests/ffmpeg_presentation_pipeline_tests.cpp
    )
    target_link_libraries(
        palmier_ffmpeg_presentation_pipeline_tests
        PRIVATE palmier_media_session palmier_render_d3d11
    )
    target_include_directories(
        palmier_ffmpeg_presentation_pipeline_tests
        PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}"
            "${CMAKE_CURRENT_SOURCE_DIR}/../media-ffmpeg/tests"
    )
    target_compile_options(
        palmier_ffmpeg_presentation_pipeline_tests
        PRIVATE /W4 /WX /permissive- /Zc:__cplusplus /utf-8
    )
    add_test(
        NAME media_session.ffmpeg_presentation_pipeline
        COMMAND palmier_ffmpeg_presentation_pipeline_tests
    )
    set_tests_properties(
        media_session.ffmpeg_presentation_pipeline
        PROPERTIES RUN_SERIAL TRUE TIMEOUT 60
    )

    add_executable(
        palmier_ffmpeg_wasapi_audio_pipeline_tests
        tests/ffmpeg_wasapi_audio_pipeline_tests.cpp
    )
    target_link_libraries(
        palmier_ffmpeg_wasapi_audio_pipeline_tests
        PRIVATE palmier_media_session palmier_audio_wasapi
    )
    target_include_directories(
        palmier_ffmpeg_wasapi_audio_pipeline_tests
        PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/../audio-wasapi"
            "${CMAKE_CURRENT_SOURCE_DIR}/../media-ffmpeg/tests"
    )
    target_compile_options(
        palmier_ffmpeg_wasapi_audio_pipeline_tests
        PRIVATE /W4 /WX /permissive- /Zc:__cplusplus /utf-8
    )
    add_test(
        NAME media_session.ffmpeg_wasapi_audio_pipeline
        COMMAND palmier_ffmpeg_wasapi_audio_pipeline_tests
    )
    set_tests_properties(
        media_session.ffmpeg_wasapi_audio_pipeline
        PROPERTIES TIMEOUT 60
    )

    add_executable(
        palmier_audio_playback_session_tests
        tests/audio_playback_session_tests.cpp
    )
    target_link_libraries(
        palmier_audio_playback_session_tests
        PRIVATE palmier_media_session
    )
    target_include_directories(
        palmier_audio_playback_session_tests
        PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/../audio-wasapi"
            "${CMAKE_CURRENT_SOURCE_DIR}/../media-ffmpeg/tests"
    )
    target_compile_options(
        palmier_audio_playback_session_tests
        PRIVATE /W4 /WX /permissive- /Zc:__cplusplus /utf-8
    )
    add_test(
        NAME media_session.audio_playback_session
        COMMAND palmier_audio_playback_session_tests
    )
    set_tests_properties(
        media_session.audio_playback_session
        PROPERTIES TIMEOUT 30
    )
endif()
