cmake_minimum_required(VERSION 3.16)
project(sipwire_sim C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

if(NOT EMSCRIPTEN)
    message(FATAL_ERROR "Configure through emcmake (see sim/web/build.sh)")
endif()

get_filename_component(SIPWIRE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

# ---- LVGL (pinned) ---------------------------------------------------------
include(FetchContent)
set(LV_BUILD_CONF_PATH "${CMAKE_CURRENT_LIST_DIR}/lv_conf.h" CACHE PATH "" FORCE)
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_USE_THORVG OFF CACHE BOOL "" FORCE)
set(LV_BUILD_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(lvgl
    GIT_REPOSITORY https://github.com/lvgl/lvgl
    GIT_TAG        v9.6.0
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(lvgl)

# SDL2 comes from Emscripten's port, not from find_package.
target_compile_options(lvgl PUBLIC -sUSE_SDL=2)

# ---- components/ui (unchanged firmware sources) ---------------------------
file(GLOB UI_SOURCES CONFIGURE_DEPENDS "${SIPWIRE_ROOT}/components/ui/*.c")
file(GLOB SIM_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/src/*.c")

add_executable(index ${UI_SOURCES} ${SIM_SOURCES})
target_include_directories(index PRIVATE "${SIPWIRE_ROOT}/components/ui/include")
target_compile_options(index PRIVATE -Wall -Wextra)
target_link_libraries(index PRIVATE lvgl)

set(SIM_EXPORTS
    _main _sim_set_net _sim_set_reg _sim_set_battery _sim_incoming
    _sim_remote_hangup _sim_set_call _sim_key _sim_set_time _sim_set_clock24 _sim_set_ota _sim_touch_raw _sim_get_model_json
    _sim_set_blf_count _sim_set_blf _sim_set_mwi
    _sim_set_unseen _sim_clear_directory)
list(JOIN SIM_EXPORTS "," SIM_EXPORTS_CSV)

target_link_options(index PRIVATE
    -O1   # link-time only: skips the slow wasm-opt pass, keeps rebuilds quick
    -sUSE_SDL=2
    -sALLOW_MEMORY_GROWTH=1
    "-sEXPORTED_FUNCTIONS=${SIM_EXPORTS_CSV}"
    "-sEXPORTED_RUNTIME_METHODS=ccall,cwrap,UTF8ToString,stringToUTF8,lengthBytesUTF8"
    --shell-file "${CMAKE_CURRENT_LIST_DIR}/shell.html")
set_target_properties(index PROPERTIES
    SUFFIX ".html"
    LINK_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/shell.html")
