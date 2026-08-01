find_package(FFMPEG REQUIRED)

if(NOT FFMPEG_VERSION VERSION_EQUAL "8.1.2")
    message(FATAL_ERROR "Palmier Pro requires FFmpeg 8.1.2, found ${FFMPEG_VERSION}")
endif()

add_library(palmier_ffmpeg INTERFACE)
target_include_directories(
    palmier_ffmpeg
    SYSTEM INTERFACE ${FFMPEG_INCLUDE_DIRS}
)
target_link_libraries(palmier_ffmpeg INTERFACE ${FFMPEG_LIBRARIES})

set(media_ffmpeg_include_dir "${CMAKE_CURRENT_SOURCE_DIR}/include")
add_library(palmier_media_ffmpeg STATIC ffmpeg_media_reader.cpp)
target_include_directories(
    palmier_media_ffmpeg
    PUBLIC "${media_ffmpeg_include_dir}"
)
target_compile_features(palmier_media_ffmpeg PUBLIC cxx_std_20)
target_compile_options(
    palmier_media_ffmpeg
    PRIVATE /W4 /WX /permissive- /Zc:__cplusplus /utf-8
)
target_link_libraries(
    palmier_media_ffmpeg
    PUBLIC palmier_audio_core
    PRIVATE palmier_ffmpeg
)

if(BUILD_TESTING)
    add_executable(
        palmier_media_ffmpeg_tests
        tests/ffmpeg_media_reader_tests.cpp
    )
    target_link_libraries(
        palmier_media_ffmpeg_tests
        PRIVATE palmier_media_ffmpeg
    )
    target_compile_options(
        palmier_media_ffmpeg_tests
        PRIVATE /W4 /WX /permissive- /Zc:__cplusplus /utf-8
    )
    add_test(NAME media_ffmpeg.direct_api COMMAND palmier_media_ffmpeg_tests)
    set_tests_properties(media_ffmpeg.direct_api PROPERTIES TIMEOUT 30)
endif()
