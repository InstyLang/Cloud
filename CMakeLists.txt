cmake_minimum_required(VERSION 3.10)

project(EcliptixCloud)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_BUILD_TYPE Release)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
enable_testing()

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -ggdb -O0")

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBPQ REQUIRED IMPORTED_TARGET libpq)
pkg_check_modules(LIBCURL REQUIRED IMPORTED_TARGET libcurl)
pkg_check_modules(LIBEVENT REQUIRED IMPORTED_TARGET libevent)
pkg_check_modules(OPENSSL REQUIRED IMPORTED_TARGET openssl)
pkg_check_modules(ZLIB REQUIRED IMPORTED_TARGET zlib)

add_library(EcliptixCloudCore STATIC
    src/cloudServer/core.cpp
    include/cloudServer/core.hpp
)

target_include_directories(EcliptixCloudCore PUBLIC include)
target_link_libraries(EcliptixCloudCore
    PUBLIC
        PkgConfig::OPENSSL
        PkgConfig::ZLIB
)

add_library(EcliptixCloudServer STATIC
    src/cloudServer/service.cpp
    include/cloudServer/service.hpp
)

target_include_directories(EcliptixCloudServer PUBLIC include)
target_link_libraries(EcliptixCloudServer
    PUBLIC
        EcliptixCloudCore
        PkgConfig::LIBPQ
        PkgConfig::LIBCURL
        PkgConfig::LIBEVENT
)

# Package manager / registry client CLI
add_executable(cloud src/cloud.cpp)
target_link_libraries(cloud PRIVATE EcliptixCloudCore PkgConfig::LIBCURL)

# Package registry server
add_executable(cloud-server src/cloudServer/main.cpp)
target_link_libraries(cloud-server PRIVATE EcliptixCloudServer)

add_executable(cloud-server-tests tests/cloudServerTests.cpp)
target_link_libraries(cloud-server-tests PRIVATE EcliptixCloudCore)
add_test(NAME cloud-server-tests COMMAND cloud-server-tests)
