if(NOT DEFINED CLI OR NOT DEFINED TEST_ROOT)
    message(FATAL_ERROR "CLI and TEST_ROOT are required")
endif()

file(MAKE_DIRECTORY "${TEST_ROOT}")

execute_process(
    COMMAND "${CLI}" capacitylab estimate
            --preset smoke
            --output "${TEST_ROOT}"
            --max-disk-gib 20
    RESULT_VARIABLE estimate_result
    OUTPUT_VARIABLE estimate_output
    ERROR_VARIABLE estimate_error
)
if(NOT estimate_result EQUAL 0)
    message(FATAL_ERROR
        "Capacity estimate failed: ${estimate_error}")
endif()
if(NOT estimate_output MATCHES "Raw combinations: 192")
    message(FATAL_ERROR
        "Capacity estimate did not report the bounded raw matrix")
endif()
if(NOT estimate_output MATCHES "Staged maximum cases: 12")
    message(FATAL_ERROR
        "Capacity smoke did not report 12 cases")
endif()

execute_process(
    COMMAND "${CLI}" capacitylab estimate
            --preset custom
            --output "${TEST_ROOT}"
            --block-size 8,6,4
            --bits-per-block 1,2
            --signal 0.75,1.0,1.25,1.5
            --repair-percent 0,1,2,5
            --resolution 1080p,2160p
            --max-cases 64
    RESULT_VARIABLE full_result
)
if(full_result EQUAL 0)
    message(FATAL_ERROR
        "Full 192-case matrix ran without an explicit 192-case limit")
endif()

set(validate_root "${TEST_ROOT}/validate-read-only")
file(MAKE_DIRECTORY "${validate_root}")
set(validate_manifest "${validate_root}/manifest.json")
file(WRITE "${validate_manifest}"
    "{\n"
    "  \"schema_version\": 4,\n"
    "  \"manifest_type\": \"youtube-capacity-lab\",\n"
    "  \"experiment_id\": \"CLI-VALIDATE\",\n"
    "  \"created_at\": \"2026-07-30T00:00:00Z\",\n"
    "  \"vidstorex_version\": \"1.4.0\",\n"
    "  \"preset\": \"smoke\",\n"
    "  \"maximum_cases\": 64,\n"
    "  \"maximum_shortlist_videos\": 8,\n"
    "  \"maximum_disk_bytes\": 21474836480,\n"
    "  \"cancelled\": false,\n"
    "  \"cases\": []\n"
    "}\n")
file(SHA256 "${validate_manifest}" validate_hash_before)
execute_process(
    COMMAND "${CLI}" capacitylab validate
            --manifest "${validate_manifest}"
    RESULT_VARIABLE validate_result
    OUTPUT_VARIABLE validate_output
    ERROR_VARIABLE validate_error
)
if(NOT validate_result EQUAL 0)
    message(FATAL_ERROR
        "Read-only Capacity validate failed: ${validate_error}")
endif()
if(NOT validate_output MATCHES "errors=0")
    message(FATAL_ERROR
        "Capacity validate did not report zero errors")
endif()
file(SHA256 "${validate_manifest}" validate_hash_after)
if(NOT validate_hash_before STREQUAL validate_hash_after)
    message(FATAL_ERROR
        "Read-only Capacity validate modified the manifest")
endif()
