set(audio_wasapi_include_dir "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(
    palmier_audio_wasapi
    STATIC
    audio_clock.cpp
    wasapi_environment_probe.cpp
)
target_include_directories(
    palmier_audio_wasapi
    PUBLIC "${audio_wasapi_include_dir}"
)
target_compile_features(palmier_audio_wasapi PUBLIC cxx_std_20)
target_compile_options(
    palmier_audio_wasapi
    PRIVATE /W4 /WX /permissive- /Zc:__cplusplus /utf-8
)
target_link_libraries(palmier_audio_wasapi PRIVATE ole32)

add_executable(
    palmier_wasapi_environment_probe
    app/environment_probe_main.cpp
)
target_link_libraries(
    palmier_wasapi_environment_probe
    PRIVATE palmier_audio_wasapi
)
target_compile_options(
    palmier_wasapi_environment_probe
    PRIVATE /W4 /WX /permissive- /Zc:__cplusplus /utf-8
)

if(BUILD_TESTING)
    add_executable(
        palmier_audio_clock_tests
        tests/audio_clock_tests.cpp
    )
    target_link_libraries(
        palmier_audio_clock_tests
        PRIVATE palmier_audio_wasapi
    )
    target_compile_options(
        palmier_audio_clock_tests
        PRIVATE /W4 /WX /permissive- /Zc:__cplusplus /utf-8
    )
    add_test(NAME audio_wasapi.clock_math COMMAND palmier_audio_clock_tests)

    add_executable(
        palmier_wasapi_environment_tests
        tests/wasapi_environment_tests.cpp
    )
    target_link_libraries(
        palmier_wasapi_environment_tests
        PRIVATE palmier_audio_wasapi
    )
    target_include_directories(
        palmier_wasapi_environment_tests
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_compile_options(
        palmier_wasapi_environment_tests
        PRIVATE /W4 /WX /permissive- /Zc:__cplusplus /utf-8
    )
    add_test(
        NAME audio_wasapi.environment_contract
        COMMAND palmier_wasapi_environment_tests
    )
    add_test(
        NAME audio_wasapi.environment_probe
        COMMAND palmier_wasapi_environment_probe
    )
    set_tests_properties(
        audio_wasapi.environment_probe
        PROPERTIES RUN_SERIAL TRUE TIMEOUT 30
    )
endif()
