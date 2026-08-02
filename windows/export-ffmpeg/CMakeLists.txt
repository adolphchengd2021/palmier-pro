set(export_ffmpeg_include_dir "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(palmier_export_ffmpeg STATIC
    h264_project_exporter.cpp
    project_clip_h264_export_workflow.cpp
)
target_include_directories(
    palmier_export_ffmpeg
    PUBLIC "${export_ffmpeg_include_dir}"
)
target_compile_features(palmier_export_ffmpeg PUBLIC cxx_std_20)
target_compile_options(
    palmier_export_ffmpeg
    PRIVATE /W4 /WX /permissive- /Zc:__cplusplus /utf-8
)
target_link_libraries(
    palmier_export_ffmpeg
    PUBLIC palmier_project_render
    PRIVATE
        palmier_ffmpeg
        palmier_media_ffmpeg
        palmier_media_render
        palmier_project_reader
        palmier_render
)

if(BUILD_TESTING)
    add_executable(
        palmier_h264_project_exporter_tests
        tests/h264_project_exporter_tests.cpp
    )
    target_include_directories(
        palmier_h264_project_exporter_tests
        PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}"
            "${CMAKE_CURRENT_SOURCE_DIR}/../media-ffmpeg/tests"
    )
    target_link_libraries(
        palmier_h264_project_exporter_tests
        PRIVATE palmier_export_ffmpeg palmier_media_ffmpeg
    )
    target_compile_options(
        palmier_h264_project_exporter_tests
        PRIVATE /W4 /WX /permissive- /Zc:__cplusplus /utf-8
    )
    add_test(
        NAME export_ffmpeg.contract
        COMMAND palmier_h264_project_exporter_tests --contract
    )
    set_tests_properties(export_ffmpeg.contract PROPERTIES TIMEOUT 30)
    add_test(
        NAME export_ffmpeg.h264_project_native
        COMMAND palmier_h264_project_exporter_tests --native
    )
    set_tests_properties(
        export_ffmpeg.h264_project_native
        PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77
    )
endif()
