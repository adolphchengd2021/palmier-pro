set(probe_include_dir "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(palmier_contract_probe_core STATIC
    src/repository_contract_probe.cpp
)
target_include_directories(palmier_contract_probe_core PUBLIC "${probe_include_dir}")
target_compile_features(palmier_contract_probe_core PUBLIC cxx_std_20)
target_link_libraries(palmier_contract_probe_core PUBLIC palmier_project_reader)

add_executable(palmier_contract_probe app/main.cpp)
target_link_libraries(palmier_contract_probe PRIVATE palmier_contract_probe_core)

foreach(target palmier_contract_probe_core palmier_contract_probe)
    target_compile_options(${target} PRIVATE /W4 /WX /permissive- /Zc:__cplusplus /utf-8)
endforeach()

if(BUILD_TESTING)
    find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
    set(test_data "${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
    set(expected_data "${CMAKE_CURRENT_SOURCE_DIR}/tests/expected")
    set(expect_failure "${CMAKE_CURRENT_SOURCE_DIR}/tests/ExpectFailure.cmake")
    set(compare_output "${CMAKE_CURRENT_SOURCE_DIR}/tests/CompareOutput.cmake")

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

    function(add_project_output_test name fixture expected)
        add_test(
            NAME "project_reader.${name}"
            COMMAND "${CMAKE_COMMAND}"
                "-DPROBE=$<TARGET_FILE:palmier_contract_probe>"
                "-DMODE=--canonical-project-source"
                "-DINPUT=${PROJECT_SOURCE_DIR}/fixtures/contracts/projects/${fixture}/project.json"
                "-DEXPECTED=${expected_data}/${expected}"
                -P "${compare_output}"
        )
    endfunction()

    add_project_output_test(current_source current-multitimeline.palmier current-source.json)
    add_project_output_test(legacy_source legacy-bare-timeline.palmier legacy-source.json)
    add_project_output_test(unknown_source unknown-fields.palmier unknown-source.json)

    function(add_project_failure name input expected_marker)
        add_test(
            NAME "project_reader.${name}"
            COMMAND "${CMAKE_COMMAND}"
                "-DPROBE=$<TARGET_FILE:palmier_contract_probe>"
                "-DMODE=--normalize-project-file"
                "-DINPUT=${input}"
                "-DEXPECTED_CODE=6"
                "-DEXPECTED_MARKER=${expected_marker}"
                -P "${expect_failure}"
        )
    endfunction()

    add_project_failure(
        rejects_empty_timelines
        "${test_data}/project-empty-timelines.json"
        emptyTimelines
    )
    add_project_failure(
        current_root_does_not_fallback
        "${test_data}/project-current-wrong-type.json"
        wrongRequiredType
    )
    add_project_failure(
        rejects_required_integer_string
        "${test_data}/project-fps-string.json"
        wrongRequiredType
    )
    add_project_failure(
        rejects_required_integer_fraction
        "${test_data}/project-fps-fraction.json"
        wrongRequiredType
    )
    add_project_failure(
        rejects_required_integer_overflow
        "${test_data}/project-fps-overflow.json"
        integerOutOfRange
    )
    add_project_failure(
        rejects_nonpositive_timeline_dimension
        "${test_data}/project-fps-zero.json"
        invalidRequiredValue
    )

    add_test(
        NAME project_reader.rejects_malformed_json
        COMMAND "${CMAKE_COMMAND}"
            "-DPROBE=$<TARGET_FILE:palmier_contract_probe>"
            "-DMODE=--normalize-project-file"
            "-DINPUT=${test_data}/malformed.json"
            "-DEXPECTED_CODE=4"
            "-DEXPECTED_MARKER=PALMIER_CONTRACT_E_JSON"
            -P "${expect_failure}"
    )
    add_test(
        NAME project_reader.rejects_missing_file
        COMMAND "${CMAKE_COMMAND}"
            "-DPROBE=$<TARGET_FILE:palmier_contract_probe>"
            "-DMODE=--normalize-project-file"
            "-DINPUT=${generated_data}/missing-project.json"
            "-DEXPECTED_CODE=3"
            "-DEXPECTED_MARKER=PALMIER_CONTRACT_E_IO"
            -P "${expect_failure}"
    )

    function(add_project_differential name input)
        add_test(
            NAME "project_reader.${name}_differential"
            COMMAND "${Python3_EXECUTABLE}" -B
                "${PROJECT_SOURCE_DIR}/tools/project_reader_oracle.py"
                "--probe=$<TARGET_FILE:palmier_contract_probe>"
                "--input=${input}"
        )
    endfunction()

    function(add_project_error_differential name input expected_error)
        add_test(
            NAME "project_reader.${name}_error_differential"
            COMMAND "${Python3_EXECUTABLE}" -B
                "${PROJECT_SOURCE_DIR}/tools/project_reader_oracle.py"
                "--probe=$<TARGET_FILE:palmier_contract_probe>"
                "--input=${input}"
                "--expected-error=${expected_error}"
        )
    endfunction()

    add_project_error_differential(
        empty_timelines
        "${test_data}/project-empty-timelines.json"
        emptyTimelines
    )
    add_project_error_differential(
        current_wrong_type
        "${test_data}/project-current-wrong-type.json"
        wrongRequiredType
    )
    add_project_error_differential(
        required_integer_string
        "${test_data}/project-fps-string.json"
        wrongRequiredType
    )
    add_project_error_differential(
        required_integer_boolean
        "${test_data}/project-fps-boolean.json"
        wrongRequiredType
    )
    add_project_error_differential(
        required_integer_fraction
        "${test_data}/project-fps-fraction.json"
        wrongRequiredType
    )
    add_project_error_differential(
        required_integer_overflow
        "${test_data}/project-fps-overflow.json"
        integerOutOfRange
    )
    add_project_error_differential(
        nonpositive_timeline_dimension
        "${test_data}/project-fps-zero.json"
        invalidRequiredValue
    )
    add_project_error_differential(
        missing_tracks
        "${test_data}/project-missing-tracks.json"
        missingRequiredField
    )
    add_project_error_differential(
        tracks_wrong_type
        "${test_data}/project-tracks-wrong-type.json"
        wrongRequiredType
    )

    add_project_differential(
        current
        "${PROJECT_SOURCE_DIR}/fixtures/contracts/projects/current-multitimeline.palmier/project.json"
    )
    add_project_differential(
        legacy
        "${PROJECT_SOURCE_DIR}/fixtures/contracts/projects/legacy-bare-timeline.palmier/project.json"
    )
    add_project_differential(
        unknown_fields
        "${PROJECT_SOURCE_DIR}/fixtures/contracts/projects/unknown-fields.palmier/project.json"
    )
    add_project_differential(
        missing_ids
        "${test_data}/project-missing-ids.json"
    )
    add_project_differential(
        diagnostics
        "${test_data}/project-diagnostics.json"
    )
    add_project_differential(
        number_boundaries
        "${test_data}/project-number-boundaries.json"
    )

    set(unicode_project_dir "${CMAKE_CURRENT_BINARY_DIR}/project-素材")
    file(MAKE_DIRECTORY "${unicode_project_dir}")
    configure_file(
        "${PROJECT_SOURCE_DIR}/fixtures/contracts/projects/current-multitimeline.palmier/project.json"
        "${unicode_project_dir}/project.json"
        COPYONLY
    )
    add_project_differential(unicode_path "${unicode_project_dir}/project.json")
endif()
