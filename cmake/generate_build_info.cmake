cmake_minimum_required(VERSION 3.22)

if(NOT DEFINED HALIONBRIDGE_SOURCE_DIR)
    message(FATAL_ERROR "HALIONBRIDGE_SOURCE_DIR is required.")
endif()

if(NOT DEFINED HALIONBRIDGE_TEMPLATE)
    message(FATAL_ERROR "HALIONBRIDGE_TEMPLATE is required.")
endif()

if(NOT DEFINED HALIONBRIDGE_OUTPUT_CPP)
    message(FATAL_ERROR "HALIONBRIDGE_OUTPUT_CPP is required.")
endif()

if(NOT DEFINED HALIONBRIDGE_OUTPUT_PACKAGE)
    message(FATAL_ERROR "HALIONBRIDGE_OUTPUT_PACKAGE is required.")
endif()

include("${HALIONBRIDGE_SOURCE_DIR}/cmake/halionbridge_version.cmake")

halionbridge_configure_git_version("${HALIONBRIDGE_SOURCE_DIR}")

if(HALIONBRIDGE_IS_TAGGED_RELEASE)
    set(HALIONBRIDGE_IS_TAGGED_RELEASE_CPP "true")
else()
    set(HALIONBRIDGE_IS_TAGGED_RELEASE_CPP "false")
endif()

if(HALIONBRIDGE_GIT_DIRTY)
    set(HALIONBRIDGE_GIT_DIRTY_CPP "true")
else()
    set(HALIONBRIDGE_GIT_DIRTY_CPP "false")
endif()

get_filename_component(output_cpp_dir "${HALIONBRIDGE_OUTPUT_CPP}" DIRECTORY)
file(MAKE_DIRECTORY "${output_cpp_dir}")

set(output_cpp_tmp "${HALIONBRIDGE_OUTPUT_CPP}.tmp")
configure_file("${HALIONBRIDGE_TEMPLATE}" "${output_cpp_tmp}" @ONLY)
file(COPY_FILE "${output_cpp_tmp}" "${HALIONBRIDGE_OUTPUT_CPP}" ONLY_IF_DIFFERENT)
file(REMOVE "${output_cpp_tmp}")

set(package_text "${HALIONBRIDGE_PACKAGE_BASENAME}\n")
set(write_package_file ON)
if(EXISTS "${HALIONBRIDGE_OUTPUT_PACKAGE}")
    file(READ "${HALIONBRIDGE_OUTPUT_PACKAGE}" existing_package_text)
    if(existing_package_text STREQUAL package_text)
        set(write_package_file OFF)
    endif()
endif()

if(write_package_file)
    file(WRITE "${HALIONBRIDGE_OUTPUT_PACKAGE}" "${package_text}")
endif()

message(STATUS "halionbridge version: ${HALIONBRIDGE_VERSION_STRING}")
message(STATUS "halionbridge package basename: ${HALIONBRIDGE_PACKAGE_BASENAME}")
