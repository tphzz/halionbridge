function(halionbridge_sanitize_ref_name input output_var)
    string(REGEX REPLACE "[^A-Za-z0-9._-]+" "-" sanitized "${input}")
    string(REGEX REPLACE "-+" "-" sanitized "${sanitized}")
    string(REGEX REPLACE "^-|-$" "" sanitized "${sanitized}")

    if(sanitized STREQUAL "")
        set(sanitized "unknown")
    endif()

    set(${output_var} "${sanitized}" PARENT_SCOPE)
endfunction()

function(halionbridge_configure_git_version)
    string(TIMESTAMP build_timestamp_utc "%Y%m%dT%H%M%SZ" UTC)

    set(git_tag "")
    set(git_branch "")
    set(git_sha_short "unknown")
    set(is_tagged_release OFF)
    set(is_dirty OFF)

    find_package(Git QUIET)

    if(Git_FOUND)
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" rev-parse --short=7 HEAD
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            RESULT_VARIABLE git_sha_result
            OUTPUT_VARIABLE git_sha_output
            ERROR_QUIET
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )

        if(git_sha_result EQUAL 0 AND NOT git_sha_output STREQUAL "")
            set(git_sha_short "${git_sha_output}")
        endif()

        execute_process(
            COMMAND "${GIT_EXECUTABLE}" describe --tags --exact-match HEAD
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            RESULT_VARIABLE git_tag_result
            OUTPUT_VARIABLE git_tag_output
            ERROR_QUIET
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )

        if(git_tag_result EQUAL 0 AND NOT git_tag_output STREQUAL "")
            set(git_tag "${git_tag_output}")
            set(is_tagged_release ON)
        endif()

        execute_process(
            COMMAND "${GIT_EXECUTABLE}" rev-parse --abbrev-ref HEAD
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            RESULT_VARIABLE git_branch_result
            OUTPUT_VARIABLE git_branch_output
            ERROR_QUIET
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )

        if(git_branch_result EQUAL 0 AND NOT git_branch_output STREQUAL "" AND NOT git_branch_output STREQUAL "HEAD")
            set(git_branch "${git_branch_output}")
        elseif(DEFINED ENV{GITHUB_REF_NAME} AND NOT "$ENV{GITHUB_REF_NAME}" STREQUAL "")
            set(git_branch "$ENV{GITHUB_REF_NAME}")
        else()
            set(git_branch "detached")
        endif()

        execute_process(
            COMMAND "${GIT_EXECUTABLE}" diff-index --quiet HEAD --
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            RESULT_VARIABLE git_dirty_result
            OUTPUT_QUIET
            ERROR_QUIET
        )

        if(NOT git_dirty_result EQUAL 0)
            set(is_dirty ON)
        endif()
    else()
        set(git_branch "local")
    endif()

    if(is_tagged_release)
        halionbridge_sanitize_ref_name("${git_tag}" sanitized_ref)
        set(version_string "${sanitized_ref}")
        set(package_basename "halionbridge-${sanitized_ref}-${build_timestamp_utc}")
    else()
        halionbridge_sanitize_ref_name("${git_branch}" sanitized_ref)
        set(version_string "${sanitized_ref}-${build_timestamp_utc}-${git_sha_short}")
        set(package_basename "halionbridge-${sanitized_ref}-${build_timestamp_utc}-${git_sha_short}")
    endif()

    if(is_dirty)
        string(APPEND version_string "-dirty")
        string(APPEND package_basename "-dirty")
    endif()

    set(HALIONBRIDGE_GIT_TAG "${git_tag}" PARENT_SCOPE)
    set(HALIONBRIDGE_GIT_BRANCH "${git_branch}" PARENT_SCOPE)
    set(HALIONBRIDGE_GIT_SHA_SHORT "${git_sha_short}" PARENT_SCOPE)
    set(HALIONBRIDGE_GIT_DIRTY "${is_dirty}" PARENT_SCOPE)
    set(HALIONBRIDGE_BUILD_TIMESTAMP_UTC "${build_timestamp_utc}" PARENT_SCOPE)
    set(HALIONBRIDGE_VERSION_STRING "${version_string}" PARENT_SCOPE)
    set(HALIONBRIDGE_PACKAGE_BASENAME "${package_basename}" PARENT_SCOPE)
    set(HALIONBRIDGE_IS_TAGGED_RELEASE "${is_tagged_release}" PARENT_SCOPE)
endfunction()
