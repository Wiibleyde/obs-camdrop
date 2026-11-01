# Camdrop dependencies on macOS
#
# obs-deps ships mbedTLS without a CMake package on macOS (OBS uses its own find module): create the
# MbedTLS::* targets from the libraries bundled with OBS. libdatachannel and qrcodegencpp have one.

include_guard(GLOBAL)

find_path(MbedTLS_INCLUDE_DIR NAMES mbedtls/ssl.h REQUIRED)
foreach(_lib IN ITEMS mbedtls mbedx509 mbedcrypto)
  find_library(MbedTLS_${_lib}_LIBRARY NAMES ${_lib} REQUIRED)
  if(NOT TARGET MbedTLS::${_lib})
    add_library(MbedTLS::${_lib} UNKNOWN IMPORTED)
    set_target_properties(
      MbedTLS::${_lib}
      PROPERTIES IMPORTED_LOCATION "${MbedTLS_${_lib}_LIBRARY}" INTERFACE_INCLUDE_DIRECTORIES "${MbedTLS_INCLUDE_DIR}"
    )
  endif()
  mark_as_advanced(MbedTLS_${_lib}_LIBRARY)
endforeach()
mark_as_advanced(MbedTLS_INCLUDE_DIR)

find_package(LibDataChannel REQUIRED)
find_package(qrcodegencpp REQUIRED)
set(CAMDROP_DATACHANNEL_TARGET LibDataChannel::LibDataChannel)
