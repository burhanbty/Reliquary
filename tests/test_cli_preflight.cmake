if (NOT DEFINED CLI OR NOT EXISTS "${CLI}")
    message(FATAL_ERROR "CLI executable was not supplied")
endif ()
if (NOT DEFINED TEST_ROOT)
    message(FATAL_ERROR "TEST_ROOT was not supplied")
endif ()

file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}")
set(INPUT "${TEST_ROOT}/input.bin")
file(WRITE "${INPUT}" "VidStoreX CLI preflight integration test data")

function(run_cli RESULT_VAR STDOUT_VAR STDERR_VAR)
    execute_process(
            COMMAND "${CLI}" ${ARGN}
            RESULT_VARIABLE RESULT
            OUTPUT_VARIABLE STDOUT
            ERROR_VARIABLE STDERR
    )
    set(${RESULT_VAR} "${RESULT}" PARENT_SCOPE)
    set(${STDOUT_VAR} "${STDOUT}" PARENT_SCOPE)
    set(${STDERR_VAR} "${STDERR}" PARENT_SCOPE)
endfunction()

function(require_success RESULT STDERR CONTEXT)
    if (NOT "${RESULT}" STREQUAL "0")
        message(FATAL_ERROR "${CONTEXT} failed (${RESULT}): ${STDERR}")
    endif ()
endfunction()

function(require_failure RESULT CONTEXT)
    if ("${RESULT}" STREQUAL "0")
        message(FATAL_ERROR "${CONTEXT} unexpectedly succeeded")
    endif ()
endfunction()

# Estimate-only must neither create a target nor alter an existing target.
set(ESTIMATE_TARGET "${TEST_ROOT}/estimate_target.mkv")
run_cli(RESULT STDOUT STDERR encode --input "${INPUT}" --output
        "${ESTIMATE_TARGET}" --estimate-only --no-probe)
require_success("${RESULT}" "${STDERR}" "estimate-only")
if (EXISTS "${ESTIMATE_TARGET}")
    message(FATAL_ERROR "estimate-only created the target")
endif ()
file(WRITE "${ESTIMATE_TARGET}" "existing-target-sentinel")
run_cli(RESULT STDOUT STDERR encode --estimate-only --output
        "${ESTIMATE_TARGET}" --input "${INPUT}" --no-probe)
require_success("${RESULT}" "${STDERR}" "reordered estimate-only")
file(READ "${ESTIMATE_TARGET}" TARGET_CONTENT)
if (NOT TARGET_CONTENT STREQUAL "existing-target-sentinel")
    message(FATAL_ERROR "estimate-only altered the existing target")
endif ()

# Both estimate JSON spellings must produce the same estimate schema.
foreach(JSON_OPTION IN ITEMS --estimate-json --benchmark-json)
    string(REPLACE "--" "" JSON_STEM "${JSON_OPTION}")
    set(JSON_PATH "${TEST_ROOT}/${JSON_STEM}.json")
    run_cli(RESULT STDOUT STDERR encode --input "${INPUT}" --output
            "${TEST_ROOT}/${JSON_STEM}.mkv" --estimate-only --no-probe
            "${JSON_OPTION}" "${JSON_PATH}")
    require_success("${RESULT}" "${STDERR}" "${JSON_OPTION} alias")
    if (NOT EXISTS "${JSON_PATH}")
        message(FATAL_ERROR "${JSON_OPTION} did not create JSON")
    endif ()
    file(READ "${JSON_PATH}" JSON_TEXT)
    foreach(REQUIRED_JSON_FIELD IN ITEMS input_size_bytes
            output_size_estimate_available disk_space_known
            can_start_encoding estimation_method)
        if (NOT JSON_TEXT MATCHES "\"${REQUIRED_JSON_FIELD}\"")
            message(FATAL_ERROR
                    "${JSON_OPTION} omitted ${REQUIRED_JSON_FIELD}")
        endif ()
    endforeach()
    if (NOT JSON_TEXT MATCHES
            "\"output_size_estimate_available\"[ \t]*:[ \t]*false")
        message(FATAL_ERROR "--no-probe did not mark output unavailable")
    endif ()
    if (NOT JSON_TEXT MATCHES
            "\"estimated_output_bytes\"[ \t]*:[ \t]*null")
        message(FATAL_ERROR "--no-probe emitted a numeric output estimate")
    endif ()
endforeach()

# A known one-byte disk budget must block by default and print all details.
set(LOW_DISK_TARGET "${TEST_ROOT}/low_disk.mkv")
execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env
        "VIDSTOREX_TEST_AVAILABLE_DISK_BYTES=1"
        "${CLI}" encode --input "${INPUT}" --output "${LOW_DISK_TARGET}"
        RESULT_VARIABLE RESULT OUTPUT_VARIABLE STDOUT ERROR_VARIABLE STDERR
)
require_failure("${RESULT}" "known low disk")
foreach(REQUIRED_TEXT IN ITEMS "Available space" "Required space"
        "Missing space" "Estimated output maximum" "Safety margin")
    if (NOT STDERR MATCHES "${REQUIRED_TEXT}")
        message(FATAL_ERROR "low-disk error omitted '${REQUIRED_TEXT}'")
    endif ()
endforeach()
if (EXISTS "${LOW_DISK_TARGET}")
    message(FATAL_ERROR "blocked low-disk encode created output")
endif ()

# The override permits only the disk blocker and emits an explicit warning.
execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env
        "VIDSTOREX_TEST_AVAILABLE_DISK_BYTES=1"
        "${CLI}" encode --input "${INPUT}" --output "${LOW_DISK_TARGET}"
        --allow-low-disk
        RESULT_VARIABLE RESULT OUTPUT_VARIABLE STDOUT ERROR_VARIABLE STDERR
)
require_success("${RESULT}" "${STDERR}" "low-disk override")
if (NOT STDERR MATCHES "consciously requested")
    message(FATAL_ERROR "low-disk override warning was not explicit")
endif ()
if (NOT EXISTS "${LOW_DISK_TARGET}")
    message(FATAL_ERROR "low-disk override did not encode")
endif ()
run_cli(RESULT STDOUT STDERR encode --input "${INPUT}" --output "${INPUT}"
        --allow-low-disk)
require_failure("${RESULT}" "input=output with low-disk override")

# Unknown disk space warns but does not require an override.
set(UNKNOWN_TARGET "${TEST_ROOT}/unknown_disk.mkv")
execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env
        "VIDSTOREX_TEST_DISK_UNKNOWN=1"
        "${CLI}" encode --input "${INPUT}" --output "${UNKNOWN_TARGET}"
        RESULT_VARIABLE RESULT OUTPUT_VARIABLE STDOUT ERROR_VARIABLE STDERR
)
require_success("${RESULT}" "${STDERR}" "unknown disk")
if (NOT "${STDOUT}${STDERR}" MATCHES
        "available disk space could not be determined")
    message(FATAL_ERROR "unknown-disk warning was not emitted")
endif ()

# Unknown options must remain a clear parsing error.
run_cli(RESULT STDOUT STDERR encode --input "${INPUT}" --output
        "${TEST_ROOT}/invalid.mkv" --not-a-real-option)
require_failure("${RESULT}" "invalid option")
if (NOT STDERR MATCHES "unknown or incomplete argument")
    message(FATAL_ERROR "invalid option error was not clear")
endif ()

file(REMOVE_RECURSE "${TEST_ROOT}")
if (EXISTS "${TEST_ROOT}")
    message(FATAL_ERROR "CLI integration temporary files were not cleaned")
endif ()
