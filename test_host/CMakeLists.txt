# Host unit tests for pure-C components (codec, rtp, sip, ...).
# Each test_host/<name>/CMakeLists.txt adds its own executable and registers
# it with add_test(). Run: tools/ci.sh host-test
cmake_minimum_required(VERSION 3.22)
project(sipwire_host_tests C)

set(CMAKE_C_STANDARD 17)
add_compile_options(-Wall -Wextra -Werror -g -fsanitize=address,undefined)
add_link_options(-fsanitize=address,undefined)

if(NOT DEFINED ENV{IDF_PATH})
    message(FATAL_ERROR "IDF_PATH not set (Unity is taken from ESP-IDF)")
endif()
set(UNITY_DIR $ENV{IDF_PATH}/components/unity/unity/src)
add_library(unity STATIC ${UNITY_DIR}/unity.c)
target_include_directories(unity PUBLIC ${UNITY_DIR})

set(SIPWIRE_COMPONENTS ${CMAKE_CURRENT_SOURCE_DIR}/../components)
enable_testing()

file(GLOB children LIST_DIRECTORIES true ${CMAKE_CURRENT_SOURCE_DIR}/*)
foreach(child ${children})
    if(IS_DIRECTORY ${child} AND EXISTS ${child}/CMakeLists.txt)
        add_subdirectory(${child})
    endif()
endforeach()
