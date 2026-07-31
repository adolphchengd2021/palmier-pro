execute_process(
    COMMAND "${PROBE}" --contract-version-file "${INPUT}"
    RESULT_VARIABLE actual_code
    OUTPUT_VARIABLE standard_output
    ERROR_VARIABLE standard_error
)

if(NOT actual_code EQUAL EXPECTED_CODE)
    message(FATAL_ERROR "expected exit ${EXPECTED_CODE}, got ${actual_code}\nstdout=${standard_output}\nstderr=${standard_error}")
endif()

string(CONCAT output "${standard_output}" "${standard_error}")
string(FIND "${output}" "${EXPECTED_MARKER}" marker_position)
if(marker_position EQUAL -1)
    message(FATAL_ERROR "expected marker ${EXPECTED_MARKER}\noutput=${output}")
endif()
