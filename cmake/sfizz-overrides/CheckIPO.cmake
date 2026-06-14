# sfizz's bundled CheckIPO.cmake emits a warning when IPO is intentionally off.
# halionbridge disables sfizz LTO, so keep the helper behavior without warning during configure.
include(CheckIPOSupported)
check_ipo_supported(RESULT SFIZZ_IPO_SUPPORTED OUTPUT SFIZZ_IPO_OUTPUT)

if(CMAKE_SYSTEM_PROCESSOR STREQUAL armv7l)
    set(ENABLE_LTO OFF CACHE BOOL "" FORCE)
endif()

if(ENABLE_LTO AND CMAKE_BUILD_TYPE STREQUAL "Release" AND SFIZZ_IPO_SUPPORTED)
    message(STATUS "\nLTO enabled.")
else()
    set(ENABLE_LTO OFF CACHE BOOL "" FORCE)
endif()

function(SFIZZ_ENABLE_LTO_IF_NEEDED TARGET)
    if(ENABLE_LTO)
        set_property(TARGET ${TARGET} PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    endif()
endfunction()
