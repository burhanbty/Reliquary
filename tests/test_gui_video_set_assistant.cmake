file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}")
string(REPEAT "V" 1048576 SOURCE_PAYLOAD)
file(WRITE "${TEST_ROOT}/source.bin" "${SOURCE_PAYLOAD}")
unset(SOURCE_PAYLOAD)

execute_process(
    COMMAND "${GUI}" --video-set-assistant-smoke-root "${TEST_ROOT}"
    RESULT_VARIABLE GUI_RESULT
    OUTPUT_VARIABLE GUI_OUTPUT
    ERROR_VARIABLE GUI_ERROR
    TIMEOUT 120)
if (NOT GUI_RESULT EQUAL 0)
    message(FATAL_ERROR
        "Video Set Assistant E2E smoke failed: ${GUI_RESULT}\n${GUI_OUTPUT}\n${GUI_ERROR}")
endif ()

if (NOT EXISTS "${TEST_ROOT}/recovered/source.bin")
    message(FATAL_ERROR "Video Set Assistant did not publish recovered source.bin")
endif ()
file(SHA256 "${TEST_ROOT}/source.bin" SOURCE_SHA)
file(SHA256 "${TEST_ROOT}/recovered/source.bin" RECOVERED_SHA)
if (NOT SOURCE_SHA STREQUAL RECOVERED_SHA)
    message(FATAL_ERROR
        "Video Set Assistant final SHA mismatch: ${SOURCE_SHA} != ${RECOVERED_SHA}")
endif ()
message(STATUS "VIDEO_SET_ASSISTANT_SOURCE_SHA=${SOURCE_SHA}")
message(STATUS "VIDEO_SET_ASSISTANT_RECOVERED_SHA=${RECOVERED_SHA}")
