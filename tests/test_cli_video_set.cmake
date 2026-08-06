file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}")
file(WRITE "${TEST_ROOT}/tiny.bin" "VidStoreX deterministic Video Set CLI test payload")

execute_process(
    COMMAND "${CLI}" set-help
    RESULT_VARIABLE HELP_RESULT
    OUTPUT_VARIABLE HELP_OUTPUT
    ERROR_VARIABLE HELP_ERROR)
if (NOT HELP_RESULT EQUAL 0 OR NOT HELP_OUTPUT MATCHES "set-plan" OR
    NOT HELP_OUTPUT MATCHES "Resilient remains the default" OR
    NOT HELP_OUTPUT MATCHES "streaming is unsupported" OR
    NOT HELP_OUTPUT MATCHES "real YouTube four-part roundtrip" OR
    NOT HELP_OUTPUT MATCHES "exact full-file SHA-256 recovery" OR
    NOT HELP_OUTPUT MATCHES "Always verify successful recovery")
    message(FATAL_ERROR "Video Set help failed: ${HELP_RESULT}\n${HELP_OUTPUT}\n${HELP_ERROR}")
endif ()

execute_process(
    COMMAND "${CLI}" set-plan "${TEST_ROOT}/tiny.bin" "${TEST_ROOT}/sets"
        --reliability-profile high-capacity
        --target-duration-seconds 2
        --max-video-size-mib 0
    RESULT_VARIABLE PLAN_RESULT
    OUTPUT_VARIABLE PLAN_OUTPUT
    ERROR_VARIABLE PLAN_ERROR)
if (NOT PLAN_RESULT EQUAL 0 OR NOT PLAN_OUTPUT MATCHES "Video Set plan" OR
    NOT PLAN_OUTPUT MATCHES "538F2B009FAB" OR NOT PLAN_OUTPUT MATCHES "Parts: 1")
    message(FATAL_ERROR "Video Set plan failed: ${PLAN_RESULT}\n${PLAN_OUTPUT}\n${PLAN_ERROR}")
endif ()

execute_process(
    COMMAND "${CLI}" set-plan "${TEST_ROOT}/tiny.bin" "${TEST_ROOT}/sets" --unknown-option
    RESULT_VARIABLE UNKNOWN_RESULT
    OUTPUT_VARIABLE UNKNOWN_OUTPUT
    ERROR_VARIABLE UNKNOWN_ERROR)
if (UNKNOWN_RESULT EQUAL 0 OR NOT UNKNOWN_ERROR MATCHES "unknown Video Set option")
    message(FATAL_ERROR "Unknown Video Set option was not rejected")
endif ()

execute_process(
    COMMAND "${CLI}" set-plan "${TEST_ROOT}/tiny.bin" "${TEST_ROOT}/sets"
        --reliability-profile imaginary
    RESULT_VARIABLE PROFILE_RESULT
    OUTPUT_VARIABLE PROFILE_OUTPUT
    ERROR_VARIABLE PROFILE_ERROR)
if (PROFILE_RESULT EQUAL 0 OR NOT PROFILE_ERROR MATCHES "reliability profile")
    message(FATAL_ERROR "Unknown profile was not rejected")
endif ()

# Real file -> 3+ independently encoded videos -> renamed/reversed,
# sidecar-free scan and exact recovery.
string(REPEAT "V" 1048576 E2E_PAYLOAD)
file(WRITE "${TEST_ROOT}/e2e-source.bin" "${E2E_PAYLOAD}")
unset(E2E_PAYLOAD)
execute_process(
    COMMAND "${CLI}" set-encode "${TEST_ROOT}/e2e-source.bin" "${TEST_ROOT}/sets"
        --reliability-profile high-capacity
        --target-duration-seconds 1
        --max-video-size-mib 0
    RESULT_VARIABLE ENCODE_RESULT
    OUTPUT_VARIABLE ENCODE_OUTPUT
    ERROR_VARIABLE ENCODE_ERROR)
if (NOT ENCODE_RESULT EQUAL 0 OR NOT ENCODE_OUTPUT MATCHES "locally verified")
    message(FATAL_ERROR "Video Set E2E encode failed: ${ENCODE_RESULT}\n${ENCODE_OUTPUT}\n${ENCODE_ERROR}")
endif ()
file(GLOB SET_MANIFESTS "${TEST_ROOT}/sets/*/set_manifest.json")
list(LENGTH SET_MANIFESTS MANIFEST_COUNT)
if (NOT MANIFEST_COUNT EQUAL 1)
    message(FATAL_ERROR "Expected one atomically published Video Set")
endif ()
list(GET SET_MANIFESTS 0 SET_MANIFEST)
get_filename_component(SET_ROOT "${SET_MANIFEST}" DIRECTORY)
file(GLOB VIDEOS "${SET_ROOT}/videos/*.mkv")
list(LENGTH VIDEOS VIDEO_COUNT)
if (VIDEO_COUNT LESS 3)
    message(FATAL_ERROR "Expected at least three Video Set videos, got ${VIDEO_COUNT}")
endif ()

file(MAKE_DIRECTORY "${TEST_ROOT}/returned-renamed")
list(REVERSE VIDEOS)
set(RENAME_INDEX 0)
foreach (VIDEO IN LISTS VIDEOS)
    math(EXPR RENAME_INDEX "${RENAME_INDEX} + 1")
    file(COPY_FILE "${VIDEO}"
        "${TEST_ROOT}/returned-renamed/Returned shuffled clip ${RENAME_INDEX}.mkv")
endforeach ()
execute_process(
    COMMAND "${CLI}" set-inspect "${TEST_ROOT}/returned-renamed"
    RESULT_VARIABLE INSPECT_RESULT
    OUTPUT_VARIABLE INSPECT_OUTPUT
    ERROR_VARIABLE INSPECT_ERROR)
if (NOT INSPECT_RESULT EQUAL 0 OR NOT INSPECT_OUTPUT MATCHES "available ${VIDEO_COUNT}/${VIDEO_COUNT} parts" OR
    NOT INSPECT_OUTPUT MATCHES "conflicts 0" OR NOT INSPECT_OUTPUT MATCHES "missing 0")
    message(FATAL_ERROR "Renamed sidecar-free scan failed: ${INSPECT_RESULT}\n${INSPECT_OUTPUT}\n${INSPECT_ERROR}")
endif ()
execute_process(
    COMMAND "${CLI}" set-recover "${TEST_ROOT}/returned-renamed" "${TEST_ROOT}/recovered"
    RESULT_VARIABLE RECOVER_RESULT
    OUTPUT_VARIABLE RECOVER_OUTPUT
    ERROR_VARIABLE RECOVER_ERROR)
if (NOT RECOVER_RESULT EQUAL 0 OR NOT RECOVER_OUTPUT MATCHES "Recovered exact" OR
    NOT EXISTS "${TEST_ROOT}/recovered/e2e-source.bin")
    message(FATAL_ERROR "Sidecar-free recovery failed: ${RECOVER_RESULT}\n${RECOVER_OUTPUT}\n${RECOVER_ERROR}")
endif ()
file(SHA256 "${TEST_ROOT}/e2e-source.bin" SOURCE_SHA)
file(SHA256 "${TEST_ROOT}/recovered/e2e-source.bin" RECOVERED_SHA)
if (NOT SOURCE_SHA STREQUAL RECOVERED_SHA)
    message(FATAL_ERROR "Recovered Video Set SHA-256 does not match source")
endif ()

list(GET VIDEOS 0 FIRST_VIDEO)
execute_process(
    COMMAND "${CLI}" decode --input "${FIRST_VIDEO}"
        --output "${TEST_ROOT}/must-not-remain.payload"
    RESULT_VARIABLE SINGLE_DECODE_RESULT
    OUTPUT_VARIABLE SINGLE_DECODE_OUTPUT
    ERROR_VARIABLE SINGLE_DECODE_ERROR)
if (NOT SINGLE_DECODE_RESULT EQUAL 3 OR
    EXISTS "${TEST_ROOT}/must-not-remain.payload" OR
    NOT SINGLE_DECODE_ERROR MATCHES "Video Set part")
    message(FATAL_ERROR "Normal decode did not safely route a Video Set part")
endif ()

execute_process(
    COMMAND "${CLI}" set-recover "${TEST_ROOT}/returned-renamed" "${TEST_ROOT}/recovered"
    RESULT_VARIABLE EXISTS_RESULT
    OUTPUT_VARIABLE EXISTS_OUTPUT
    ERROR_VARIABLE EXISTS_ERROR)
if (EXISTS_RESULT EQUAL 0 OR NOT EXISTS_ERROR MATCHES "output exists")
    message(FATAL_ERROR "Existing recovery output was overwritten without --overwrite")
endif ()
execute_process(
    COMMAND "${CLI}" set-recover "${TEST_ROOT}/returned-renamed" "${TEST_ROOT}/recovered"
        --overwrite
    RESULT_VARIABLE OVERWRITE_RESULT
    OUTPUT_VARIABLE OVERWRITE_OUTPUT
    ERROR_VARIABLE OVERWRITE_ERROR)
if (NOT OVERWRITE_RESULT EQUAL 0 OR NOT OVERWRITE_OUTPUT MATCHES "Recovered exact" OR
    EXISTS "${TEST_ROOT}/recovered/e2e-source.bin.vsx-replace-backup")
    message(FATAL_ERROR "Safe recovery overwrite failed: ${OVERWRITE_RESULT}\n${OVERWRITE_OUTPUT}\n${OVERWRITE_ERROR}")
endif ()

# An incomplete set must have its meaningful exit code and no final file.
file(MAKE_DIRECTORY "${TEST_ROOT}/returned-missing")
set(COPIED 0)
math(EXPR COPY_LIMIT "${VIDEO_COUNT} - 1")
foreach (VIDEO IN LISTS VIDEOS)
    if (COPIED LESS COPY_LIMIT)
        file(COPY "${VIDEO}" DESTINATION "${TEST_ROOT}/returned-missing")
        math(EXPR COPIED "${COPIED} + 1")
    endif ()
endforeach ()
execute_process(
    COMMAND "${CLI}" set-recover "${TEST_ROOT}/returned-missing" "${TEST_ROOT}/missing-output"
    RESULT_VARIABLE MISSING_RESULT
    OUTPUT_VARIABLE MISSING_OUTPUT
    ERROR_VARIABLE MISSING_ERROR)
if (NOT MISSING_RESULT EQUAL 3 OR NOT MISSING_OUTPUT MATCHES "Incomplete: missing parts" OR
    EXISTS "${TEST_ROOT}/missing-output/e2e-source.bin")
    message(FATAL_ERROR "Incomplete Video Set behavior failed: ${MISSING_RESULT}\n${MISSING_OUTPUT}\n${MISSING_ERROR}")
endif ()

# An identical duplicate is reported but remains recoverable.
file(COPY "${TEST_ROOT}/returned-renamed" DESTINATION "${TEST_ROOT}/duplicate-parent")
list(GET VIDEOS 0 DUPLICATE_VIDEO)
file(COPY_FILE "${DUPLICATE_VIDEO}"
    "${TEST_ROOT}/duplicate-parent/returned-renamed/identical duplicate.mkv")
execute_process(
    COMMAND "${CLI}" set-inspect "${TEST_ROOT}/duplicate-parent/returned-renamed"
    RESULT_VARIABLE DUPLICATE_INSPECT_RESULT
    OUTPUT_VARIABLE DUPLICATE_INSPECT_OUTPUT
    ERROR_VARIABLE DUPLICATE_INSPECT_ERROR)
if (NOT DUPLICATE_INSPECT_RESULT EQUAL 0 OR NOT DUPLICATE_INSPECT_OUTPUT MATCHES "duplicates 1")
    message(FATAL_ERROR "Identical duplicate was not reported")
endif ()
execute_process(
    COMMAND "${CLI}" set-recover "${TEST_ROOT}/duplicate-parent/returned-renamed"
        "${TEST_ROOT}/duplicate-output"
    RESULT_VARIABLE DUPLICATE_RECOVER_RESULT
    OUTPUT_VARIABLE DUPLICATE_RECOVER_OUTPUT
    ERROR_VARIABLE DUPLICATE_RECOVER_ERROR)
if (NOT DUPLICATE_RECOVER_RESULT EQUAL 0 OR
    NOT DUPLICATE_RECOVER_OUTPUT MATCHES "Recovered exact")
    message(FATAL_ERROR "Identical duplicate prevented exact recovery")
endif ()

execute_process(
    COMMAND "${CLI}" stream-encode --input "${TEST_ROOT}/tiny.bin"
        --url "rtmp://example.invalid/live" --video-set
    RESULT_VARIABLE STREAM_SET_RESULT
    OUTPUT_VARIABLE STREAM_SET_OUTPUT
    ERROR_VARIABLE STREAM_SET_ERROR)
if (NOT STREAM_SET_RESULT EQUAL 2 OR
    NOT STREAM_SET_ERROR MATCHES "unsupported for streams")
    message(FATAL_ERROR "Stream Video Set request did not return explicit unsupported error")
endif ()
