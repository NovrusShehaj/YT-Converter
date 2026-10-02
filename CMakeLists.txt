cmake_minimum_required(VERSION 3.16)

if(POLICY CMP0167)
    cmake_policy(SET CMP0167 NEW)
endif()

project(YT-Converter
    VERSION 1.0.0
    DESCRIPTION "Local YouTube to MP3/MP4/WAV converter (CLI and localhost REST API)"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

option(BUILD_TESTS "Build unit tests" OFF)
option(BUILD_API "Build the localhost REST API (requires cpprestsdk)" ON)
option(YTCONV_WERROR "Treat compiler warnings as errors" OFF)
set(YTCONV_SANITIZE "" CACHE STRING
    "Comma-separated sanitizers for GCC/Clang builds, for example address,undefined or thread")

if(MSVC)
    add_compile_options(/W4 /WX)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
    if(YTCONV_WERROR)
        add_compile_options(-Werror)
    endif()
    if(CMAKE_BUILD_TYPE MATCHES Debug)
        add_compile_options(-g -O0)
    else()
        add_compile_options(-O3)
    endif()
    if(YTCONV_SANITIZE)
        add_compile_options(-fsanitize=${YTCONV_SANITIZE} -fno-omit-frame-pointer -g)
        add_link_options(-fsanitize=${YTCONV_SANITIZE})
    endif()
endif()

find_package(Threads REQUIRED)

if(BUILD_API)
    # An explicitly requested API build must not silently degrade to CLI-only.
    find_package(cpprestsdk CONFIG QUIET)
    if(NOT cpprestsdk_FOUND)
        message(FATAL_ERROR
            "BUILD_API=ON requires cpprestsdk, but its CMake package was not found.\n"
            "Install it (Debian/Ubuntu: apt-get install libcpprest-dev libboost-all-dev libssl-dev; "
            "macOS: brew install cpprestsdk boost openssl) or point CMAKE_PREFIX_PATH / "
            "cpprestsdk_DIR at an existing installation.\n"
            "For a CLI-only build, configure with -DBUILD_API=OFF.")
    endif()
    find_package(Boost REQUIRED)
    find_package(OpenSSL REQUIRED)
endif()

find_program(YT_DLP_EXECUTABLE NAMES yt-dlp)
find_program(FFMPEG_EXECUTABLE NAMES ffmpeg)
if(NOT YT_DLP_EXECUTABLE)
    message(WARNING "yt-dlp not found on PATH; conversions will fail until it is installed")
endif()
if(NOT FFMPEG_EXECUTABLE)
    message(WARNING "ffmpeg not found on PATH; conversions will fail until it is installed")
endif()

set(YTCONV_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include)
set(YTCONV_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src)
set(YTCONV_GENERATED_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${YTCONV_GENERATED_DIR})
configure_file(${YTCONV_INCLUDE_DIR}/version.h.in ${YTCONV_GENERATED_DIR}/version.h @ONLY)

set(CORE_SOURCES
    ${YTCONV_SRC_DIR}/core/converter.cpp
    ${YTCONV_SRC_DIR}/core/source_cache.cpp
    ${YTCONV_SRC_DIR}/utils/file_lock.cpp
    ${YTCONV_SRC_DIR}/utils/validation.cpp
    ${YTCONV_SRC_DIR}/utils/logger.cpp
    ${YTCONV_SRC_DIR}/utils/error.cpp
    ${YTCONV_SRC_DIR}/utils/config.cpp
    ${YTCONV_SRC_DIR}/utils/process.cpp
    ${YTCONV_SRC_DIR}/utils/job_limiter.cpp
    ${YTCONV_SRC_DIR}/utils/dependencies.cpp
    ${YTCONV_SRC_DIR}/utils/metrics.cpp
    ${YTCONV_SRC_DIR}/utils/job_queue.cpp
)

add_library(yt-converter-core STATIC ${CORE_SOURCES})
target_include_directories(yt-converter-core
    PUBLIC
        $<BUILD_INTERFACE:${YTCONV_INCLUDE_DIR}>
        $<BUILD_INTERFACE:${YTCONV_GENERATED_DIR}>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(yt-converter-core PUBLIC Threads::Threads)

add_executable(yt2mp3-cli ${YTCONV_SRC_DIR}/cli/main.cpp)
target_link_libraries(yt2mp3-cli PRIVATE yt-converter-core)
install(TARGETS yt2mp3-cli RUNTIME DESTINATION bin)

if(BUILD_API)
    add_library(yt-converter-api STATIC ${YTCONV_SRC_DIR}/api/api_app.cpp)
    target_link_libraries(yt-converter-api
        PUBLIC
            yt-converter-core
            cpprestsdk::cpprest
            Boost::boost
            OpenSSL::SSL
            OpenSSL::Crypto
    )
    add_executable(yt2mp3-api ${YTCONV_SRC_DIR}/api/server.cpp)
    target_link_libraries(yt2mp3-api PRIVATE yt-converter-api)
    install(TARGETS yt2mp3-api RUNTIME DESTINATION bin)
endif()

if(BUILD_TESTS)
    enable_testing()
    find_package(GTest QUIET)
    if(NOT GTest_FOUND)
        include(FetchContent)
        FetchContent_Declare(
            googletest
            URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.zip
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        )
        set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(googletest)
    endif()

    set(TEST_SOURCES
        tests/test_validation.cpp
        tests/test_process.cpp
        tests/test_converter.cpp
        tests/test_security_argv.cpp
        tests/test_error.cpp
        tests/test_config.cpp
        tests/test_logger.cpp
        tests/test_job_queue.cpp
        tests/test_source_cache.cpp
    )
    add_executable(yt-converter-tests ${TEST_SOURCES})
    target_link_libraries(yt-converter-tests
        PRIVATE
            yt-converter-core
            GTest::gtest
            GTest::gtest_main
    )
    # Compiled child fixture so process tests do not depend on shell scripts.
    add_executable(ytconv-output-writer tests/fixtures/output_writer.cpp)
    add_dependencies(yt-converter-tests ytconv-output-writer)
    target_compile_definitions(yt-converter-tests
        PRIVATE
            YTCONV_FAKE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fakes"
            YTCONV_OUTPUT_WRITER="$<TARGET_FILE:ytconv-output-writer>"
            YTCONV_CLI_PATH="$<TARGET_FILE:yt2mp3-cli>"
    )
    add_dependencies(yt-converter-tests yt2mp3-cli)

    file(GLOB YTCONV_FAKES ${CMAKE_CURRENT_SOURCE_DIR}/tests/fakes/*)
    foreach(fake ${YTCONV_FAKES})
        execute_process(COMMAND chmod +x "${fake}")
    endforeach()

    add_test(NAME yt-converter-tests COMMAND yt-converter-tests)

    # Real ffmpeg/ffprobe on locally generated media; skipped only when the tools are absent and
    # YTCONV_REQUIRE_MEDIA_TESTS is not 1.
    add_executable(yt-converter-media-tests tests/test_media.cpp)
    target_link_libraries(yt-converter-media-tests
        PRIVATE
            yt-converter-core
            GTest::gtest
            GTest::gtest_main
    )
    target_compile_definitions(yt-converter-media-tests
        PRIVATE
            YTCONV_FAKE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fakes"
    )
    add_test(NAME yt-converter-media-tests COMMAND yt-converter-media-tests)

    if(BUILD_API)
        # HTTP tests live in their own binary so CI can prove they were built and run.
        add_executable(yt-converter-api-tests tests/test_api.cpp)
        target_link_libraries(yt-converter-api-tests
            PRIVATE
                yt-converter-api
                GTest::gtest
                GTest::gtest_main
        )
        target_compile_definitions(yt-converter-api-tests
            PRIVATE
                YTCONV_FAKE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fakes"
        )
        add_test(NAME yt-converter-api-tests COMMAND yt-converter-api-tests)
    endif()
    add_test(NAME no_system_in_src COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_no_system.sh)
    add_test(NAME cli_help COMMAND yt2mp3-cli --help)
    add_test(NAME cli_version COMMAND yt2mp3-cli --version)
endif()

install(DIRECTORY ${YTCONV_INCLUDE_DIR}/
    DESTINATION include/yt-converter
    FILES_MATCHING PATTERN "*.h"
)
install(FILES ${YTCONV_GENERATED_DIR}/version.h DESTINATION include/yt-converter)

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

message(STATUS "")
message(STATUS "========================================")
message(STATUS "YT-Converter ${PROJECT_VERSION}")
message(STATUS "========================================")
message(STATUS "Build Type: ${CMAKE_BUILD_TYPE}")
message(STATUS "Build Tests: ${BUILD_TESTS}")
message(STATUS "Build API: ${BUILD_API}")
message(STATUS "yt-dlp: ${YT_DLP_EXECUTABLE}")
message(STATUS "ffmpeg: ${FFMPEG_EXECUTABLE}")
message(STATUS "========================================")
