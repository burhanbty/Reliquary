file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}" "${TEST_ROOT}/sets")
string(REPEAT "Y" 1048576 SOURCE_PAYLOAD)
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
    message(FATAL_ERROR
        "YouTube Sync fixture encode failed: ${ENCODE_RESULT}\n${ENCODE_OUTPUT}\n${ENCODE_ERROR}")
endif ()
file(GLOB MANIFESTS "${TEST_ROOT}/sets/*/set_manifest.json")
list(LENGTH MANIFESTS MANIFEST_COUNT)
if (NOT MANIFEST_COUNT EQUAL 1)
    message(FATAL_ERROR
        "YouTube Sync fixture expected one manifest, got ${MANIFEST_COUNT}")
endif ()
list(GET MANIFESTS 0 MANIFEST)

execute_process(
    COMMAND "${GUI}"
            --youtube-sync-smoke-root "${TEST_ROOT}"
            --youtube-sync-manifest "${MANIFEST}"
    RESULT_VARIABLE GUI_RESULT
    OUTPUT_VARIABLE GUI_OUTPUT
    ERROR_VARIABLE GUI_ERROR
    TIMEOUT 150)
if (NOT GUI_RESULT EQUAL 0)
    message(FATAL_ERROR
        "YouTube Sync GUI E2E failed: ${GUI_RESULT}\n${GUI_OUTPUT}\n${GUI_ERROR}")
endif ()
get_filename_component(SET_ROOT "${MANIFEST}" DIRECTORY)
if (NOT EXISTS "${SET_ROOT}/youtube_sync_state.json")
    message(FATAL_ERROR "YouTube Sync sidecar was not persisted")
endif ()
if (NOT EXISTS "${TEST_ROOT}/youtube-sync-ready.png")
    message(FATAL_ERROR "YouTube Sync ready-state visual was not captured")
endif ()
