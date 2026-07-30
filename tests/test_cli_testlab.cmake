if(NOT DEFINED CLI OR NOT DEFINED TEST_ROOT)
    message(FATAL_ERROR "CLI and TEST_ROOT are required")
endif()

file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}")

execute_process(
    COMMAND "${CLI}" testlab generate --preset quick
            --output "${TEST_ROOT}" --estimate-only
    RESULT_VARIABLE estimate_result
    OUTPUT_VARIABLE estimate_output
    ERROR_VARIABLE estimate_error
)
if(NOT estimate_result EQUAL 0)
    message(FATAL_ERROR
        "Test Lab estimate-only failed: ${estimate_error}")
endif()
if(EXISTS "${TEST_ROOT}/youtube_test_lab")
    message(FATAL_ERROR
        "Estimate-only touched the suite output")
endif()
if(NOT estimate_output MATCHES "Cases: 6")
    message(FATAL_ERROR
        "Quick preset estimate did not report six cases")
endif()
if(NOT estimate_output MATCHES "Minimum per candidate: 2")
    message(FATAL_ERROR
        "Quick preset did not report the 2-second minimum")
endif()

foreach(BAD_ARGS
        "generate;--preset;invalid;--output;${TEST_ROOT};--estimate-only"
        "generate;--preset;quick;--output;${TEST_ROOT};--resolution;1919x1080;--estimate-only"
        "generate;--preset;quick;--output;${TEST_ROOT};--minimum-upload-duration;1.99;--estimate-only"
        "generate;--preset;quick;--output;${TEST_ROOT};--mode;fast-local;--estimate-only"
        "analyze-folder;--suite;missing.json"
        "analyze-folder;--suite;missing.json;--folder;${TEST_ROOT};--map;invalid"
        "deduplicate;--suite;missing.json;--dry-run;--apply")
    execute_process(
        COMMAND "${CLI}" testlab ${BAD_ARGS}
        RESULT_VARIABLE bad_result
        OUTPUT_QUIET
        ERROR_QUIET
    )
    if(bad_result EQUAL 0)
        message(FATAL_ERROR
            "Invalid Test Lab command unexpectedly succeeded: ${BAD_ARGS}")
    endif()
endforeach()
