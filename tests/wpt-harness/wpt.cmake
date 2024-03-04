enable_testing()

add_builtin(wpt_builtins SRC "${CMAKE_CURRENT_LIST_DIR}/wpt_builtins.cpp")
target_include_directories(wpt_builtins PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/builtins/web/")

if(NOT DEFINED ENV{WPT_ROOT})
    message(FATAL_ERROR "WPT_ROOT environment variable is not set")
endif()

add_custom_command(
        OUTPUT wpt-runtime.wasm
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        COMMAND ${CMAKE_COMMAND} -E env PATH=${WASM_TOOLS_DIR}:${WIZER_DIR}:$ENV{PATH} WPT_ROOT=$ENV{WPT_ROOT} ${CMAKE_CURRENT_SOURCE_DIR}/tests/wpt-harness/build-wpt-runtime.sh
        DEPENDS starling.wasm componentize.sh tests/wpt-harness/build-wpt-runtime.sh tests/wpt-harness/pre-harness.js tests/wpt-harness/post-harness.js
        VERBATIM
)

add_custom_target(wpt-runtime DEPENDS wpt-runtime.wasm)
if (CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(BT_DETAILS "1")
endif ()

add_test(
        NAME wpt
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        COMMAND ${CMAKE_COMMAND} -E env PATH=${WASMTIME_DIR}:$ENV{PATH} WASMTIME_BACKTRACE_DETAILS=${BT_DETAILS} node ${CMAKE_CURRENT_SOURCE_DIR}/tests/wpt-harness/run-wpt.mjs --wpt-root=$ENV{WPT_ROOT} -v $ENV{WPT_FILTER}
)
