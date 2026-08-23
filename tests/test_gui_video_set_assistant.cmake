file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}")
string(REPEAT "V" 1048576 SOURCE_PAYLOAD)
file(WRITE "${TEST_ROOT}/source.bin" "${SOURCE_PAYLOAD}")
unset(SOURCE_PAYLOAD)

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env "QT_SCALE_FACTOR=1.0"
            "${GUI}" --video-set-assistant-smoke-root "${TEST_ROOT}"
            --video-set-assistant-fake-ytdlp "${FAKE_YTDLP}"
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

foreach(SCALE IN ITEMS 1.25 1.5)
    string(REPLACE "." "_" SCALE_DIR "${SCALE}")
    set(RUN_ROOT "${TEST_ROOT}/layout_${SCALE_DIR}")
    file(MAKE_DIRECTORY "${RUN_ROOT}")
    string(REPEAT "V" 1048576 SOURCE_PAYLOAD)
    file(WRITE "${RUN_ROOT}/source.bin" "${SOURCE_PAYLOAD}")
    unset(SOURCE_PAYLOAD)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env
                "QT_SCALE_FACTOR=${SCALE}"
                "VIDSTOREX_WORKFLOW_LAYOUT_ONLY=1"
                "${GUI}" --video-set-assistant-smoke-root "${RUN_ROOT}"
                --video-set-assistant-fake-ytdlp "${FAKE_YTDLP}"
        RESULT_VARIABLE LAYOUT_RESULT
        OUTPUT_VARIABLE LAYOUT_OUTPUT
        ERROR_VARIABLE LAYOUT_ERROR
        TIMEOUT 120)
    if (NOT LAYOUT_RESULT EQUAL 0)
        message(FATAL_ERROR
            "Workflow layout qwindows audit failed at ${SCALE}: ${LAYOUT_RESULT}\n${LAYOUT_OUTPUT}\n${LAYOUT_ERROR}")
    endif ()
    foreach(SCREENSHOT IN ITEMS
            e2e-create-step1-1280x720.png
            e2e-create-step2-1366x768.png
            e2e-create-step3-1280x720.png
            e2e-create-step3-1366x768.png
            e2e-recover-initial-1280x720.png
            e2e-recover-initial-1366x768.png)
        if (NOT EXISTS "${RUN_ROOT}/${SCREENSHOT}")
            message(FATAL_ERROR
                "Workflow screenshot missing at ${SCALE}: ${SCREENSHOT}")
        endif ()
    endforeach()
endforeach()

message(STATUS "VIDEO_SET_ASSISTANT_SOURCE_SHA=${SOURCE_SHA}")
message(STATUS "VIDEO_SET_ASSISTANT_RECOVERED_SHA=${RECOVERED_SHA}")
message(STATUS "Workflow qwindows layout passed at 100%, 125%, and 150% DPI")
