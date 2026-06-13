set_property(GLOBAL PROPERTY HALIONBRIDGE_CONVERTER_REGISTRATION_INCLUDES "")
set_property(GLOBAL PROPERTY HALIONBRIDGE_CONVERTER_REGISTRATION_CALLS "")
set_property(GLOBAL PROPERTY HALIONBRIDGE_CONVERTER_TARGETS "")

function(halionbridge_register_converter_target)
    set(options)
    set(one_value_args TARGET REGISTRATION_FUNCTION HEADER)
    set(multi_value_args)
    cmake_parse_arguments(HALIONBRIDGE_CONVERTER "${options}" "${one_value_args}" "${multi_value_args}" ${ARGN})

    if(NOT HALIONBRIDGE_CONVERTER_TARGET)
        message(FATAL_ERROR "halionbridge_register_converter_target requires TARGET.")
    endif()

    if(NOT HALIONBRIDGE_CONVERTER_REGISTRATION_FUNCTION)
        message(FATAL_ERROR "halionbridge_register_converter_target requires REGISTRATION_FUNCTION.")
    endif()

    if(NOT HALIONBRIDGE_CONVERTER_HEADER)
        message(FATAL_ERROR "halionbridge_register_converter_target requires HEADER.")
    endif()

    set_property(GLOBAL APPEND PROPERTY HALIONBRIDGE_CONVERTER_REGISTRATION_INCLUDES
        "#include \"${HALIONBRIDGE_CONVERTER_HEADER}\"\n"
    )
    set_property(GLOBAL APPEND PROPERTY HALIONBRIDGE_CONVERTER_REGISTRATION_CALLS
        "    ${HALIONBRIDGE_CONVERTER_REGISTRATION_FUNCTION}(registry)\\;\n"
    )
    set_property(GLOBAL APPEND PROPERTY HALIONBRIDGE_CONVERTER_TARGETS
        "${HALIONBRIDGE_CONVERTER_TARGET}"
    )
endfunction()

set(HALIONBRIDGE_CONVERTER_COMMON_SOURCES
    ${CMAKE_SOURCE_DIR}/converters/common/src/Converter.cpp
    ${CMAKE_SOURCE_DIR}/converters/common/src/BuildDirectoryEmitter.cpp
)

add_library(halionbridge_converter_common_static STATIC ${HALIONBRIDGE_CONVERTER_COMMON_SOURCES})
target_compile_features(halionbridge_converter_common_static PUBLIC cxx_std_23)
target_include_directories(halionbridge_converter_common_static
    PUBLIC
        ${CMAKE_SOURCE_DIR}/converters/common/include
)
enable_halionbridge_strict_warnings(${HALIONBRIDGE_CONVERTER_COMMON_SOURCES})

if(HALIONBRIDGE_ENABLE_CONVERTER_SFZ)
    function(halionbridge_add_sfizz_dependency)
        set(HALIONBRIDGE_RESTORE_BUILD_TESTING "${BUILD_TESTING}")
        set(HALIONBRIDGE_RESTORE_C_FLAGS "${CMAKE_C_FLAGS}")
        set(HALIONBRIDGE_RESTORE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
        if(DEFINED CACHE{CMAKE_DISABLE_FIND_PACKAGE_Qt5})
            set(HALIONBRIDGE_RESTORE_DISABLE_FIND_QT5_DEFINED TRUE)
            set(HALIONBRIDGE_RESTORE_DISABLE_FIND_QT5 "$CACHE{CMAKE_DISABLE_FIND_PACKAGE_Qt5}")
        else()
            set(HALIONBRIDGE_RESTORE_DISABLE_FIND_QT5_DEFINED FALSE)
        endif()
        if(DEFINED CACHE{CMAKE_POLICY_DEFAULT_CMP0077})
            set(HALIONBRIDGE_RESTORE_POLICY_CMP0077_DEFINED TRUE)
            set(HALIONBRIDGE_RESTORE_POLICY_CMP0077 "$CACHE{CMAKE_POLICY_DEFAULT_CMP0077}")
        else()
            set(HALIONBRIDGE_RESTORE_POLICY_CMP0077_DEFINED FALSE)
        endif()
        if(DEFINED CACHE{CMAKE_SUPPRESS_DEVELOPER_WARNINGS})
            set(HALIONBRIDGE_RESTORE_SUPPRESS_DEVELOPER_WARNINGS_DEFINED TRUE)
            set(HALIONBRIDGE_RESTORE_SUPPRESS_DEVELOPER_WARNINGS "$CACHE{CMAKE_SUPPRESS_DEVELOPER_WARNINGS}")
        else()
            set(HALIONBRIDGE_RESTORE_SUPPRESS_DEVELOPER_WARNINGS_DEFINED FALSE)
        endif()
        if(DEFINED CACHE{CMAKE_WARN_DEPRECATED})
            set(HALIONBRIDGE_RESTORE_WARN_DEPRECATED_DEFINED TRUE)
            set(HALIONBRIDGE_RESTORE_WARN_DEPRECATED "$CACHE{CMAKE_WARN_DEPRECATED}")
        else()
            set(HALIONBRIDGE_RESTORE_WARN_DEPRECATED_DEFINED FALSE)
        endif()

        set(BUILD_TESTING OFF CACHE BOOL "Disable third-party tests while configuring sfizz." FORCE)
        set(ENABLE_LTO OFF CACHE BOOL "Disable sfizz LTO in halionbridge builds." FORCE)
        set(SFIZZ_RENDER OFF CACHE BOOL "Disable sfizz render client." FORCE)
        set(SFIZZ_BENCHMARKS OFF CACHE BOOL "Disable sfizz benchmarks." FORCE)
        set(SFIZZ_TESTS OFF CACHE BOOL "Disable sfizz tests." FORCE)
        set(SFIZZ_DEMOS OFF CACHE BOOL "Disable sfizz demos." FORCE)
        set(SFIZZ_DEVTOOLS OFF CACHE BOOL "Disable sfizz developer tools." FORCE)
        set(SFIZZ_SHARED OFF CACHE BOOL "Disable sfizz shared library." FORCE)
        set(SFIZZ_USE_SNDFILE OFF CACHE BOOL "Disable sfizz sndfile dependency." FORCE)
        set(SFIZZ_GIT_SUBMODULE_CHECK OFF CACHE BOOL "halionbridge initializes sfizz submodules through Git directly." FORCE)
        set(ABSL_PROPAGATE_CXX_STD ON CACHE BOOL "Keep vendored Abseil aligned with halionbridge's C++ standard." FORCE)

        # sfizz's bundled dependency tree contains older CMake projects and optional UI/tools probes.
        # Keep that configure noise scoped to the vendored subdirectory without hiding first-party warnings.
        set(CMAKE_DISABLE_FIND_PACKAGE_Qt5 TRUE CACHE BOOL "Disable sfizz's unused Qt development-tool probe." FORCE)
        set(CMAKE_POLICY_DEFAULT_CMP0077 NEW CACHE STRING "Honor normal variables in vendored dependency option() calls." FORCE)
        set(CMAKE_SUPPRESS_DEVELOPER_WARNINGS ON CACHE BOOL "Suppress vendored CMake developer warnings while configuring sfizz." FORCE)
        set(CMAKE_WARN_DEPRECATED OFF CACHE BOOL "Suppress vendored CMake deprecation warnings while configuring sfizz." FORCE)
        if(MSVC)
            string(APPEND CMAKE_CXX_FLAGS " /D_SILENCE_CXX23_ALIGNED_STORAGE_DEPRECATION_WARNING /D_SILENCE_CXX23_DENORM_DEPRECATION_WARNING")
        elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|AppleClang")
            if(CMAKE_C_COMPILER_ID MATCHES "Clang|AppleClang")
                string(APPEND CMAKE_C_FLAGS " -w -Qunused-arguments")
            endif()
            string(APPEND CMAKE_CXX_FLAGS
                " -w"
                " -Qunused-arguments"
                " -Wno-deprecated-builtins"
                " -Wno-deprecated-declarations"
                " -Wno-missing-template-arg-list-after-template-kw"
                " -Wno-nan-infinity-disabled"
            )
        endif()
        list(PREPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake/sfizz-overrides")

        add_subdirectory(${CMAKE_SOURCE_DIR}/external/sfizz ${CMAKE_BINARY_DIR}/external/sfizz EXCLUDE_FROM_ALL)
        set(CMAKE_C_FLAGS "${HALIONBRIDGE_RESTORE_C_FLAGS}")
        set(CMAKE_CXX_FLAGS "${HALIONBRIDGE_RESTORE_CXX_FLAGS}")
        set(BUILD_TESTING "${HALIONBRIDGE_RESTORE_BUILD_TESTING}" CACHE BOOL "Build halionbridge tests." FORCE)

        if(HALIONBRIDGE_RESTORE_DISABLE_FIND_QT5_DEFINED)
            set(CMAKE_DISABLE_FIND_PACKAGE_Qt5 "${HALIONBRIDGE_RESTORE_DISABLE_FIND_QT5}" CACHE BOOL "Disable find_package(Qt5)." FORCE)
        else()
            unset(CMAKE_DISABLE_FIND_PACKAGE_Qt5 CACHE)
        endif()
        if(HALIONBRIDGE_RESTORE_POLICY_CMP0077_DEFINED)
            set(CMAKE_POLICY_DEFAULT_CMP0077 "${HALIONBRIDGE_RESTORE_POLICY_CMP0077}" CACHE STRING "Default policy setting for CMP0077." FORCE)
        else()
            unset(CMAKE_POLICY_DEFAULT_CMP0077 CACHE)
        endif()
        if(HALIONBRIDGE_RESTORE_SUPPRESS_DEVELOPER_WARNINGS_DEFINED)
            set(CMAKE_SUPPRESS_DEVELOPER_WARNINGS "${HALIONBRIDGE_RESTORE_SUPPRESS_DEVELOPER_WARNINGS}" CACHE BOOL "Suppress CMake developer warnings." FORCE)
        else()
            unset(CMAKE_SUPPRESS_DEVELOPER_WARNINGS CACHE)
        endif()
        if(HALIONBRIDGE_RESTORE_WARN_DEPRECATED_DEFINED)
            set(CMAKE_WARN_DEPRECATED "${HALIONBRIDGE_RESTORE_WARN_DEPRECATED}" CACHE BOOL "Whether to warn about deprecated CMake usage." FORCE)
        else()
            unset(CMAKE_WARN_DEPRECATED CACHE)
        endif()
    endfunction()

    halionbridge_add_sfizz_dependency()

    set(HALIONBRIDGE_CONVERTER_SFZ_SOURCES
        ${CMAKE_SOURCE_DIR}/converters/sfz/src/SfzConverter.cpp
    )

    add_library(halionbridge_converter_sfz_static STATIC ${HALIONBRIDGE_CONVERTER_SFZ_SOURCES})
    target_compile_features(halionbridge_converter_sfz_static PUBLIC cxx_std_23)
    target_include_directories(halionbridge_converter_sfz_static
        PUBLIC
            ${CMAKE_SOURCE_DIR}/converters/sfz/include
    )
    target_include_directories(halionbridge_converter_sfz_static SYSTEM
        PRIVATE
            ${CMAKE_SOURCE_DIR}/external/sfizz/src
            ${CMAKE_SOURCE_DIR}/external/sfizz/external/abseil-cpp
            ${CMAKE_SOURCE_DIR}/external/sfizz/external/filesystem/include
            ${CMAKE_SOURCE_DIR}/external/sfizz/external/simde
    )
    target_compile_definitions(halionbridge_converter_sfz_static PRIVATE
        _SILENCE_CXX23_ALIGNED_STORAGE_DEPRECATION_WARNING
        _SILENCE_CXX23_DENORM_DEPRECATION_WARNING
    )
    target_link_libraries(halionbridge_converter_sfz_static
        PRIVATE
            halionbridge_converter_common_static
            sfizz::sfizz
    )
    target_include_directories(halionbridge_converter_sfz_static
        PUBLIC
            ${CMAKE_SOURCE_DIR}/converters/common/include
    )
    enable_halionbridge_strict_warnings(${HALIONBRIDGE_CONVERTER_SFZ_SOURCES})

    halionbridge_register_converter_target(
        TARGET halionbridge_converter_sfz_static
        REGISTRATION_FUNCTION halionbridge::converters::sfz::registerConverter
        HEADER halionbridge_converters/sfz/SfzConverter.h
    )
endif()

if(EXISTS "${CMAKE_SOURCE_DIR}/converters.local.cmake")
    include("${CMAKE_SOURCE_DIR}/converters.local.cmake")
endif()

foreach(converter_dir IN LISTS HALIONBRIDGE_EXTRA_CONVERTER_DIRS)
    if(NOT IS_DIRECTORY "${converter_dir}" OR NOT EXISTS "${converter_dir}/CMakeLists.txt")
        message(FATAL_ERROR "HALIONBRIDGE_EXTRA_CONVERTER_DIRS entry is not a converter directory: ${converter_dir}")
    endif()

    get_filename_component(converter_name "${converter_dir}" NAME)
    add_subdirectory("${converter_dir}" "${CMAKE_CURRENT_BINARY_DIR}/extra-converters/${converter_name}")
endforeach()

file(GLOB HALIONBRIDGE_PRIVATE_CONVERTER_DIRS CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/converters/private/*"
)

foreach(converter_dir IN LISTS HALIONBRIDGE_PRIVATE_CONVERTER_DIRS)
    if(IS_DIRECTORY "${converter_dir}" AND EXISTS "${converter_dir}/CMakeLists.txt")
        get_filename_component(converter_name "${converter_dir}" NAME)
        add_subdirectory("${converter_dir}" "${CMAKE_CURRENT_BINARY_DIR}/private-converters/${converter_name}")
    endif()
endforeach()

get_property(HALIONBRIDGE_CONVERTER_REGISTRATION_INCLUDE_LIST GLOBAL PROPERTY HALIONBRIDGE_CONVERTER_REGISTRATION_INCLUDES)
get_property(HALIONBRIDGE_CONVERTER_REGISTRATION_CALL_LIST GLOBAL PROPERTY HALIONBRIDGE_CONVERTER_REGISTRATION_CALLS)
get_property(HALIONBRIDGE_CONVERTER_TARGETS GLOBAL PROPERTY HALIONBRIDGE_CONVERTER_TARGETS)

string(JOIN "" HALIONBRIDGE_CONVERTER_REGISTRATION_INCLUDES ${HALIONBRIDGE_CONVERTER_REGISTRATION_INCLUDE_LIST})
string(JOIN "" HALIONBRIDGE_CONVERTER_REGISTRATION_CALLS ${HALIONBRIDGE_CONVERTER_REGISTRATION_CALL_LIST})

set(HALIONBRIDGE_CONVERTER_REGISTRATIONS_CPP "${CMAKE_CURRENT_BINARY_DIR}/generated-src/ConverterRegistrations.cpp")
configure_file(
    "${CMAKE_SOURCE_DIR}/converters/ConverterRegistrations.cpp.in"
    "${HALIONBRIDGE_CONVERTER_REGISTRATIONS_CPP}"
    @ONLY
)

set(HALIONBRIDGE_CONVERTER_TARGETS "${HALIONBRIDGE_CONVERTER_TARGETS}" PARENT_SCOPE)
set(HALIONBRIDGE_CONVERTER_REGISTRATIONS_CPP "${HALIONBRIDGE_CONVERTER_REGISTRATIONS_CPP}" PARENT_SCOPE)
set(HALIONBRIDGE_CONVERTER_COMMON_TARGET halionbridge_converter_common_static PARENT_SCOPE)
