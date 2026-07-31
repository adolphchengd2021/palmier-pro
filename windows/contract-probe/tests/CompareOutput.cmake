execute_process(
    COMMAND "${PROBE}" "${MODE}" "${INPUT}"
    RESULT_VARIABLE exit_code
    OUTPUT_VARIABLE actual
    ERROR_VARIABLE standard_error
)

if(NOT exit_code EQUAL 0)
    message(FATAL_ERROR "probe failed with ${exit_code}: ${standard_error}")
endif()

file(READ "${EXPECTED}" expected)
string(STRIP "${actual}" actual)
string(STRIP "${expected}" expected)
if(NOT actual STREQUAL expected)
    message(FATAL_ERROR "normalized output drifted\nexpected=${expected}\nactual=${actual}")
endif()
