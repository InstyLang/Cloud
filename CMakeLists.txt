cmake_minimum_required(VERSION 3.10)

project(InstyCloud)
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

add_library(InstyCloudCore STATIC
    src/cloudServer/core.cpp
    include/cloudServer/core.hpp
)

target_include_directories(InstyCloudCore PUBLIC include)
target_link_libraries(InstyCloudCore
    PUBLIC
        PkgConfig::OPENSSL
        PkgConfig::ZLIB
)

add_library(InstyCloudServer STATIC
    src/cloudServer/service.cpp
    include/cloudServer/service.hpp
)

target_include_directories(InstyCloudServer PUBLIC include)
target_link_libraries(InstyCloudServer
    PUBLIC
        InstyCloudCore
        PkgConfig::LIBPQ
        PkgConfig::LIBCURL
        PkgConfig::LIBEVENT
)

# Package manager / registry client CLI
add_executable(cloud src/cloud.cpp)
target_link_libraries(cloud PRIVATE InstyCloudCore PkgConfig::LIBCURL)

# Package registry server
add_executable(cloud-server src/cloudServer/main.cpp)
target_link_libraries(cloud-server PRIVATE InstyCloudServer)

add_executable(cloud-server-tests tests/cloudServerTests.cpp)
target_link_libraries(cloud-server-tests PRIVATE InstyCloudServer)
add_test(NAME cloud-server-tests COMMAND cloud-server-tests)
