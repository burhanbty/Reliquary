# A previous GUI/codec run or an external scanner may still hold the reused
# staging directory on Windows. Keep each synthetic run in a fresh, short
# sibling, preserving failed-run evidence instead of deleting and reusing it.
get_filename_component(TEST_PARENT "${TEST_ROOT}" DIRECTORY)
string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef RUN_ID)
set(TEST_ROOT "${TEST_PARENT}/instant-${RUN_ID}")
if(EXISTS "${TEST_ROOT}")
    message(FATAL_ERROR "Instant fixture directory collision: ${TEST_ROOT}")
endif()
message(STATUS "Instant Recovery synthetic run: ${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}" "${TEST_ROOT}/sets")
string(REPEAT "I" 1048576 SOURCE_PAYLOAD)
file(WRITE "${TEST_ROOT}/source.bin" "${SOURCE_PAYLOAD}")
unset(SOURCE_PAYLOAD)

execute_process(
    COMMAND "${CLI}" set-encode "${TEST_ROOT}/source.bin" "${TEST_ROOT}/sets"
            --reliability-profile high-capacity
            --target-duration-seconds 1
            --max-video-size-mib 0
    RESULT_VARIABLE ENCODE_RESULT
    OUTPUT_VARIABLE ENCODE_OUTPUT
    ERROR_VARIABLE ENCODE_ERROR
    TIMEOUT 120)
if (NOT ENCODE_RESULT EQUAL 0)
    message(FATAL_ERROR "Instant fixture encode failed: ${ENCODE_RESULT}\n${ENCODE_OUTPUT}\n${ENCODE_ERROR}")
endif ()
file(GLOB SET_DIRS LIST_DIRECTORIES true "${TEST_ROOT}/sets/*")
list(LENGTH SET_DIRS SET_COUNT)
if (NOT SET_COUNT EQUAL 1)
    message(FATAL_ERROR "Instant fixture expected one set, got ${SET_COUNT}")
endif ()
list(GET SET_DIRS 0 SET_ROOT)
set(VIDEOS "${SET_ROOT}/videos")

execute_process(
    COMMAND "${GUI}"
            --instant-recovery-smoke-root "${TEST_ROOT}"
            --instant-recovery-fake-ytdlp "${FAKE_YTDLP}"
            --instant-recovery-fixture-videos "${VIDEOS}"
    RESULT_VARIABLE GUI_RESULT
    OUTPUT_VARIABLE GUI_OUTPUT
    ERROR_VARIABLE GUI_ERROR
    TIMEOUT 150)
if (NOT GUI_RESULT EQUAL 0)
    message(FATAL_ERROR "Instant Recovery GUI E2E failed: ${GUI_RESULT}\n${GUI_OUTPUT}\n${GUI_ERROR}")
endif ()
if (NOT EXISTS "${TEST_ROOT}/recovered-instant/source.bin")
    message(FATAL_ERROR "Instant Recovery output is missing")
endif ()
file(SHA256 "${TEST_ROOT}/source.bin" SOURCE_SHA)
file(SHA256 "${TEST_ROOT}/recovered-instant/source.bin" OUTPUT_SHA)
if (NOT SOURCE_SHA STREQUAL OUTPUT_SHA)
    message(FATAL_ERROR "Instant Recovery SHA mismatch")
endif ()
