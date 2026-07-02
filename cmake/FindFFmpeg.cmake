# FindFFmpeg.cmake — locate FFmpeg (avcodec/avutil/swscale) from obs-deps.
# Provides imported targets FFmpeg::avcodec, FFmpeg::avutil, FFmpeg::swscale.

include(FindPackageHandleStandardArgs)

set(_ffmpeg_components avcodec avutil swscale)
if(FFmpeg_FIND_COMPONENTS)
  set(_ffmpeg_components ${FFmpeg_FIND_COMPONENTS})
endif()

set(_ffmpeg_all_found TRUE)
foreach(_comp IN LISTS _ffmpeg_components)
  find_path(FFMPEG_${_comp}_INCLUDE_DIR
    NAMES lib${_comp}/${_comp}.h
    PATH_SUFFIXES include)
  find_library(FFMPEG_${_comp}_LIBRARY
    NAMES ${_comp} lib${_comp}
    PATH_SUFFIXES lib bin)

  if(FFMPEG_${_comp}_INCLUDE_DIR AND FFMPEG_${_comp}_LIBRARY)
    set(FFmpeg_${_comp}_FOUND TRUE)
    if(NOT TARGET FFmpeg::${_comp})
      add_library(FFmpeg::${_comp} UNKNOWN IMPORTED)
      set_target_properties(FFmpeg::${_comp} PROPERTIES
        IMPORTED_LOCATION "${FFMPEG_${_comp}_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_${_comp}_INCLUDE_DIR}")
    endif()
  else()
    set(_ffmpeg_all_found FALSE)
  endif()
endforeach()

find_package_handle_standard_args(FFmpeg
  REQUIRED_VARS _ffmpeg_all_found
  HANDLE_COMPONENTS)
