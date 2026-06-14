include("${CMAKE_SOURCE_DIR}/external/sfizz/cmake/SfizzConfig.cmake")

if(APPLE)
    # sfizz defaults to 10.13, which makes ghc/fs_std.hpp expose ghc::filesystem.
    # halionbridge's C++23 code uses std::filesystem, available from macOS 10.15.
    set(CMAKE_OSX_DEPLOYMENT_TARGET "10.15")
endif()
