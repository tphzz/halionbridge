function(halionbridge_sanitize_ref_name input output_var)
    string(REGEX REPLACE "[^A-Za-z0-9._-]+" "-" sanitized "${input}")
    string(REGEX REPLACE "-+" "-" sanitized "${sanitized}")
    string(REGEX REPLACE "^-|-$" "" sanitized "${sanitized}")

    if(sanitized STREQUAL "")
        set(sanitized "unknown")
    endif()

    set(${output_var} "${sanitized}" PARENT_SCOPE)
endfunction()

function(halionbridge_configure_git_version source_dir)
    set(git_tag "")
    set(git_branch "")
    set(git_sha_short "unknown")
    set(is_tagged_release OFF)
    set(is_dirty OFF)
    set(ref_name "local")

    find_package(Git QUIET)

    if(Git_FOUND)
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" rev-parse --short=7 HEAD
            WORKING_DIRECTORY "${source_dir}"
            RESULT_VARIABLE git_sha_result
            OUTPUT_VARIABLE git_sha_output
            ERROR_QUIET
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )

        if(git_sha_result EQUAL 0 AND NOT git_sha_output STREQUAL "")
            set(git_sha_short "${git_sha_output}")
        endif()

        execute_process(
            COMMAND "${GIT_EXECUTABLE}" rev-parse --abbrev-ref HEAD
            WORKING_DIRECTORY "${source_dir}"
            RESULT_VARIABLE git_branch_result
            OUTPUT_VARIABLE git_branch_output
            ERROR_QUIET
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )

        execute_process(
            COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD
            WORKING_DIRECTORY "${source_dir}"
            OUTPUT_VARIABLE git_sha_full
            ERROR_QUIET
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )
        set(is_matching_github_checkout OFF)
        if("$ENV{GITHUB_ACTIONS}" STREQUAL "true" AND NOT git_sha_full STREQUAL "" AND "$ENV{GITHUB_SHA}" STREQUAL git_sha_full)
            set(is_matching_github_checkout ON)
        endif()

        if(git_branch_result EQUAL 0 AND NOT git_branch_output STREQUAL "" AND NOT git_branch_output STREQUAL "HEAD")
            set(ref_name "${git_branch_output}")
            set(git_branch "${git_branch_output}")
        elseif(is_matching_github_checkout AND "$ENV{GITHUB_REF_TYPE}" STREQUAL "branch" AND NOT "$ENV{GITHUB_REF_NAME}" STREQUAL "")
            # Checkout pins the dispatch SHA, so CI branch builds are detached.
            # A tag on that same commit must not turn a branch preview into a release.
            set(ref_name "$ENV{GITHUB_REF_NAME}")
            set(git_branch "$ENV{GITHUB_REF_NAME}")
        elseif(is_matching_github_checkout AND "$ENV{GITHUB_REF_TYPE}" STREQUAL "tag" AND NOT "$ENV{GITHUB_REF_NAME}" STREQUAL "")
            set(ref_name "$ENV{GITHUB_REF_NAME}")
            set(git_tag "$ENV{GITHUB_REF_NAME}")
            set(is_tagged_release ON)
        else()
            execute_process(
                COMMAND "${GIT_EXECUTABLE}" describe --tags --exact-match HEAD
                WORKING_DIRECTORY "${source_dir}"
                RESULT_VARIABLE git_tag_result
                OUTPUT_VARIABLE git_tag_output
                ERROR_QUIET
                OUTPUT_STRIP_TRAILING_WHITESPACE
            )

            if(git_tag_result EQUAL 0 AND NOT git_tag_output STREQUAL "")
                set(ref_name "${git_tag_output}")
                set(git_tag "${git_tag_output}")
                set(is_tagged_release ON)
            else()
                set(ref_name "detached")
                set(git_branch "detached")
            endif()
        endif()

        execute_process(
            COMMAND "${GIT_EXECUTABLE}" status --porcelain --untracked-files=normal
            WORKING_DIRECTORY "${source_dir}"
            RESULT_VARIABLE git_dirty_result
            OUTPUT_VARIABLE git_dirty_output
            ERROR_QUIET
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )

        if(git_dirty_result EQUAL 0 AND NOT git_dirty_output STREQUAL "")
            set(is_dirty ON)
        endif()
    endif()

    halionbridge_sanitize_ref_name("${ref_name}" sanitized_ref)
    set(version_string "${sanitized_ref}-${git_sha_short}")
    set(package_basename "halionbridge-${version_string}")

    if(git_branch STREQUAL "")
        set(git_branch "${sanitized_ref}")
    else()
        halionbridge_sanitize_ref_name("${git_branch}" git_branch)
    endif()

    if(git_tag STREQUAL "" AND is_tagged_release)
        set(git_tag "${sanitized_ref}")
    elseif(NOT git_tag STREQUAL "")
        halionbridge_sanitize_ref_name("${git_tag}" git_tag)
    endif()

    if(is_dirty)
        string(APPEND version_string "-mod")
        string(APPEND package_basename "-mod")
    endif()

    set(HALIONBRIDGE_GIT_TAG "${git_tag}" PARENT_SCOPE)
    set(HALIONBRIDGE_GIT_BRANCH "${git_branch}" PARENT_SCOPE)
    set(HALIONBRIDGE_GIT_SHA_SHORT "${git_sha_short}" PARENT_SCOPE)
    set(HALIONBRIDGE_GIT_DIRTY "${is_dirty}" PARENT_SCOPE)
    set(HALIONBRIDGE_VERSION_STRING "${version_string}" PARENT_SCOPE)
    set(HALIONBRIDGE_PACKAGE_BASENAME "${package_basename}" PARENT_SCOPE)
    set(HALIONBRIDGE_IS_TAGGED_RELEASE "${is_tagged_release}" PARENT_SCOPE)
endfunction()
