set(audio_wasapi_include_dir "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(
    palmier_audio_wasapi
    STATIC
    audio_clock.cpp
    wasapi_environment_probe.cpp
    wasapi_native_stream.cpp
    wasapi_output.cpp
    wasapi_silent_output_probe.cpp
)
target_include_directories(
    palmier_audio_wasapi
    PUBLIC "${audio_wasapi_include_dir}"
)
target_compile_features(palmier_audio_wasapi PUBLIC cxx_std_20)
target_compile_definitions(
    palmier_audio_wasapi
    PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN
)
target_compile_options(
    palmier_audio_wasapi
    PRIVATE /W4 /WX /permissive- /Zc:__cplusplus /utf-8
)
target_link_libraries(palmier_audio_wasapi PRIVATE ole32)
target_link_libraries(palmier_audio_wasapi PUBLIC palmier_audio_core)

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

add_executable(
    palmier_wasapi_output_silence_smoke
    app/output_silence_smoke_main.cpp
)
target_link_libraries(
    palmier_wasapi_output_silence_smoke
    PRIVATE palmier_audio_wasapi
)
target_compile_options(
    palmier_wasapi_output_silence_smoke
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
    add_executable(
        palmier_wasapi_output_tests
        tests/wasapi_output_tests.cpp
    )
    target_link_libraries(
        palmier_wasapi_output_tests
        PRIVATE palmier_audio_wasapi
    )
    target_include_directories(
        palmier_wasapi_output_tests
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_compile_options(
        palmier_wasapi_output_tests
        PRIVATE /W4 /WX /permissive- /Zc:__cplusplus /utf-8
    )
    add_test(
        NAME audio_wasapi.output_state
        COMMAND palmier_wasapi_output_tests
    )
    add_test(
        NAME audio_wasapi.output_silence_smoke
        COMMAND palmier_wasapi_output_silence_smoke
    )
    set_tests_properties(
        audio_wasapi.environment_probe
        audio_wasapi.output_silence_smoke
        PROPERTIES RUN_SERIAL TRUE TIMEOUT 30
    )
endif()
