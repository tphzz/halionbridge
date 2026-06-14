if(APPLE)
    if(DEFINED CMAKE_OSX_DEPLOYMENT_TARGET AND NOT CMAKE_OSX_DEPLOYMENT_TARGET VERSION_LESS "10.15")
        set(HALIONBRIDGE_SFIZZ_OSX_DEPLOYMENT_TARGET "${CMAKE_OSX_DEPLOYMENT_TARGET}")
    else()
        set(HALIONBRIDGE_SFIZZ_OSX_DEPLOYMENT_TARGET "10.15")
    endif()
endif()

if(DEFINED CMAKE_CXX_STANDARD AND NOT CMAKE_CXX_STANDARD VERSION_LESS "23")
    set(HALIONBRIDGE_SFIZZ_CXX_STANDARD "${CMAKE_CXX_STANDARD}")
else()
    set(HALIONBRIDGE_SFIZZ_CXX_STANDARD "23")
endif()

set(CMAKE_CXX_STANDARD "${HALIONBRIDGE_SFIZZ_CXX_STANDARD}"
    CACHE STRING "C++ standard for sfizz in halionbridge builds." FORCE)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include("${CMAKE_SOURCE_DIR}/external/sfizz/cmake/SfizzConfig.cmake")

set(CMAKE_CXX_STANDARD "${HALIONBRIDGE_SFIZZ_CXX_STANDARD}"
    CACHE STRING "C++ standard for sfizz in halionbridge builds." FORCE)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(APPLE)
    # sfizz defaults to 10.13, which makes ghc/fs_std.hpp expose ghc::filesystem.
    # halionbridge's C++23 code uses std::filesystem, available from macOS 10.15.
    # The chosen deployment target and C++ standard are part of sfizz's exported
    # C++ ABI because sfizz headers expose fs::path and absl::optional in public
    # member functions.
    set(CMAKE_OSX_DEPLOYMENT_TARGET "${HALIONBRIDGE_SFIZZ_OSX_DEPLOYMENT_TARGET}"
        CACHE STRING "Minimum macOS version for sfizz in halionbridge builds." FORCE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "${HALIONBRIDGE_SFIZZ_OSX_DEPLOYMENT_TARGET}")
endif()
