set(probe_include_dir "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(palmier_contract_probe_core STATIC
    src/repository_contract_probe.cpp
    src/top_level_json.cpp
)
target_include_directories(palmier_contract_probe_core PUBLIC "${probe_include_dir}")
target_compile_features(palmier_contract_probe_core PUBLIC cxx_std_20)

add_executable(palmier_contract_probe app/main.cpp)
target_link_libraries(palmier_contract_probe PRIVATE palmier_contract_probe_core)

foreach(target palmier_contract_probe_core palmier_contract_probe)
    target_compile_options(${target} PRIVATE /W4 /WX /permissive- /Zc:__cplusplus /utf-8)
endforeach()

if(BUILD_TESTING)
    set(test_data "${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
    set(expect_failure "${CMAKE_CURRENT_SOURCE_DIR}/tests/ExpectFailure.cmake")

    add_test(
        NAME contract_probe.repository_v1
        COMMAND palmier_contract_probe --repo-root "${PROJECT_SOURCE_DIR}"
    )
    add_test(
        NAME contract_probe.valid_version
        COMMAND palmier_contract_probe --contract-version-file "${test_data}/valid-version.json"
    )

    set(unicode_dir "${CMAKE_CURRENT_BINARY_DIR}/unicode-合同")
    file(MAKE_DIRECTORY "${unicode_dir}")
    configure_file(
        "${test_data}/valid-version.json"
        "${unicode_dir}/valid-version.json"
        COPYONLY
    )
    add_test(
        NAME contract_probe.unicode_path
        COMMAND palmier_contract_probe --contract-version-file "${unicode_dir}/valid-version.json"
    )

    function(add_probe_failure name input expected_code expected_marker)
        add_test(
            NAME "contract_probe.${name}"
            COMMAND "${CMAKE_COMMAND}"
                "-DPROBE=$<TARGET_FILE:palmier_contract_probe>"
                "-DINPUT=${input}"
                "-DEXPECTED_CODE=${expected_code}"
                "-DEXPECTED_MARKER=${expected_marker}"
                -P "${expect_failure}"
        )
    endfunction()

    add_probe_failure(rejects_malformed_json "${test_data}/malformed.json" 4 PALMIER_CONTRACT_E_JSON)
    add_probe_failure(rejects_missing_version "${test_data}/missing-version.json" 5 PALMIER_CONTRACT_E_VERSION)
    add_probe_failure(rejects_wrong_version "${test_data}/wrong-version.json" 5 PALMIER_CONTRACT_E_VERSION)
    add_probe_failure(rejects_non_integer_version "${test_data}/non-integer-version.json" 5 PALMIER_CONTRACT_E_VERSION)
    add_probe_failure(rejects_duplicate_version "${test_data}/duplicate-version.json" 4 PALMIER_CONTRACT_E_JSON)

    configure_file(
        "${test_data}/wrong-version.json"
        "${unicode_dir}/wrong-version.json"
        COPYONLY
    )
    configure_file(
        "${test_data}/malformed.json"
        "${unicode_dir}/malformed.json"
        COPYONLY
    )
    add_probe_failure(unicode_wrong_version "${unicode_dir}/wrong-version.json" 5 PALMIER_CONTRACT_E_VERSION)
    add_probe_failure(unicode_malformed "${unicode_dir}/malformed.json" 4 PALMIER_CONTRACT_E_JSON)
    add_probe_failure(unicode_missing_file "${unicode_dir}/missing.json" 3 PALMIER_CONTRACT_E_IO)

    set(generated_data "${CMAKE_CURRENT_BINARY_DIR}/generated-test-data")
    file(MAKE_DIRECTORY "${generated_data}")
    string(ASCII 128 invalid_continuation)
    string(ASCII 192 128 overlong_sequence)
    string(ASCII 237 160 128 raw_surrogate)
    string(ASCII 244 144 128 128 above_unicode_range)
    file(WRITE "${generated_data}/invalid-continuation.json" "{\"name\":\"${invalid_continuation}\"}")
    file(WRITE "${generated_data}/overlong.json" "{\"name\":\"${overlong_sequence}\"}")
    file(WRITE "${generated_data}/raw-surrogate.json" "{\"name\":\"${raw_surrogate}\"}")
    file(WRITE "${generated_data}/above-unicode-range.json" "{\"name\":\"${above_unicode_range}\"}")
    add_probe_failure(rejects_invalid_continuation "${generated_data}/invalid-continuation.json" 4 PALMIER_CONTRACT_E_JSON)
    add_probe_failure(rejects_overlong_utf8 "${generated_data}/overlong.json" 4 PALMIER_CONTRACT_E_JSON)
    add_probe_failure(rejects_raw_surrogate "${generated_data}/raw-surrogate.json" 4 PALMIER_CONTRACT_E_JSON)
    add_probe_failure(rejects_above_unicode_range "${generated_data}/above-unicode-range.json" 4 PALMIER_CONTRACT_E_JSON)
endif()
