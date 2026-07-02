cmake_minimum_required(VERSION 3.22)
project(obs-dji-uvc VERSION 1.0.0 LANGUAGES C)

# ---------------------------------------------------------------------------
# obs-dji-uvc — DJI Osmo Pocket encoded-UVC source for OBS Studio.
#
# Build (Windows, recommended via obs-plugintemplate conventions):
#   cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo ^
#         -DCMAKE_PREFIX_PATH="C:/obs-deps;C:/obs-studio/build/install"
#   cmake --build build --config RelWithDebInfo
#
# Dependencies:
#   * libobs (headers + import lib) — from an OBS Studio build or obs-deps
#   * FFmpeg (avcodec/avutil/swscale) — ships with obs-deps
#   * libusb-1.0 — fetched below if not found
#   * libuvc    — fetched below (BELABOX-compatible fork preferred)
# ---------------------------------------------------------------------------

include(FetchContent)

find_package(libobs QUIET)
if(NOT libobs_FOUND)
  # Fall back to manual hints: -DLIBOBS_INCLUDE_DIR / -DLIBOBS_LIB
  if(NOT LIBOBS_INCLUDE_DIR OR NOT LIBOBS_LIB)
    message(FATAL_ERROR
      "libobs not found. Either install OBS Studio dev package / build "
      "OBS from source and pass -DCMAKE_PREFIX_PATH, or set "
      "-DLIBOBS_INCLUDE_DIR and -DLIBOBS_LIB manually.")
  endif()
endif()

find_package(FFmpeg REQUIRED COMPONENTS avcodec avutil swscale)

# --- libusb ----------------------------------------------------------------
find_library(LIBUSB_LIB NAMES usb-1.0 libusb-1.0)
find_path(LIBUSB_INCLUDE_DIR libusb-1.0/libusb.h)
if(NOT LIBUSB_LIB OR NOT LIBUSB_INCLUDE_DIR)
  message(STATUS "libusb not found — fetching")
  FetchContent_Declare(libusb
    GIT_REPOSITORY https://github.com/libusb/libusb-cmake.git
    GIT_TAG v1.0.27)
  FetchContent_MakeAvailable(libusb)
  set(LIBUSB_TARGET usb-1.0)
else()
  add_library(vendored_libusb INTERFACE)
  target_include_directories(vendored_libusb INTERFACE ${LIBUSB_INCLUDE_DIR})
  target_link_libraries(vendored_libusb INTERFACE ${LIBUSB_LIB})
  set(LIBUSB_TARGET vendored_libusb)
endif()

# --- libuvc ------------------------------------------------------------------
# The UnlimitedIRL/BELABOX lineage fork carries fixes for DJI frame-based
# H.264 streams; upstream libuvc also works for H264 (UVC_FRAME_FORMAT_H264).
option(DJI_UVC_USE_BELABOX_LIBUVC "Prefer the BELABOX-lineage libuvc fork" ON)
if(DJI_UVC_USE_BELABOX_LIBUVC)
  set(LIBUVC_REPO https://github.com/UnlimitedIRL-Team/libuvc.git)
else()
  set(LIBUVC_REPO https://github.com/libuvc/libuvc.git)
endif()
set(BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(BUILD_TEST OFF CACHE BOOL "" FORCE)
set(CMAKE_BUILD_TARGET "Static" CACHE STRING "" FORCE) # libuvc option name
FetchContent_Declare(libuvc GIT_REPOSITORY ${LIBUVC_REPO} GIT_TAG master)
FetchContent_MakeAvailable(libuvc)

# --- plugin ------------------------------------------------------------------
add_library(obs-dji-uvc MODULE
  src/plugin-main.c
  src/dji-source.c
  src/dji-capture.c
  src/dji-decode.c
  src/dji-nal.c)

target_compile_definitions(obs-dji-uvc PRIVATE DJI_HAVE_OBS)

if(libobs_FOUND)
  target_link_libraries(obs-dji-uvc PRIVATE OBS::libobs)
else()
  target_include_directories(obs-dji-uvc PRIVATE ${LIBOBS_INCLUDE_DIR})
  target_link_libraries(obs-dji-uvc PRIVATE ${LIBOBS_LIB})
endif()

target_link_libraries(obs-dji-uvc PRIVATE
  FFmpeg::avcodec FFmpeg::avutil FFmpeg::swscale
  uvc_static ${LIBUSB_TARGET})

target_include_directories(obs-dji-uvc PRIVATE
  ${libuvc_SOURCE_DIR}/include
  ${libuvc_BINARY_DIR}/include)

if(WIN32)
  target_compile_definitions(obs-dji-uvc PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

set_target_properties(obs-dji-uvc PROPERTIES PREFIX "")

# --- host-side unit test (no OBS required) -----------------------------------
option(DJI_UVC_BUILD_TESTS "Build NAL assembler unit test" OFF)
if(DJI_UVC_BUILD_TESTS)
  add_executable(test-nal src/test-nal.c src/dji-nal.c)
  enable_testing()
  add_test(NAME nal COMMAND test-nal)
endif()

# --- install ------------------------------------------------------------------
if(WIN32)
  install(TARGETS obs-dji-uvc
          LIBRARY DESTINATION "obs-plugins/64bit"
          RUNTIME DESTINATION "obs-plugins/64bit")
  install(DIRECTORY data/ DESTINATION "data/obs-plugins/obs-dji-uvc")
else()
  install(TARGETS obs-dji-uvc LIBRARY DESTINATION "lib/obs-plugins")
  install(DIRECTORY data/ DESTINATION "share/obs/obs-plugins/obs-dji-uvc")
endif()
