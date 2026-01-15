cmake_minimum_required(VERSION 3.16)

project(
    yt-dlpp
    VERSION 0.1.0
    LANGUAGES CXX
    DESCRIPTION "A C++ YouTube video/audio downloader library"
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# =============================================================================
# OPTIONS
# =============================================================================

# Determine if we're the top-level project
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(YTDLPP_IS_TOP_LEVEL ON)
else()
    set(YTDLPP_IS_TOP_LEVEL OFF)
endif()

option(YTDLPP_BUILD_EXAMPLES "Build example applications"
       ${YTDLPP_IS_TOP_LEVEL}
)
option(YTDLPP_BUILD_CLI "Build the yt-dlpp command-line tool"
       ${YTDLPP_IS_TOP_LEVEL}
)
option(YTDLPP_INSTALL "Generate install targets" ${YTDLPP_IS_TOP_LEVEL})
option(YTDLPP_ENABLE_LTO "Enable Link-Time Optimization for Release builds" ON)
option(YTDLPP_STRIP_SYMBOLS "Strip debug symbols in Release builds" ON)
option(YTDLPP_FAST_MATH "Enable fast math optimizations (may affect precision)"
       OFF
)

# =============================================================================
# BUILD TYPE
# =============================================================================

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    message(STATUS "Setting build type to 'Release' as none was specified.")
    set(CMAKE_BUILD_TYPE
        "Release"
        CACHE STRING "Choose the type of build." FORCE
    )
    set_property(
        CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release" "MinSizeRel"
                                        "RelWithDebInfo"
    )
endif()

# =============================================================================
# LTO CONFIGURATION
# =============================================================================

if(YTDLPP_ENABLE_LTO AND CMAKE_BUILD_TYPE MATCHES "Release|RelWithDebInfo")
    include(CheckIPOSupported)
    check_ipo_supported(RESULT LTO_SUPPORTED OUTPUT LTO_ERROR)
    if(LTO_SUPPORTED)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
    else()
        message(WARNING "LTO not supported: ${LTO_ERROR}")
    endif()
endif()

# =============================================================================
# DEPENDENCIES
# =============================================================================

# -----------------------------------------------------------------------------
# Boost Configuration
# -----------------------------------------------------------------------------
option(YTDLPP_USE_SYSTEM_BOOST "Use system-installed Boost instead of vcpkg"
       OFF
)

if(YTDLPP_USE_SYSTEM_BOOST)
    set(Boost_USE_STATIC_LIBS OFF)
    set(Boost_USE_MULTITHREADED ON)
    find_package(
        Boost 1.86 REQUIRED COMPONENTS filesystem program_options regex
    )
    message(STATUS "Using system Boost: ${Boost_VERSION}")
else()
    find_package(
        Boost REQUIRED
        COMPONENTS asio
                   charconv
                   program_options
                   regex
                   scope_exit
                   url
                   CONFIG
    )
    message(STATUS "Using vcpkg Boost: ${Boost_VERSION}")
endif()

find_package(certify CONFIG REQUIRED)
find_package(fmt CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(utf8cpp CONFIG REQUIRED)
find_package(ZLIB REQUIRED)
find_package(v8 CONFIG REQUIRED) # V8 Dependency

# -----------------------------------------------------------------------------
# FFmpeg Configuration
# -----------------------------------------------------------------------------
option(YTDLPP_USE_SYSTEM_FFMPEG "Use system-installed FFmpeg instead of vcpkg"
       OFF
)

if(YTDLPP_USE_SYSTEM_FFMPEG)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(
        LIBAV
        REQUIRED
        IMPORTED_TARGET
        libavcodec
        libavformat
        libswresample
        libswscale
        libavutil
    )

    set(FFMPEG_LIBRARIES PkgConfig::LIBAV)
    set(FFMPEG_INCLUDE_DIRS ${LIBAV_INCLUDE_DIRS})
    message(STATUS "Using system FFmpeg: ${LIBAV_LINK_LIBRARIES}")
else()
    find_package(FFMPEG REQUIRED COMPONENTS avcodec avformat swresample swscale)
    message(STATUS "Using vcpkg FFmpeg: ${FFMPEG_LIBRARIES}")
endif()

# =============================================================================
# LIBRARY TARGET
# =============================================================================

add_library(
    yt-dlpp-lib
    src/error.cpp
    src/net/http_client.cpp
    src/scripting/js_engine_v8.cpp # V8 Implementation
    src/scripting/native_js_solver.cpp
    src/ejs_solver.cpp
    src/youtube/innertube.cpp
    src/youtube/extractor.cpp
    src/youtube/player_script.cpp
    src/youtube/decipher.cpp
    src/downloader/downloader.cpp
    src/media/muxer.cpp
    src/media/audio_streamer.cpp
)

add_library(ytdlpp::ytdlpp ALIAS yt-dlpp-lib)

# -----------------------------------------------------------------------------
# Symbol Visibility (for shared library builds)
# -----------------------------------------------------------------------------

include(GenerateExportHeader)

generate_export_header(
    yt-dlpp-lib
    BASE_NAME
    YTDLPP
    EXPORT_FILE_NAME
    ${CMAKE_CURRENT_BINARY_DIR}/include/ytdlpp/ytdlpp_export.h
    EXPORT_MACRO_NAME
    YTDLPP_EXPORT
    STATIC_DEFINE
    YTDLPP_STATIC_DEFINE
)

set_target_properties(
    yt-dlpp-lib
    PROPERTIES CXX_VISIBILITY_PRESET hidden
               VISIBILITY_INLINES_HIDDEN ON
               VERSION ${PROJECT_VERSION}
               SOVERSION ${PROJECT_VERSION_MAJOR}
)

# -----------------------------------------------------------------------------
# Include Directories
# -----------------------------------------------------------------------------

target_include_directories(
    yt-dlpp-lib
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
           $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
           $<INSTALL_INTERFACE:include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${FFMPEG_INCLUDE_DIRS}
)

# -----------------------------------------------------------------------------
# Compile Definitions
# -----------------------------------------------------------------------------

target_compile_definitions(yt-dlpp-lib PUBLIC BOOST_ASIO_NO_DEPRECATED)

if(MSVC)
    target_compile_definitions(
        yt-dlpp-lib
        PUBLIC _WIN32_WINNT=0x0601
               _SCL_SECURE_NO_WARNINGS=1
               _CRT_SECURE_NO_WARNINGS=1
               _SILENCE_CXX17_ALLOCATOR_VOID_DEPRECATION_WARNING
               _SILENCE_CXX17_ADAPTOR_TYPEDEFS_DEPRECATION_WARNING
    )
endif()

# -----------------------------------------------------------------------------
# Compiler Flags
# -----------------------------------------------------------------------------

if(MSVC)
    target_compile_options(yt-dlpp-lib PRIVATE /W4 /utf-8 /bigobj)
    if(CMAKE_BUILD_TYPE MATCHES "Release|RelWithDebInfo")
        target_compile_options(yt-dlpp-lib PRIVATE /O2 /Ob2 /Oi /Ot /GL)
        target_link_options(yt-dlpp-lib PRIVATE /LTCG /OPT:REF /OPT:ICF)
    endif()
else()
    target_compile_options(yt-dlpp-lib PRIVATE -Wall -Wextra -Wpedantic)
    if(CMAKE_BUILD_TYPE MATCHES "Release|RelWithDebInfo")
        target_compile_options(
            yt-dlpp-lib PRIVATE -O3 -ffunction-sections -fdata-sections
        )
        target_link_options(yt-dlpp-lib PRIVATE -Wl,--gc-sections)

        if(YTDLPP_FAST_MATH)
            target_compile_options(yt-dlpp-lib PRIVATE -ffast-math)
        endif()

        # Only use -march=native when NOT cross-compiling (host == target)
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm|aarch64|ARM|ARM64"
           AND NOT CMAKE_CROSSCOMPILING
        )
            target_compile_options(
                yt-dlpp-lib PRIVATE -march=native -mtune=native
            )
        endif()

        if(YTDLPP_STRIP_SYMBOLS AND CMAKE_BUILD_TYPE STREQUAL "Release")
            target_link_options(yt-dlpp-lib PRIVATE -s)
        endif()
    endif()
endif()

# -----------------------------------------------------------------------------
# Link Libraries
# -----------------------------------------------------------------------------

if(YTDLPP_USE_SYSTEM_BOOST)
    # System Boost
    target_include_directories(yt-dlpp-lib PUBLIC ${Boost_INCLUDE_DIRS})
    target_link_libraries(
        yt-dlpp-lib
        PUBLIC Boost::regex
        PRIVATE Boost::program_options
                fmt::fmt
                nlohmann_json::nlohmann_json
                spdlog::spdlog
                spdlog::spdlog
                utf8cpp::utf8cpp
                $<$<PLATFORM_ID:Linux>:-Wl,--start-group>
                v8::v8
                $<$<PLATFORM_ID:Linux>:-Wl,--end-group>
                certify::core
                certify::core
                ZLIB::ZLIB
                ${FFMPEG_LIBRARIES}
    )
else()
    # vcpkg Boost
    target_link_libraries(
        yt-dlpp-lib
        PUBLIC Boost::asio
        PRIVATE Boost::url
                Boost::charconv
                Boost::regex
                Boost::scope_exit
                fmt::fmt
                nlohmann_json::nlohmann_json
                spdlog::spdlog
                spdlog::spdlog
                utf8cpp::utf8cpp
                $<$<PLATFORM_ID:Linux>:-Wl,--start-group>
                v8::v8
                $<$<PLATFORM_ID:Linux>:-Wl,--end-group>
                certify::core
                certify::core
                ZLIB::ZLIB
                ${FFMPEG_LIBRARIES}
    )
endif()

if(UNIX AND NOT APPLE)
    target_link_libraries(
        yt-dlpp-lib PRIVATE "-Wl,--start-group" v8::v8 "-Wl,--end-group"
    )
else()
    target_link_libraries(yt-dlpp-lib PRIVATE v8::v8)
endif()

# =============================================================================
# CLI EXECUTABLE
# =============================================================================

if(YTDLPP_BUILD_CLI)
    add_executable(yt-dlpp src/main.cpp)
    target_include_directories(yt-dlpp PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(
        yt-dlpp PRIVATE yt-dlpp-lib fmt::fmt spdlog::spdlog
                        Boost::program_options
    )

    if(MSVC)
        target_link_options(yt-dlpp PRIVATE /STACK:16777216) # 16MB stack
    endif()

    if(MSVC)
        target_compile_options(yt-dlpp PRIVATE /W4 /bigobj)
        if(CMAKE_BUILD_TYPE MATCHES "Release|RelWithDebInfo")
            target_compile_options(yt-dlpp PRIVATE /O2 /Ob2 /Oi /Ot /GL)
        endif()
    else()
        if(CMAKE_BUILD_TYPE MATCHES "Release|RelWithDebInfo")
            target_compile_options(
                yt-dlpp PRIVATE -O3 -ffunction-sections -fdata-sections
            )
            target_link_options(yt-dlpp PRIVATE -Wl,--gc-sections)
            if(YTDLPP_STRIP_SYMBOLS AND CMAKE_BUILD_TYPE STREQUAL "Release")
                target_link_options(yt-dlpp PRIVATE -s)
            endif()
        endif()
    endif()
endif()

if(YTDLPP_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# =============================================================================
# BUILD SUMMARY
# =============================================================================

message(STATUS "")
message(STATUS "=== yt-dlpp ${PROJECT_VERSION} Build Configuration ===")
message(STATUS "  Build type:        ${CMAKE_BUILD_TYPE}")
message(STATUS "  Build CLI:         ${YTDLPP_BUILD_CLI}")
message(STATUS "  Build examples:    ${YTDLPP_BUILD_EXAMPLES}")
message(STATUS "  Install:           ${YTDLPP_INSTALL}")
message(STATUS "  LTO enabled:       ${CMAKE_INTERPROCEDURAL_OPTIMIZATION}")
message(STATUS "  Strip symbols:     ${YTDLPP_STRIP_SYMBOLS}")
if(YTDLPP_USE_SYSTEM_BOOST)
    message(STATUS "  Boost:             system")
else()
    message(STATUS "  Boost:             vcpkg")
endif()
if(YTDLPP_USE_SYSTEM_FFMPEG)
    message(STATUS "  FFmpeg:            system (pkg-config)")
else()
    message(STATUS "  FFmpeg:            vcpkg")
endif()
message(STATUS "  System processor:  ${CMAKE_SYSTEM_PROCESSOR}")
message(STATUS "")

# =============================================================================
# INSTALLATION
# =============================================================================

if(YTDLPP_INSTALL)
    include(GNUInstallDirs)
    include(CMakePackageConfigHelpers)

    # Install library
    install(
        TARGETS yt-dlpp-lib
        EXPORT ytdlpp-targets
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        INCLUDES
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )

    # Install headers
    install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

    # Install generated export header
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/include/ytdlpp/ytdlpp_export.h
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/ytdlpp
    )

    # Install CLI executable
    if(YTDLPP_BUILD_CLI)
        install(TARGETS yt-dlpp RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    endif()

    # Export targets
    install(
        EXPORT ytdlpp-targets
        FILE ytdlpp-targets.cmake
        NAMESPACE ytdlpp::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ytdlpp
    )

    # Generate and install config files
    configure_package_config_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/ytdlpp-config.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/ytdlpp-config.cmake
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ytdlpp
    )

    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/ytdlpp-config-version.cmake
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion
    )

    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/ytdlpp-config.cmake
                  ${CMAKE_CURRENT_BINARY_DIR}/ytdlpp-config-version.cmake
            DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ytdlpp
    )
endif()
