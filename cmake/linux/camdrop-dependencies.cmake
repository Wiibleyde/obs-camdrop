# Camdrop dependencies on Linux
#
# OBS on Linux uses the distribution's libraries, which lack libdatachannel and ship mbedTLS 2.x
# (Camdrop needs 3.x). Both are built from source and linked statically, with their symbols kept
# private to the plugin so they cannot clash with the libraries OBS loads.

include_guard(GLOBAL)

include(FetchContent)

set(_camdrop_saved_warning_as_error ${CMAKE_COMPILE_WARNING_AS_ERROR})
set(CMAKE_COMPILE_WARNING_AS_ERROR OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# mbedTLS 3.6 (TLS 1.2 only and thread-safe, see mbedtls-user-config.h)
set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
set(USE_SHARED_MBEDTLS_LIBRARY OFF CACHE BOOL "" FORCE)
set(USE_STATIC_MBEDTLS_LIBRARY ON CACHE BOOL "" FORCE)
set(MBEDTLS_USER_CONFIG_FILE "${CMAKE_CURRENT_LIST_DIR}/mbedtls-user-config.h" CACHE FILEPATH "" FORCE)
FetchContent_Declare(
  mbedtls
  URL https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.4/mbedtls-3.6.4.tar.bz2
  URL_HASH SHA256=ec35b18a6c593cf98c3e30db8b98ff93e8940a8c4e690e66b41dfc011d678110
  EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(mbedtls)
foreach(_lib IN ITEMS mbedtls mbedx509 mbedcrypto)
  if(NOT TARGET MbedTLS::${_lib})
    add_library(MbedTLS::${_lib} ALIAS ${_lib})
  endif()
endforeach()

# libdatachannel 0.24 (same version as the one bundled with OBS on Windows and macOS), OpenSSL backend
set(NO_WEBSOCKET ON CACHE BOOL "" FORCE)
set(NO_EXAMPLES ON CACHE BOOL "" FORCE)
set(NO_TESTS ON CACHE BOOL "" FORCE)
set(USE_GNUTLS OFF CACHE BOOL "" FORCE)
set(USE_MBEDTLS OFF CACHE BOOL "" FORCE)
set(USE_NICE OFF CACHE BOOL "" FORCE)
set(PREFER_SYSTEM_LIB OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  libdatachannel
  GIT_REPOSITORY https://github.com/paullouisageneau/libdatachannel.git
  GIT_TAG v0.24.2
  GIT_SHALLOW TRUE
  EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(libdatachannel)
set(CAMDROP_DATACHANNEL_TARGET LibDataChannel::LibDataChannelStatic)

set(CMAKE_COMPILE_WARNING_AS_ERROR ${_camdrop_saved_warning_as_error})

# qrcodegen (C++), from the distribution
find_package(PkgConfig REQUIRED)
pkg_check_modules(qrcodegencpp REQUIRED IMPORTED_TARGET qrcodegencpp)
if(NOT TARGET qrcodegencpp::qrcodegencpp)
  add_library(qrcodegencpp::qrcodegencpp ALIAS PkgConfig::qrcodegencpp)
endif()
