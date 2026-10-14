cmake_minimum_required(VERSION 3.20)
project(kage_typing_web LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
if(NOT EMSCRIPTEN)
  message(FATAL_ERROR "Use emcmake cmake -S simulator -B build/typing-wasm")
endif()
if(NOT DEFINED LVGL_SOURCE)
  message(FATAL_ERROR "Set -DLVGL_SOURCE=/path/to/lvgl-v9.6.0")
endif()
set(LV_BUILD_CONF_PATH "${CMAKE_CURRENT_SOURCE_DIR}/lv_conf.h" CACHE FILEPATH "" FORCE)
add_subdirectory("${LVGL_SOURCE}" "${CMAKE_CURRENT_BINARY_DIR}/lvgl")
add_executable(kage_lvgl wasm_bridge.cpp typing_scene.cpp)
target_include_directories(kage_lvgl PRIVATE "${LVGL_SOURCE}")
target_link_libraries(kage_lvgl PRIVATE lvgl)
target_compile_options(kage_lvgl PRIVATE -O2 -Wall -Wextra)
set_target_properties(kage_lvgl PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../backend/typing_simulator_dist"
  OUTPUT_NAME "kage_lvgl"
  SUFFIX ".js")
target_link_options(kage_lvgl PRIVATE
  "SHELL:--no-entry"
  "SHELL:-sMODULARIZE=1"
  "SHELL:-sEXPORT_NAME=createKageLVGL"
  "SHELL:-sALLOW_MEMORY_GROWTH=1"
  "SHELL:-sINITIAL_MEMORY=16777216"
  "SHELL:-sSTACK_SIZE=524288"
  "SHELL:-sENVIRONMENT=web"
  "SHELL:-sFILESYSTEM=0"
  "SHELL:-sEXPORTED_FUNCTIONS=_malloc,_free,_kage_init,_kage_framebuffer,_kage_field_count,_kage_defaults,_kage_set_config,_kage_press,_kage_frame,_kage_mode,_kage_reset,_kage_strokes,_kage_pending,_kage_is_raised,_kage_tick"
  "SHELL:-sEXPORTED_RUNTIME_METHODS=HEAPU8,HEAP32"
  "SHELL:-sNO_EXIT_RUNTIME=1"
  "SHELL:-O2"
)
