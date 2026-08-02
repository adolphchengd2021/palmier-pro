add_executable(
    palmier_ffmpeg_distribution_probe
    distribution_probe.cpp
)
set_target_properties(
    palmier_ffmpeg_distribution_probe
    PROPERTIES WIN32_EXECUTABLE FALSE
)
target_link_libraries(
    palmier_ffmpeg_distribution_probe
    PRIVATE palmier_ffmpeg
)
target_compile_features(
    palmier_ffmpeg_distribution_probe
    PRIVATE cxx_std_20
)
target_compile_options(
    palmier_ffmpeg_distribution_probe
    PRIVATE /W4 /WX /permissive- /Zc:__cplusplus /utf-8
)

if(BUILD_TESTING)
    add_test(
        NAME packaging.ffmpeg_distribution_probe
        COMMAND palmier_ffmpeg_distribution_probe
    )
    set_tests_properties(
        packaging.ffmpeg_distribution_probe
        PROPERTIES TIMEOUT 30
    )
endif()
