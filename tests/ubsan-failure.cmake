cmake_minimum_required(VERSION 3.20)

if(NOT EXISTS "${PROBE}" OR NOT IS_ABSOLUTE "${TEST_DIRECTORY}")
    message(FATAL_ERROR "Expected a compiled UBSan probe and an absolute test directory")
endif()
file(MAKE_DIRECTORY "${TEST_DIRECTORY}")
file(WRITE "${TEST_DIRECTORY}/CTestTestfile.cmake"
     "add_test(overflow \"${PROBE}\")\n")

# A caller's halt_on_error setting must not mask a missing no-recovery flag.
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env UBSAN_OPTIONS=halt_on_error=0
            "${CMAKE_CTEST_COMMAND}" --test-dir "${TEST_DIRECTORY}"
            --output-on-failure --no-tests=error --timeout 10
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE errors
    TIMEOUT 20)
if(NOT "${result}" STREQUAL "8" OR
   NOT "${output}${errors}" MATCHES "runtime error: signed integer overflow")
    message(FATAL_ERROR
        "CTest must fail and expose the UBSan diagnostic (exit ${result}).\n${output}${errors}")
endif()
message(STATUS "UBSan overflow fails CTest and remains visible with --output-on-failure")
