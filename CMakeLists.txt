cmake_minimum_required(VERSION 3.16)

project(yt-dlpp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find Dependencies
find_package(Boost REQUIRED COMPONENTS program_options filesystem url system
                                       charconv)
find_package(fmt CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(qjs CONFIG REQUIRED)
find_package(FFMPEG REQUIRED COMPONENTS avcodec avformat swresample swscale)

add_subdirectory(3rdparty/certify)

# Basic definitions
add_library(
  yt-dlpp-lib STATIC
  src/ytdlpp.cpp
  src/error.cpp
  src/net/http_client.cpp
  src/scripting/quickjs_engine.cpp
  src/youtube/innertube.cpp
  src/youtube/extractor.cpp
  src/youtube/player_script.cpp
  src/youtube/decipher.cpp
  src/downloader/downloader.cpp
  src/media/muxer.cpp)

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

if(MSVC)
  add_compile_options(/utf-8)
endif()

target_include_directories(
  yt-dlpp-lib
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
         $<INSTALL_INTERFACE:include>)

target_link_libraries(
  yt-dlpp-lib
  PUBLIC Boost::system
  PRIVATE Boost::program_options
          Boost::filesystem
          Boost::url
          Boost::charconv
          fmt::fmt
          nlohmann_json::nlohmann_json
          OpenSSL::SSL
          OpenSSL::Crypto
          spdlog::spdlog_header_only
          qjs
          certify::core
          ${FFMPEG_LIBRARIES})

add_executable(simple_download examples/simple_download.cpp)
target_link_libraries(simple_download PRIVATE yt-dlpp-lib)

target_include_directories(yt-dlpp-lib PRIVATE ${FFMPEG_INCLUDE_DIRS})

add_executable(yt-dlpp-cli src/main.cpp)

target_link_libraries(yt-dlpp-cli PRIVATE yt-dlpp-lib fmt::fmt)

# Enable all warnings
if(MSVC)
  target_compile_options(yt-dlpp-lib PRIVATE /W4)
  target_compile_options(yt-dlpp-cli PRIVATE /W4)
else()
  target_compile_options(yt-dlpp PRIVATE -Wall -Wextra -Wpedantic)
endif()
