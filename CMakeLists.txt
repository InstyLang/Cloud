cmake_minimum_required(VERSION 3.16)

project(InstyCloud CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
enable_testing()

# The registry server (cloud-server) needs libpq + libevent, which are Unix-only
# and heavier than what the `cloud` CLI itself requires (curl + zlib + a crypto
# backend). Keep it opt-in so the CLI can be built/installed without pulling in
# Postgres/libevent. Enable with -DINSTY_BUILD_SERVER=ON.
option(INSTY_BUILD_SERVER "Build the cloud-server registry (needs libpq + libevent)" OFF)

# Debug-friendly flags only on compilers that understand them (GCC/Clang).
if(NOT MSVC)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -ggdb -O0")
endif()

# ---------------------------------------------------------------------------
# Dependencies
#
# The `cloud` CLI needs libcurl and zlib (plus a SHA/HMAC/RNG crypto backend:
# OpenSSL on Unix, Windows CNG/bcrypt on Windows). The registry server
# additionally needs libpq and libevent and is therefore only built on
# non-Windows hosts (its dependencies are Unix-oriented).
#
# On Windows, dependencies come from vcpkg (find_package / config packages):
#   vcpkg install curl zlib --triplet x64-windows
# and the vcpkg toolchain file is passed at configure time.
# On Unix we keep the original pkg-config based discovery.
# ---------------------------------------------------------------------------

if(WIN32)
    find_package(CURL CONFIG REQUIRED)
    find_package(ZLIB REQUIRED)

    add_library(InstyCloudCore STATIC
        src/cloudServer/core.cpp
        include/cloudServer/core.hpp
    )
    target_include_directories(InstyCloudCore PUBLIC include)
    # bcrypt provides SHA256 / HMAC-SHA256 / secure RNG via CNG (no OpenSSL).
    target_link_libraries(InstyCloudCore PUBLIC ZLIB::ZLIB bcrypt)

    add_executable(cloud src/cloud.cpp)
    target_link_libraries(cloud PRIVATE InstyCloudCore CURL::libcurl ZLIB::ZLIB)
else()
    find_package(PkgConfig REQUIRED)
    # The `cloud` CLI only needs libcurl (+ zlib + OpenSSL via InstyCloudCore).
    pkg_check_modules(LIBCURL REQUIRED IMPORTED_TARGET libcurl)
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

    # Package manager / registry client CLI
    add_executable(cloud src/cloud.cpp)
    target_link_libraries(cloud PRIVATE InstyCloudCore PkgConfig::LIBCURL)

    # Package registry server (Unix only). Opt-in via -DINSTY_BUILD_SERVER=ON
    # because it pulls in libpq + libevent.
    if(INSTY_BUILD_SERVER)
        pkg_check_modules(LIBPQ REQUIRED IMPORTED_TARGET libpq)
        pkg_check_modules(LIBEVENT REQUIRED IMPORTED_TARGET libevent)

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

        add_executable(cloud-server src/cloudServer/main.cpp)
        target_link_libraries(cloud-server PRIVATE InstyCloudServer)

        add_executable(cloud-server-tests tests/cloudServerTests.cpp)
        target_link_libraries(cloud-server-tests PRIVATE InstyCloudServer)
        add_test(NAME cloud-server-tests COMMAND cloud-server-tests)
    endif()
endif()
