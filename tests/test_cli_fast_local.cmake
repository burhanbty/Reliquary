if(NOT DEFINED CLI OR NOT DEFINED TEST_ROOT)
    message(FATAL_ERROR "CLI and TEST_ROOT are required")
endif()

file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}")
file(WRITE "${TEST_ROOT}/input.bin" "Fast Local CLI roundtrip")

execute_process(
    COMMAND "${CLI}" encode
            --input "${TEST_ROOT}/input.bin"
            --output "${TEST_ROOT}/estimate.mkv"
            --mode fast-local
            --estimate-only
            --no-probe
            --estimate-json "${TEST_ROOT}/estimate.json"
    RESULT_VARIABLE estimate_status)
if(NOT estimate_status EQUAL 0 OR EXISTS "${TEST_ROOT}/estimate.mkv")
    message(FATAL_ERROR "Fast Local estimate-only behavior failed")
endif()
file(READ "${TEST_ROOT}/estimate.json" estimate_json)
if(NOT estimate_json MATCHES "\"encoding_mode\": \"fast-local\"")
    message(FATAL_ERROR "Fast Local estimate JSON omitted its mode")
endif()

execute_process(
    COMMAND "${CLI}" encode
            --input "${TEST_ROOT}/input.bin"
            --output "${TEST_ROOT}/invalid.mkv"
            --mode invalid
    RESULT_VARIABLE invalid_mode_status)
if(invalid_mode_status EQUAL 0)
    message(FATAL_ERROR "Invalid Fast Local mode was accepted")
endif()

execute_process(
    COMMAND "${CLI}" encode
            --input "${TEST_ROOT}/input.bin"
            --output "${TEST_ROOT}/conflict.mkv"
            --mode fast-local
            --repair-percent 5
    RESULT_VARIABLE conflict_status)
if(conflict_status EQUAL 0)
    message(FATAL_ERROR "Fast Local repair option conflict was accepted")
endif()

execute_process(
    COMMAND "${CLI}" encode
            --input "${TEST_ROOT}/input.bin"
            --output "${TEST_ROOT}/roundtrip.mkv"
            --mode fast-local
            --no-probe
    RESULT_VARIABLE encode_status)
if(NOT encode_status EQUAL 0)
    message(FATAL_ERROR "Fast Local CLI encode failed")
endif()
execute_process(
    COMMAND "${CLI}" decode
            --input "${TEST_ROOT}/roundtrip.mkv"
            --output "${TEST_ROOT}/restored.bin"
    RESULT_VARIABLE decode_status)
if(NOT decode_status EQUAL 0)
    message(FATAL_ERROR "Fast Local CLI auto-detect decode failed")
endif()
file(SHA256 "${TEST_ROOT}/input.bin" input_sha)
file(SHA256 "${TEST_ROOT}/restored.bin" restored_sha)
if(NOT input_sha STREQUAL restored_sha)
    message(FATAL_ERROR "Fast Local CLI SHA-256 mismatch")
endif()

file(REMOVE_RECURSE "${TEST_ROOT}")
