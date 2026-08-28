set(HALIONBRIDGE_DXSYX_SOURCES
    src/Dx7Sysex.cpp
)

add_library(halionbridge_dxsyx_static STATIC ${HALIONBRIDGE_DXSYX_SOURCES})
add_library(dxsyx::dxsyx ALIAS halionbridge_dxsyx_static)

target_compile_features(halionbridge_dxsyx_static PUBLIC cxx_std_23)
target_include_directories(halionbridge_dxsyx_static
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

enable_halionbridge_strict_warnings(${HALIONBRIDGE_DXSYX_SOURCES})
