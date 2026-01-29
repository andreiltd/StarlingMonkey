cmake_minimum_required(VERSION 3.27)
set(CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)

# Slim build option - creates a minimal runtime targeting wasi:cli only
option(BUILD_SLIM "Build a minimal slim variant without HTTP, crypto, and most builtins" OFF)

if(BUILD_SLIM)
    message(STATUS "Building SLIM variant - minimal runtime with reduced builtins")
    # Force disable builtins that are not needed for slim build
    set(ENABLE_BUILTIN_WEB_FETCH OFF CACHE BOOL "" FORCE)
    set(ENABLE_BUILTIN_WEB_FETCH_FETCH_EVENT OFF CACHE BOOL "" FORCE)
    set(ENABLE_BUILTIN_WEB_CRYPTO OFF CACHE BOOL "" FORCE)
    set(ENABLE_BUILTIN_WEB_FORM_DATA OFF CACHE BOOL "" FORCE)
    set(ENABLE_BUILTIN_WEB_STREAMS OFF CACHE BOOL "" FORCE)
    set(ENABLE_BUILTIN_WEB_ABORT OFF CACHE BOOL "" FORCE)
    set(ENABLE_BUILTIN_WEB_BLOB OFF CACHE BOOL "" FORCE)
    set(ENABLE_BUILTIN_WEB_FILE OFF CACHE BOOL "" FORCE)
    set(ENABLE_BUILTIN_WEB_EVENT OFF CACHE BOOL "" FORCE)
    set(ENABLE_BUILTIN_WEB_STRUCTURED_CLONE OFF CACHE BOOL "" FORCE)
    set(ENABLE_BUILTIN_WEB_DOM_EXCEPTION OFF CACHE BOOL "" FORCE)
    set(ENABLE_BUILTIN_WEB_URL OFF CACHE BOOL "" FORCE)
    set(ENABLE_BUILTIN_WEB_BASE64 OFF CACHE BOOL "" FORCE)
    set(ENABLE_BUILTIN_WEB_WORKER_LOCATION OFF CACHE BOOL "" FORCE)
    # Keep ENABLE_BUILTIN_WEB_PERFORMANCE enabled (needed by ComponentizeJS embedding)
    set(ENABLE_BUILTIN_WPT_SUPPORT OFF CACHE BOOL "" FORCE)
    set(ENABLE_JS_DEBUGGER OFF CACHE BOOL "" FORCE)
    # Use size optimization for slim builds
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Oz")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Oz")
endif()

if (DEFINED ENV{HOST_API})
    set(HOST_API $ENV{HOST_API})
    if (EXISTS ${HOST_API})
        cmake_path(ABSOLUTE_PATH HOST_API)
    else ()
        set(HOST_API ${CMAKE_CURRENT_SOURCE_DIR}/host-apis/$ENV{HOST_API})
    endif()
    if (NOT EXISTS ${HOST_API})
        message(FATAL_ERROR "Host API `$ENV{HOST_API}` not found. The HOST_API environment \
            variable must be the name of a host API implementation provided in the `host-apis` \
            folder of StarlingMonkey, or be an absolute path pointing to custom host API implementation.")
    endif()
else()
    set(HOST_API ${CMAKE_CURRENT_SOURCE_DIR}/host-apis/wasi-0.2.3)
endif()
message(STATUS "Using host API: ${HOST_API}")

# Ensure that the CPM cache is created outside the build dir, even if no location is specified by the developer.
if(NOT DEFINED ENV{CPM_SOURCE_CACHE})
    set(ENV{CPM_SOURCE_CACHE} ${CMAKE_CURRENT_SOURCE_DIR}/deps/cpm_cache)
endif()
include("CPM")
include("toolchain")

project(StarlingMonkey)

include("init-corrosion")

include("wasm-tools")
include("binaryen")
include("wizer")
include("weval")
include("wasmtime")
include("cbindgen")

include("spidermonkey")

# OpenSSL is only needed for the crypto builtin
option(ENABLE_OPENSSL "Build OpenSSL for crypto support" ON)
if(BUILD_SLIM)
    set(ENABLE_OPENSSL OFF CACHE BOOL "" FORCE)
endif()
if(ENABLE_OPENSSL)
    include("openssl")
endif()

include("${HOST_API}/host_api.cmake")
include("build-crates")
include("lint")

option(ENABLE_JS_DEBUGGER "Enable support for debugging content via a socket connection" ON)

add_library(extension_api INTERFACE include/extension-api.h runtime/encode.h runtime/decode.h)
if (ENABLE_JS_DEBUGGER)
    target_compile_definitions(extension_api INTERFACE ENABLE_JS_DEBUGGER)
endif()
target_link_libraries(extension_api INTERFACE rust-url spidermonkey)
target_include_directories(extension_api INTERFACE include deps/include runtime)

include("builtins")

include("tests/wpt-harness/wpt.cmake")

set(SOURCES
    runtime/js.cpp
    runtime/allocator.cpp
    runtime/encode.cpp
    runtime/decode.cpp
    runtime/engine.cpp
    runtime/event_loop.cpp
    runtime/builtin.cpp
    runtime/script_loader.cpp
    runtime/debugger.cpp
)

add_executable(starling-raw.wasm ${SOURCES})

target_link_libraries(starling-raw.wasm PRIVATE host_api extension_api builtins spidermonkey rust-crates)

option(USE_WASM_OPT "use wasm-opt to optimize the StarlingMonkey binary" ON)

# Choose optimization level based on BUILD_SLIM
if(BUILD_SLIM)
    set(WASM_OPT_LEVEL "-Oz")
else()
    set(WASM_OPT_LEVEL "-O3")
endif()

# For release builds, use wasm-opt to optimize the generated wasm file.
if (USE_WASM_OPT)
    if (CMAKE_BUILD_TYPE STREQUAL "Release")
        add_custom_command(
                TARGET starling-raw.wasm
                POST_BUILD
                COMMAND ${WASM_OPT} --strip-debug ${WASM_OPT_LEVEL} -o starling-raw.wasm starling-raw.wasm
                WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        )
    elseif (CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
        add_custom_command(
                TARGET starling-raw.wasm
                POST_BUILD
                COMMAND ${WASM_OPT} ${WASM_OPT_LEVEL} -g -o starling-raw.wasm starling-raw.wasm
                WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        )
    endif()
else()
    if (CMAKE_BUILD_TYPE STREQUAL "Release")
        add_custom_command(
            TARGET starling-raw.wasm
            POST_BUILD
            COMMAND ${WASM_OPT} --strip-debug -O0 -o starling-raw.wasm starling-raw.wasm
            WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        )
    endif()
endif()

# build a compilation cache of ICs
if(WEVAL)
    include("weval")
    file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/null.js "function main() {}")
    add_custom_target(
        starling-ics.wevalcache
        ALL
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        COMMAND rm -f starling-ics.wevalcache
        COMMAND echo ./null.js | ${WEVAL_BIN} weval --dir . --show-stats --cache starling-ics.wevalcache -w -i starling-raw.wasm -o /dev/null
        DEPENDS starling-raw.wasm
        VERBATIM
    )
    set(WEVAL_CACHE_FILE "starling-ics.wevalcache")
    set(AOT 1)
else()
    set(AOT 0)
endif()

set(RUNTIME_FILE "starling-raw.wasm")
set(ADAPTER_FILE "preview1-adapter.wasm")
configure_file("componentize.sh.in" "${CMAKE_CURRENT_BINARY_DIR}/componentize.sh" @ONLY)
if(EXISTS ${ADAPTER})
    configure_file(${ADAPTER} "${CMAKE_CURRENT_BINARY_DIR}/${ADAPTER_FILE}" COPYONLY)
endif()

function(componentize OUTPUT)
    set(options)
    set(oneValueArgs)
    set(multiValueArgs SOURCES)
    cmake_parse_arguments(ARG "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})
    list(TRANSFORM ARG_SOURCES PREPEND ${CMAKE_CURRENT_SOURCE_DIR}/)
    list(JOIN ARG_SOURCES " " SOURCES)
    get_target_property(RUNTIME_DIR starling-raw.wasm BINARY_DIR)

    add_custom_command(
            OUTPUT ${OUTPUT}.wasm
            WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
            COMMAND ${RUNTIME_DIR}/componentize.sh ${SOURCES} -o ${OUTPUT}.wasm
            DEPENDS ${ARG_SOURCES} ${RUNTIME_DIR}/componentize.sh starling-raw.wasm
            VERBATIM
    )
    add_custom_target(${OUTPUT} DEPENDS ${OUTPUT}.wasm)
endfunction()

componentize(smoke-test SOURCES tests/cases/smoke/smoke.js)
componentize(starling)

include("tests/tests.cmake")
