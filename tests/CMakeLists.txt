# This file is part of yt-media-storage, a tool for encoding media.
# Copyright (C) 2026 Brandon Li <https://brandonli.me/>
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.

add_executable(media_storage_tests
        test_integrity.cpp
        test_chunker.cpp
        test_codec.cpp
        test_crypto.cpp
        test_roundtrip.cpp
        test_stream.cpp
        test_dct.cpp
        test_api.cpp
        test_performance_profiler.cpp
        test_encoding_reliability.cpp
        test_encoding_preflight.cpp
        test_gui_preflight_model.cpp
        test_fast_local.cpp
        test_youtube_test_lab.cpp
        test_youtube_capacity_lab.cpp
        test_video_set.cpp
        test_video_set_workflow.cpp
)

target_link_libraries(media_storage_tests PRIVATE
        media_storage_core
        media_storage_lib
        GTest::gtest
        GTest::gtest_main
)

target_include_directories(media_storage_tests PRIVATE
        "${CMAKE_SOURCE_DIR}/src"
)

if (WIN32)
    add_custom_command(TARGET media_storage_tests POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:media_storage_tests>
            $<TARGET_FILE_DIR:media_storage_tests>
            COMMAND_EXPAND_LISTS
    )
endif ()

include(GoogleTest)
gtest_discover_tests(media_storage_tests)

if (TARGET media_storage_gui)
    add_executable(media_storage_ui_language_tests
            test_ui_language.cpp
            "${CMAKE_SOURCE_DIR}/src/interface_language.cpp"
            "${CMAKE_SOURCE_DIR}/src/interface_language.h"
    )
    target_include_directories(media_storage_ui_language_tests PRIVATE
            "${CMAKE_SOURCE_DIR}/src")
    target_compile_definitions(media_storage_ui_language_tests PRIVATE
            VIDSTOREX_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    target_link_libraries(media_storage_ui_language_tests PRIVATE
            Qt6::Core
            GTest::gtest
            GTest::gtest_main)
    gtest_discover_tests(media_storage_ui_language_tests)
endif ()

add_test(
        NAME CLI.PreflightIntegration
        COMMAND ${CMAKE_COMMAND}
        "-DCLI=$<TARGET_FILE:media_storage>"
        "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/cli_preflight_integration"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/test_cli_preflight.cmake"
)

add_test(
        NAME CLI.FastLocalIntegration
        COMMAND ${CMAKE_COMMAND}
        "-DCLI=$<TARGET_FILE:media_storage>"
        "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/cli_fast_local_integration"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/test_cli_fast_local.cmake"
)

add_test(
        NAME CLI.YouTubeTestLabIntegration
        COMMAND ${CMAKE_COMMAND}
        "-DCLI=$<TARGET_FILE:media_storage>"
        "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/cli_testlab_integration"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/test_cli_testlab.cmake"
)

add_test(
        NAME CLI.YouTubeCapacityLabIntegration
        COMMAND ${CMAKE_COMMAND}
        "-DCLI=$<TARGET_FILE:media_storage>"
        "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/cli_capacitylab_integration"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/test_cli_capacitylab.cmake"
)

add_test(
        NAME CLI.VideoSetIntegration
        COMMAND ${CMAKE_COMMAND}
        "-DCLI=$<TARGET_FILE:media_storage>"
        "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/cli_video_set_integration"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/test_cli_video_set.cmake"
)

if (TARGET media_storage_gui)
    add_test(
            NAME GUI.LaunchSmoke
            COMMAND $<TARGET_FILE:media_storage_gui>
                    --smoke-test
    )
    set_tests_properties(GUI.LaunchSmoke PROPERTIES TIMEOUT 15)

    add_test(
            NAME GUI.CloseDuringEstimate
            COMMAND $<TARGET_FILE:media_storage_gui>
                    --preflight-smoke-input
                    "${CMAKE_SOURCE_DIR}/README.md"
                    --preflight-smoke-output
                    "${CMAKE_CURRENT_BINARY_DIR}/close-during-estimate.mkv"
                    --close-during-estimate
    )
    set_tests_properties(
            GUI.CloseDuringEstimate PROPERTIES TIMEOUT 15)
    add_test(
            NAME GUI.VideoSetAssistantE2E
            COMMAND ${CMAKE_COMMAND}
                    "-DGUI=$<TARGET_FILE:media_storage_gui>"
                    "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/gui_video_set_assistant"
                    -P "${CMAKE_CURRENT_SOURCE_DIR}/test_gui_video_set_assistant.cmake"
    )
    set_tests_properties(
            GUI.VideoSetAssistantE2E PROPERTIES TIMEOUT 130)
    if (WIN32 AND TARGET Qt6::QWindowsIntegrationPlugin)
        set_tests_properties(
                GUI.LaunchSmoke
                GUI.CloseDuringEstimate
                GUI.VideoSetAssistantE2E
                PROPERTIES ENVIRONMENT
                "QT_QPA_PLATFORM_PLUGIN_PATH=$<TARGET_FILE_DIR:Qt6::QWindowsIntegrationPlugin>")
    endif ()
endif ()
