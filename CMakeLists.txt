cmake_minimum_required(VERSION 3.16)
project(YT-Converter
    VERSION 1.0.0
    DESCRIPTION "YouTube to MP3/MP4/WAV Converter with CLI and REST API"
    LANGUAGES CXX
)

# Set C++ Standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Set default build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Project-wide compiler flags
if(MSVC)
    add_compile_options(/W4 /WX)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
    if(CMAKE_BUILD_TYPE MATCHES Debug)
        add_compile_options(-g -O0)
    else()
        add_compile_options(-O3)
    endif()
endif()

# Find required packages
find_package(cpprestsdk REQUIRED CONFIG)
find_package(Boost REQUIRED COMPONENTS system)
find_package(OpenSSL REQUIRED)

# Optional: Enable testing
option(BUILD_TESTS "Build unit tests" OFF)
if(BUILD_TESTS)
    enable_testing()
    find_package(GTest REQUIRED)
endif()

# Include directories
set(PROJECT_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include)
set(PROJECT_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src)

include_directories(${PROJECT_INCLUDE_DIR})
include_directories(${Boost_INCLUDE_DIRS})
include_directories(${OPENSSL_INCLUDE_DIR})

# Core Library - Shared converter, validation, and logger functionality
set(CORE_SOURCES
    ${PROJECT_SOURCE_DIR}/core/converter.cpp
    ${PROJECT_SOURCE_DIR}/core/validator.cpp
    ${PROJECT_SOURCE_DIR}/core/logger.cpp
    ${PROJECT_SOURCE_DIR}/utils/file_utils.cpp
)

set(CORE_HEADERS
    ${PROJECT_INCLUDE_DIR}/core/converter.h
    ${PROJECT_INCLUDE_DIR}/core/validator.h
    ${PROJECT_INCLUDE_DIR}/core/logger.h
    ${PROJECT_INCLUDE_DIR}/utils/file_utils.h
)

# Check if source files exist, if not create a minimal library
if(EXISTS ${PROJECT_SOURCE_DIR}/core/converter.cpp)
    add_library(yt-converter-core STATIC ${CORE_SOURCES})
else()
    message(STATUS "Core source files not found, creating minimal core library")
    add_library(yt-converter-core INTERFACE)
endif()

# Link core library dependencies
if(TARGET yt-converter-core)
    target_link_libraries(yt-converter-core
        PUBLIC
            Boost::system
            OpenSSL::SSL
            OpenSSL::Crypto
    )

    target_include_directories(yt-converter-core
        PUBLIC
            $<BUILD_INTERFACE:${PROJECT_INCLUDE_DIR}>
            $<INSTALL_INTERFACE:include>
    )
endif()

# CLI Executable
set(CLI_SOURCES
    YT2MP3.cpp
)

# Only add CLI executable if the main file exists
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/YT2MP3.cpp)
    add_executable(yt2mp3-cli ${CLI_SOURCES})

    target_link_libraries(yt2mp3-cli
        PRIVATE
            yt-converter-core
            Boost::system
            OpenSSL::SSL
            OpenSSL::Crypto
    )

    target_include_directories(yt2mp3-cli
        PRIVATE
            ${PROJECT_INCLUDE_DIR}
    )

    # Installation
    install(TARGETS yt2mp3-cli
        RUNTIME DESTINATION bin
    )
endif()

# API Server Executable
set(API_SOURCES
    yt2mp3-API.cpp
)

# Only add API executable if the main file exists
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/yt2mp3-API.cpp)
    add_executable(yt2mp3-api ${API_SOURCES})

    target_link_libraries(yt2mp3-api
        PRIVATE
            yt-converter-core
            cpprestsdk::cpprest
            Boost::system
            OpenSSL::SSL
            OpenSSL::Crypto
    )

    target_include_directories(yt2mp3-api
        PRIVATE
            ${PROJECT_INCLUDE_DIR}
    )

    # Installation
    install(TARGETS yt2mp3-api
        RUNTIME DESTINATION bin
    )
endif()

# Unit Tests (optional)
if(BUILD_TESTS)
    set(TEST_SOURCES
        tests/test_validator.cpp
        tests/test_converter.cpp
    )

    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests)
        add_executable(yt-converter-tests ${TEST_SOURCES})

        target_link_libraries(yt-converter-tests
            PRIVATE
                yt-converter-core
                GTest::gtest
                GTest::gtest_main
        )

        target_include_directories(yt-converter-tests
            PRIVATE
                ${PROJECT_INCLUDE_DIR}
        )

        add_test(NAME yt-converter-tests COMMAND yt-converter-tests)
    endif()
endif()

# Installation targets
install(DIRECTORY ${PROJECT_INCLUDE_DIR}/
    DESTINATION include/yt-converter
)

# CMake config files for external projects
include(CMakePackageConfigHelpers)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/YT-ConverterConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY AnyNewerVersion
)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/YT-ConverterConfigVersion.cmake
    DESTINATION lib/cmake/YT-Converter
)

# Summary
message(STATUS "")
message(STATUS "========================================")
message(STATUS "YT-Converter Build Configuration Summary")
message(STATUS "========================================")
message(STATUS "Build Type: ${CMAKE_BUILD_TYPE}")
message(STATUS "C++ Standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "Install Prefix: ${CMAKE_INSTALL_PREFIX}")
message(STATUS "Build Tests: ${BUILD_TESTS}")
message(STATUS "")
message(STATUS "Packages Found:")
message(STATUS "  - cpprestsdk: YES")
message(STATUS "  - Boost: YES")
message(STATUS "  - OpenSSL: YES")
message(STATUS "")
message(STATUS "Targets:")
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/YT2MP3.cpp)
message(STATUS "  - yt2mp3-cli (CLI executable)")
endif()
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/yt2mp3-API.cpp)
message(STATUS "  - yt2mp3-api (REST API server)")
endif()
message(STATUS "  - yt-converter-core (Core library)")
message(STATUS "========================================")
message(STATUS "")
