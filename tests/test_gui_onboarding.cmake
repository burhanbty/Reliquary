file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}")

foreach(SCALE IN ITEMS 1.0 1.25 1.5)
    string(REPLACE "." "_" SCALE_DIR "${SCALE}")
    set(RUN_ROOT "${TEST_ROOT}/scale_${SCALE_DIR}")
    file(MAKE_DIRECTORY "${RUN_ROOT}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env
                "QT_SCALE_FACTOR=${SCALE}"
                "${GUI}" --onboarding-smoke-root "${RUN_ROOT}"
        RESULT_VARIABLE GUI_RESULT
        OUTPUT_VARIABLE GUI_OUTPUT
        ERROR_VARIABLE GUI_ERROR
        TIMEOUT 25)
    if (NOT GUI_RESULT EQUAL 0)
        message(FATAL_ERROR
            "Onboarding qwindows E2E failed at ${SCALE}: ${GUI_RESULT}\n${GUI_OUTPUT}\n${GUI_ERROR}")
    endif ()
    foreach(SCREENSHOT IN ITEMS
            brand-intro-initial.png
            brand-intro-name.png
            brand-intro-definition-1280x720.png
            brand-intro-definition-1366x768.png
            brand-intro-definition-1920x1080.png
            onboarding-1280x720.png
            onboarding-1366x768.png
            onboarding-1920x1080.png
            onboarding-light.png
            onboarding-dark.png
            onboarding-page-2.png
            onboarding-page-3.png)
        if (NOT EXISTS "${RUN_ROOT}/${SCREENSHOT}")
            message(FATAL_ERROR
                "Onboarding screenshot missing at ${SCALE}: ${SCREENSHOT}")
        endif ()
    endforeach()
endforeach()

message(STATUS "Onboarding qwindows E2E passed at 100%, 125%, and 150% DPI")
