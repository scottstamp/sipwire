# sipwire - ESP-IDF project
cmake_minimum_required(VERSION 3.22)

# Firmware version shown in the UI, `ota status` and the log (esp_app_desc_t.version, 31 chars):
# `git describe` of this tree, unless PROJECT_VER is set (-DPROJECT_VER=...) or a version.txt exists.
if(NOT DEFINED PROJECT_VER AND NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/version.txt")
    execute_process(COMMAND git describe --always --tags --dirty
                    WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}"
                    OUTPUT_VARIABLE SIPWIRE_GIT_VERSION OUTPUT_STRIP_TRAILING_WHITESPACE
                    ERROR_QUIET RESULT_VARIABLE SIPWIRE_GIT_RESULT)
    if(SIPWIRE_GIT_RESULT EQUAL 0 AND SIPWIRE_GIT_VERSION)
        set(PROJECT_VER "${SIPWIRE_GIT_VERSION}")
    else()
        set(PROJECT_VER "0.0-nogit")   # e.g. a source tarball without .git
    endif()
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(sipwire)
