cmake_minimum_required(VERSION 3.16)

project(yt-dlpp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# =============================================================================
# BUILD TYPE CONFIGURATION
# =============================================================================
# Provide sensible defaults for CMAKE_BUILD_TYPE if not set
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  message(STATUS "Setting build type to 'Release' as none was specified.")
  set(CMAKE_BUILD_TYPE
      "Release"
      CACHE STRING "Choose the type of build." FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release"
                                               "MinSizeRel" "RelWithDebInfo")
endif()

# =============================================================================
# COMPILE-TIME OPTIMIZATIONS
# =============================================================================
# These settings improve runtime performance and reduce binary size. - LTO:
# Enables whole-program optimization across translation units - Section GC:
# Removes unused code/data sections - Fast math: Relaxed floating-point for
# better ARM performance
# =============================================================================

option(YTDLPP_ENABLE_LTO "Enable Link-Time Optimization for Release builds" ON)
option(YTDLPP_STRIP_SYMBOLS "Strip debug symbols in Release builds" ON)
option(YTDLPP_FAST_MATH "Enable fast math optimizations (may affect precision)"
       OFF)

# Enable LTO for Release builds
if(YTDLPP_ENABLE_LTO AND CMAKE_BUILD_TYPE MATCHES "Release|RelWithDebInfo")
  include(CheckIPOSupported)
  check_ipo_supported(RESULT LTO_SUPPORTED OUTPUT LTO_ERROR)
  if(LTO_SUPPORTED)
    message(STATUS "LTO enabled for yt-dlpp")
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
  else()
    message(WARNING "LTO not supported: ${LTO_ERROR}")
  endif()
endif()

# Find Dependencies
find_package(boost_asio CONFIG REQUIRED)
find_package(boost_charconv CONFIG REQUIRED)
find_package(boost_program_options CONFIG REQUIRED)
find_package(boost_regex CONFIG REQUIRED)
find_package(boost_scope_exit CONFIG REQUIRED)
find_package(boost_url CONFIG REQUIRED)

find_package(fmt CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(qjs CONFIG REQUIRED)
find_package(ZLIB REQUIRED)
find_package(FFMPEG REQUIRED COMPONENTS avcodec avformat swresample swscale)

add_subdirectory(3rdparty/certify)

# Basic definitions
add_library(
  yt-dlpp-lib STATIC
  src/error.cpp
  src/net/http_client.cpp
  src/scripting/quickjs_engine.cpp
  src/youtube/innertube.cpp
  src/youtube/extractor.cpp
  src/youtube/player_script.cpp
  src/youtube/decipher.cpp
  src/downloader/downloader.cpp
  src/media/muxer.cpp
  src/media/audio_streamer.cpp)

target_compile_definitions(yt-dlpp-lib PUBLIC BOOST_ASIO_NO_DEPRECATED)

if(MSVC)
  target_compile_definitions(
    yt-dlpp-lib
    PUBLIC -D_WIN32_WINNT=0x0601
           -D_SCL_SECURE_NO_WARNINGS=1
           -D_CRT_SECURE_NO_WARNINGS=1
           -D_SILENCE_CXX17_ALLOCATOR_VOID_DEPRECATION_WARNING
           -D_SILENCE_CXX17_ADAPTOR_TYPEDEFS_DEPRECATION_WARNING)
endif()

# =============================================================================
# PLATFORM-SPECIFIC COMPILER FLAGS
# =============================================================================

if(MSVC)
  # MSVC optimizations
  add_compile_options(/utf-8 /bigobj)

  # Release-specific optimizations
  if(CMAKE_BUILD_TYPE MATCHES "Release|RelWithDebInfo")
    target_compile_options(
      yt-dlpp-lib
      PRIVATE /O2 # Maximum optimization
              /Ob2 # Inline any suitable function
              /Oi # Enable intrinsic functions
              /Ot # Favor fast code
              /GL # Whole program optimization (works with LTO)
    )
    # Linker optimizations for executables
    set(CMAKE_EXE_LINKER_FLAGS_RELEASE
        "${CMAKE_EXE_LINKER_FLAGS_RELEASE} /LTCG /OPT:REF /OPT:ICF")
  endif()
else()
  # GCC/Clang optimizations
  target_compile_options(yt-dlpp-lib PRIVATE -Wall -Wextra -Wpedantic)

  if(CMAKE_BUILD_TYPE MATCHES "Release|RelWithDebInfo")
    target_compile_options(
      yt-dlpp-lib
      PRIVATE -O3 # Maximum optimization
              -ffunction-sections # Put each function in its own section
              -fdata-sections # Put each data item in its own section
    )

    # Fast math (optional, can affect precision)
    if(YTDLPP_FAST_MATH)
      target_compile_options(yt-dlpp-lib PRIVATE -ffast-math)
    endif()

    # Linker optimizations - garbage collect unused sections
    target_link_options(yt-dlpp-lib PRIVATE -Wl,--gc-sections)

    # ARM-specific optimizations
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm|aarch64|ARM|ARM64")
      message(STATUS "ARM target detected - enabling ARM optimizations")
      target_compile_options(
        yt-dlpp-lib PRIVATE -march=native # Optimize for local CPU
                            -mtune=native # Tune for local CPU
      )
    endif()

    # Strip symbols in Release
    if(YTDLPP_STRIP_SYMBOLS AND CMAKE_BUILD_TYPE STREQUAL "Release")
      target_link_options(yt-dlpp-lib PRIVATE -s)
    endif()
  endif()
endif()

target_include_directories(
  yt-dlpp-lib
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
         $<INSTALL_INTERFACE:include>)

target_link_libraries(
  yt-dlpp-lib
  PUBLIC Boost::asio
  PRIVATE Boost::program_options
          Boost::url
          Boost::charconv
          fmt::fmt
          nlohmann_json::nlohmann_json
          OpenSSL::SSL
          OpenSSL::Crypto
          spdlog::spdlog_header_only
          qjs
          certify::core
          ZLIB::ZLIB
          Boost::regex
          ${FFMPEG_LIBRARIES}
          Boost::scope_exit)

target_include_directories(yt-dlpp-lib PRIVATE ${FFMPEG_INCLUDE_DIRS})

add_subdirectory(examples)

add_executable(yt-dlpp src/main.cpp)

target_link_libraries(yt-dlpp PRIVATE yt-dlpp-lib fmt::fmt)

# =============================================================================
# EXECUTABLE-SPECIFIC OPTIMIZATIONS
# =============================================================================

if(MSVC)
  target_compile_options(yt-dlpp-lib PRIVATE /W4 /bigobj)
  target_compile_options(yt-dlpp PRIVATE /W4 /bigobj)

  if(CMAKE_BUILD_TYPE MATCHES "Release|RelWithDebInfo")
    target_compile_options(yt-dlpp PRIVATE /O2 /Ob2 /Oi /Ot /GL)
  endif()
else()
  if(CMAKE_BUILD_TYPE MATCHES "Release|RelWithDebInfo")
    target_compile_options(yt-dlpp PRIVATE -O3 -ffunction-sections
                                           -fdata-sections)
    target_link_options(yt-dlpp PRIVATE -Wl,--gc-sections)

    if(YTDLPP_STRIP_SYMBOLS AND CMAKE_BUILD_TYPE STREQUAL "Release")
      target_link_options(yt-dlpp PRIVATE -s)
    endif()
  endif()
endif()

# =============================================================================
# BUILD SUMMARY
# =============================================================================

message(STATUS "=== yt-dlpp Build Configuration ===")
message(STATUS "  Build type:        ${CMAKE_BUILD_TYPE}")
message(STATUS "  LTO enabled:       ${CMAKE_INTERPROCEDURAL_OPTIMIZATION}")
message(STATUS "  Strip symbols:     ${YTDLPP_STRIP_SYMBOLS}")
message(STATUS "  Fast math:         ${YTDLPP_FAST_MATH}")
message(STATUS "  System processor:  ${CMAKE_SYSTEM_PROCESSOR}")
