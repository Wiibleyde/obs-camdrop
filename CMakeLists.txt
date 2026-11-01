cmake_minimum_required(VERSION 3.28...3.31)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" ON)
option(ENABLE_QT "Use Qt functionality" ON)

include(compilerconfig)
include(defaults)
include(helpers)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake/finders")

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)
if(TARGET plugin-support)
  target_link_libraries(plugin-support PRIVATE OBS::libobs)
endif()

if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
  target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE CAMDROP_HAVE_FRONTEND)
endif()

if(ENABLE_QT)
  find_package(Qt6 COMPONENTS Widgets Core Gui REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets Qt6::Gui)
  target_compile_options(
    ${CMAKE_PROJECT_NAME}
    PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
  )
  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
  )
  target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE CAMDROP_HAVE_QT)
  target_sources(
    ${CMAKE_PROJECT_NAME}
    PRIVATE src/ui/settings-dialog.cpp src/ui/settings-dialog.hpp src/ui/dock.cpp src/ui/dock.hpp
  )
endif()

# Dependencies shipped by obs-deps (same versions as the ones bundled with OBS itself)
find_package(LibDataChannel REQUIRED)
find_package(MbedTLS REQUIRED)
find_package(qrcodegencpp REQUIRED)
find_package(FFmpeg REQUIRED COMPONENTS avcodec avutil)
find_package(nlohmann_json QUIET)

target_link_libraries(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    LibDataChannel::LibDataChannel
    MbedTLS::mbedtls
    MbedTLS::mbedx509
    MbedTLS::mbedcrypto
    qrcodegencpp::qrcodegencpp
    FFmpeg::avcodec
    FFmpeg::avutil
)

if(TARGET nlohmann_json::nlohmann_json)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE nlohmann_json::nlohmann_json)
endif()

if(OS_WINDOWS)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE ws2_32 iphlpapi bcrypt)
  target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_sources(${CMAKE_PROJECT_NAME} PRIVATE src/platform/platform-windows.cpp)
else()
  target_sources(${CMAKE_PROJECT_NAME} PRIVATE src/platform/platform-posix.cpp)
endif()

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    src/plugin-main.cpp
    src/plugin-support.h
    src/settings.cpp
    src/settings.hpp
    src/util/json.hpp
    src/util/random.cpp
    src/util/random.hpp
    src/util/strings.cpp
    src/util/strings.hpp
    src/platform/platform.hpp
    src/net/certificate.cpp
    src/net/certificate.hpp
    src/net/http.cpp
    src/net/http.hpp
    src/net/websocket.cpp
    src/net/websocket.hpp
    src/net/server.cpp
    src/net/server.hpp
    src/net/rate-limiter.cpp
    src/net/rate-limiter.hpp
    src/core/app.cpp
    src/core/app.hpp
    src/core/camera-session.cpp
    src/core/camera-session.hpp
    src/core/channel.cpp
    src/core/channel.hpp
    src/core/client-manager.cpp
    src/core/client-manager.hpp
    src/core/sdp-utils.cpp
    src/core/sdp-utils.hpp
    src/media/rtp.hpp
    src/media/rtp.cpp
    src/media/receiver-handler.cpp
    src/media/receiver-handler.hpp
    src/media/video-receiver.cpp
    src/media/video-receiver.hpp
    src/media/audio-receiver.cpp
    src/media/audio-receiver.hpp
    src/media/clock-sync.cpp
    src/media/clock-sync.hpp
    src/media/video-decoder.cpp
    src/media/video-decoder.hpp
    src/media/audio-decoder.cpp
    src/media/audio-decoder.hpp
    src/media/media-pipeline.cpp
    src/media/media-pipeline.hpp
    src/obs/camdrop-source.cpp
    src/obs/camdrop-source.hpp
)

set_target_properties(${CMAKE_PROJECT_NAME} PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
if(MSVC)
  target_compile_options(${CMAKE_PROJECT_NAME} PRIVATE /Zc:char8_t-)
endif()

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})
