# FindFFmpeg: locate the FFmpeg libraries shipped with obs-deps (or the system on Linux/macOS).
#
# Creates imported targets FFmpeg::<component> for each requested component (avcodec, avutil, ...).
# The libraries must be the ones OBS is built against: the plugin loads the DLLs bundled with OBS.

include(FindPackageHandleStandardArgs)

find_package(PkgConfig QUIET)

set(FFmpeg_FOUND TRUE)

foreach(_component IN LISTS FFmpeg_FIND_COMPONENTS)
  if(PKG_CONFIG_FOUND)
    pkg_check_modules(_PC_FFmpeg_${_component} QUIET lib${_component})
  endif()

  find_path(
    FFmpeg_${_component}_INCLUDE_DIR
    NAMES lib${_component}/${_component}.h
    HINTS ${_PC_FFmpeg_${_component}_INCLUDE_DIRS}
    PATH_SUFFIXES ffmpeg libav
  )

  find_library(
    FFmpeg_${_component}_LIBRARY
    NAMES ${_component} lib${_component}
    HINTS ${_PC_FFmpeg_${_component}_LIBRARY_DIRS}
  )

  if(FFmpeg_${_component}_INCLUDE_DIR AND FFmpeg_${_component}_LIBRARY)
    set(FFmpeg_${_component}_FOUND TRUE)
    if(NOT TARGET FFmpeg::${_component})
      add_library(FFmpeg::${_component} UNKNOWN IMPORTED)
      set_target_properties(
        FFmpeg::${_component}
        PROPERTIES
          IMPORTED_LOCATION "${FFmpeg_${_component}_LIBRARY}"
          INTERFACE_INCLUDE_DIRECTORIES "${FFmpeg_${_component}_INCLUDE_DIR}"
      )
    endif()
  else()
    set(FFmpeg_${_component}_FOUND FALSE)
    if(FFmpeg_FIND_REQUIRED_${_component})
      set(FFmpeg_FOUND FALSE)
    endif()
  endif()

  mark_as_advanced(FFmpeg_${_component}_INCLUDE_DIR FFmpeg_${_component}_LIBRARY)
endforeach()

find_package_handle_standard_args(FFmpeg REQUIRED_VARS FFmpeg_FOUND HANDLE_COMPONENTS)
