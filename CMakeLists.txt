cmake_minimum_required(VERSION 3.22)

if(POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)
endif()

if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin" OR CMAKE_SYSTEM_NAME STREQUAL "Darwin" OR DEFINED CMAKE_OSX_ARCHITECTURES)
    if(NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET OR CMAKE_OSX_DEPLOYMENT_TARGET VERSION_LESS "10.15")
        set(CMAKE_OSX_DEPLOYMENT_TARGET "10.15" CACHE STRING "Minimum macOS version for halionbridge builds." FORCE)
    endif()
endif()

project(halionbridge VERSION 0.1.0)

include(CTest)
include(GenerateExportHeader)
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/halionbridge_version.cmake")

# Set C++ standard to C++23 as per requirements. This is cache-backed because
# vendored sfizz writes its own CMAKE_CXX_STANDARD cache entry during configure;
# first-party code and sfizz headers must agree on ABI-affecting standard library
# aliases such as absl::optional.
set(CMAKE_CXX_STANDARD 23 CACHE STRING "C++ standard for halionbridge and first-party integrated dependencies." FORCE)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

option(HALIONBRIDGE_BUILD_CLI "Build the halionbridge CLI executable." ON)
option(HALIONBRIDGE_BUILD_TESTS "Build the halionbridge unit test executable." ${BUILD_TESTING})
option(HALIONBRIDGE_ENABLE_DIAGNOSTICS "Enable verbose JUCE/VST3 diagnostic logging in this build." OFF)
option(HALIONBRIDGE_ENABLE_CRASH_DUMPS "Write a Windows minidump when the bridge process crashes." ON)
option(HALIONBRIDGE_ENABLE_CONVERTERS "Build converter support into the halionbridge CLI." ON)
option(HALIONBRIDGE_ENABLE_CONVERTER_SFZ "Build the built-in SFZ converter." ON)
set(HALIONBRIDGE_EXTRA_CONVERTER_DIRS "" CACHE STRING "Semicolon-separated extra converter source directories.")

set(HALIONBRIDGE_GENERATED_SOURCE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated-src")
set(HALIONBRIDGE_BUILD_INFO_CPP "${HALIONBRIDGE_GENERATED_SOURCE_DIR}/BuildInfo.cpp")
set(HALIONBRIDGE_PACKAGE_BASENAME_FILE "${CMAKE_CURRENT_BINARY_DIR}/halionbridge_package_basename.txt")
set(HALIONBRIDGE_GENERATE_BUILD_INFO_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/cmake/generate_build_info.cmake")
set(HALIONBRIDGE_BUILD_INFO_TEMPLATE "${CMAKE_CURRENT_SOURCE_DIR}/cmake/BuildInfo.cpp.in")

execute_process(
    COMMAND "${CMAKE_COMMAND}"
        "-DHALIONBRIDGE_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DHALIONBRIDGE_TEMPLATE=${HALIONBRIDGE_BUILD_INFO_TEMPLATE}"
        "-DHALIONBRIDGE_OUTPUT_CPP=${HALIONBRIDGE_BUILD_INFO_CPP}"
        "-DHALIONBRIDGE_OUTPUT_PACKAGE=${HALIONBRIDGE_PACKAGE_BASENAME_FILE}"
        -P "${HALIONBRIDGE_GENERATE_BUILD_INFO_SCRIPT}"
    RESULT_VARIABLE HALIONBRIDGE_GENERATE_BUILD_INFO_RESULT
)

if(NOT HALIONBRIDGE_GENERATE_BUILD_INFO_RESULT EQUAL 0)
    message(FATAL_ERROR "Failed to generate halionbridge build metadata.")
endif()

add_custom_target(halionbridge_generate_build_info ALL
    COMMAND "${CMAKE_COMMAND}"
        "-DHALIONBRIDGE_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DHALIONBRIDGE_TEMPLATE=${HALIONBRIDGE_BUILD_INFO_TEMPLATE}"
        "-DHALIONBRIDGE_OUTPUT_CPP=${HALIONBRIDGE_BUILD_INFO_CPP}"
        "-DHALIONBRIDGE_OUTPUT_PACKAGE=${HALIONBRIDGE_PACKAGE_BASENAME_FILE}"
        -P "${HALIONBRIDGE_GENERATE_BUILD_INFO_SCRIPT}"
    BYPRODUCTS
        "${HALIONBRIDGE_BUILD_INFO_CPP}"
        "${HALIONBRIDGE_PACKAGE_BASENAME_FILE}"
    VERBATIM
)

# Add JUCE submodule
add_subdirectory(external/JUCE)
add_subdirectory(external/spdlog EXCLUDE_FROM_ALL)

juce_add_binary_data(halionbridge_assets
    NAMESPACE halionbridge_assets
    HEADER_NAME halionbridge_assets.h
    SOURCES
        halion-lua/builder_bootstrap.vstpreset
        halion-lua/builder.lua
        halion-lua/preset_remap.lua
)

set(HALIONBRIDGE_COMMON_DEFINITIONS
    JUCE_PLUGINHOST_VST3=1
    JUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_MODAL_LOOPS_PERMITTED=1
    $<$<CONFIG:Debug>:DEBUG=1>
)

set(HALIONBRIDGE_DIAGNOSTIC_DEFINITIONS
    JUCE_VST3_DEBUGGING=1
    JUCE_LOG_ASSERTIONS=1
    JUCE_CATCH_UNHANDLED_EXCEPTIONS=1
    HALIONBRIDGE_DIAGNOSTIC_BUILD=1
)

set(HALIONBRIDGE_LIBRARY_SOURCES
    src/Bridge.cpp
    src/BuildWorker.cpp
    src/ChildProcessOutput.cpp
    src/CliCommand.cpp
    src/CrashDiagnostics.cpp
    src/Log.cpp
    src/BuildFile.cpp
    src/PathUtils.cpp
    src/PluginScan.cpp
    src/ProgressMarkers.cpp
    src/PresetRemap.cpp
    "${HALIONBRIDGE_BUILD_INFO_CPP}"
)

set(HALIONBRIDGE_STRICT_WARNING_OPTIONS)
if(MSVC)
    list(APPEND HALIONBRIDGE_STRICT_WARNING_OPTIONS /W4 /WX)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|AppleClang|GNU")
    list(APPEND HALIONBRIDGE_STRICT_WARNING_OPTIONS -Wall -Wextra -Wpedantic -Werror)
endif()

function(enable_halionbridge_strict_warnings)
    if(HALIONBRIDGE_STRICT_WARNING_OPTIONS)
        set_source_files_properties(${ARGN} PROPERTIES
            COMPILE_OPTIONS "${HALIONBRIDGE_STRICT_WARNING_OPTIONS}"
        )
    endif()
endfunction()

enable_halionbridge_strict_warnings(${HALIONBRIDGE_LIBRARY_SOURCES})

if(HALIONBRIDGE_ENABLE_CONVERTERS)
    add_subdirectory(converters)
endif()

set(HALIONBRIDGE_LIBRARY_OUTPUT_DIRECTORY
    "${CMAKE_BINARY_DIR}/halionbridge_library_artefacts/$<CONFIG>"
)

function(configure_halionbridge_library target_name)
    target_compile_features(${target_name} PUBLIC cxx_std_23)

    target_include_directories(${target_name}
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated>
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
    )

    target_compile_definitions(${target_name} PRIVATE
        ${HALIONBRIDGE_COMMON_DEFINITIONS}
        $<$<CONFIG:Debug>:${HALIONBRIDGE_DIAGNOSTIC_DEFINITIONS}>
        $<$<BOOL:${HALIONBRIDGE_ENABLE_DIAGNOSTICS}>:${HALIONBRIDGE_DIAGNOSTIC_DEFINITIONS}>
        $<$<BOOL:${HALIONBRIDGE_ENABLE_CRASH_DUMPS}>:HALIONBRIDGE_ENABLE_CRASH_DUMPS=1>
    )

    target_link_libraries(${target_name} PRIVATE
        halionbridge_assets
        juce::juce_events
        juce::juce_data_structures
        juce::juce_gui_basics
    )

    target_link_libraries(${target_name} PRIVATE
        juce::juce_audio_processors
        juce::juce_core
        spdlog::spdlog_header_only
    )

    if(WIN32 AND HALIONBRIDGE_ENABLE_CRASH_DUMPS)
        target_link_libraries(${target_name} PRIVATE Dbghelp)
    endif()
endfunction()

function(configure_halionbridge_juce_consumer target_name)
    target_include_directories(${target_name} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/external/JUCE/modules
    )

    target_compile_definitions(${target_name} PRIVATE
        ${HALIONBRIDGE_COMMON_DEFINITIONS}
        $<$<CONFIG:Debug>:${HALIONBRIDGE_DIAGNOSTIC_DEFINITIONS}>
        $<$<BOOL:${HALIONBRIDGE_ENABLE_DIAGNOSTICS}>:${HALIONBRIDGE_DIAGNOSTIC_DEFINITIONS}>
        $<$<BOOL:${HALIONBRIDGE_ENABLE_CRASH_DUMPS}>:HALIONBRIDGE_ENABLE_CRASH_DUMPS=1>
    )

    target_link_libraries(${target_name} PRIVATE
        spdlog::spdlog_header_only
    )
endfunction()

add_library(halionbridge_shared SHARED ${HALIONBRIDGE_LIBRARY_SOURCES})
add_dependencies(halionbridge_shared halionbridge_generate_build_info)
set_target_properties(halionbridge_shared PROPERTIES
    OUTPUT_NAME halionbridge
    ARCHIVE_OUTPUT_DIRECTORY "${HALIONBRIDGE_LIBRARY_OUTPUT_DIRECTORY}"
    LIBRARY_OUTPUT_DIRECTORY "${HALIONBRIDGE_LIBRARY_OUTPUT_DIRECTORY}"
    RUNTIME_OUTPUT_DIRECTORY "${HALIONBRIDGE_LIBRARY_OUTPUT_DIRECTORY}"
)
configure_halionbridge_library(halionbridge_shared)
generate_export_header(halionbridge_shared
    BASE_NAME HALIONBRIDGE
    EXPORT_FILE_NAME "${CMAKE_CURRENT_BINARY_DIR}/generated/halionbridge/halionbridge_export.h"
)

add_library(halionbridge_static STATIC ${HALIONBRIDGE_LIBRARY_SOURCES})
add_dependencies(halionbridge_static halionbridge_generate_build_info)
set_target_properties(halionbridge_static PROPERTIES
    OUTPUT_NAME halionbridge_static
    ARCHIVE_OUTPUT_DIRECTORY "${HALIONBRIDGE_LIBRARY_OUTPUT_DIRECTORY}"
    LIBRARY_OUTPUT_DIRECTORY "${HALIONBRIDGE_LIBRARY_OUTPUT_DIRECTORY}"
    RUNTIME_OUTPUT_DIRECTORY "${HALIONBRIDGE_LIBRARY_OUTPUT_DIRECTORY}"
)
configure_halionbridge_library(halionbridge_static)
target_compile_definitions(halionbridge_static PUBLIC HALIONBRIDGE_STATIC_DEFINE)

if(HALIONBRIDGE_BUILD_CLI)
    # Create the Console Application
    juce_add_console_app(halionbridge
        PRODUCT_NAME "halionbridge")

    target_sources(halionbridge PRIVATE
        cli/Main.cpp
    )
    enable_halionbridge_strict_warnings(cli/Main.cpp)

    target_compile_features(halionbridge PRIVATE cxx_std_23)
    configure_halionbridge_juce_consumer(halionbridge)
    target_link_libraries(halionbridge PRIVATE halionbridge_static)

    if(HALIONBRIDGE_ENABLE_CONVERTERS)
        target_sources(halionbridge PRIVATE "${HALIONBRIDGE_CONVERTER_REGISTRATIONS_CPP}")
        target_compile_definitions(halionbridge PRIVATE HALIONBRIDGE_ENABLE_CONVERTERS=1)
        if(HALIONBRIDGE_CONVERTER_TARGETS)
            target_link_libraries(halionbridge PRIVATE ${HALIONBRIDGE_CONVERTER_TARGETS})
        else()
            target_link_libraries(halionbridge PRIVATE "${HALIONBRIDGE_CONVERTER_COMMON_TARGET}")
        endif()
    endif()
endif()

if(HALIONBRIDGE_BUILD_TESTS)
    add_library(halionbridge_public_header_smoke OBJECT
        tests/PublicHeaderSmoke.cpp
    )
    enable_halionbridge_strict_warnings(tests/PublicHeaderSmoke.cpp)

    target_compile_features(halionbridge_public_header_smoke PRIVATE cxx_std_23)
    target_include_directories(halionbridge_public_header_smoke PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_BINARY_DIR}/generated
    )

    if(HALIONBRIDGE_ENABLE_CONVERTERS)
        target_compile_definitions(halionbridge_public_header_smoke PRIVATE HALIONBRIDGE_ENABLE_CONVERTERS=1)
        target_include_directories(halionbridge_public_header_smoke PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/converters/common/include
            ${CMAKE_CURRENT_SOURCE_DIR}/converters/sfz/include
        )
    endif()

    # Create Unit Tests Executable
    juce_add_console_app(halionbridge_tests
        PRODUCT_NAME "halionbridge_tests")

    target_sources(halionbridge_tests PRIVATE
        tests/Tests.cpp
    )
    enable_halionbridge_strict_warnings(tests/Tests.cpp)

    target_compile_features(halionbridge_tests PRIVATE cxx_std_23)
    target_compile_definitions(halionbridge_tests PRIVATE JUCE_UNIT_TESTS=1)
    configure_halionbridge_juce_consumer(halionbridge_tests)
    target_link_libraries(halionbridge_tests PRIVATE halionbridge_static)

    if(HALIONBRIDGE_ENABLE_CONVERTERS)
        target_sources(halionbridge_tests PRIVATE "${HALIONBRIDGE_CONVERTER_REGISTRATIONS_CPP}")
        target_compile_definitions(halionbridge_tests PRIVATE HALIONBRIDGE_ENABLE_CONVERTERS=1)
        if(HALIONBRIDGE_CONVERTER_TARGETS)
            target_link_libraries(halionbridge_tests PRIVATE ${HALIONBRIDGE_CONVERTER_TARGETS})
        else()
            target_link_libraries(halionbridge_tests PRIVATE "${HALIONBRIDGE_CONVERTER_COMMON_TARGET}")
        endif()
    endif()

    if(BUILD_TESTING)
        add_test(NAME halionbridge_tests COMMAND halionbridge_tests)
        set_tests_properties(halionbridge_tests PROPERTIES
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        )
    endif()
endif()
