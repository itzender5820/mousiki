cmake_minimum_required(VERSION 3.16)
project(mousiki VERSION 0.1.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# ---------------------------------------------------------------------------
# miniaudio.h — fetched at configure time (needs network on the build machine)
# ---------------------------------------------------------------------------
set(MINIAUDIO_DIR "${CMAKE_SOURCE_DIR}/third_party")
set(MINIAUDIO_HEADER "${MINIAUDIO_DIR}/miniaudio.h")
if(NOT EXISTS "${MINIAUDIO_HEADER}")
  message(STATUS "Downloading miniaudio.h ...")
  file(DOWNLOAD
    "https://raw.githubusercontent.com/mackron/miniaudio/master/miniaudio.h"
    "${MINIAUDIO_HEADER}"
    SHOW_PROGRESS
    STATUS MA_DL_STATUS)
  list(GET MA_DL_STATUS 0 MA_DL_CODE)
  if(NOT MA_DL_CODE EQUAL 0)
    message(FATAL_ERROR "Failed to download miniaudio.h — download it manually into ${MINIAUDIO_DIR}/miniaudio.h")
  endif()
endif()

# ---------------------------------------------------------------------------
# kissfft — small real-input FFT used by the spectrum visualizer strip.
# Fetched as loose files (same approach as miniaudio.h above) rather than
# FetchContent+add_subdirectory, so we don't depend on upstream's own
# CMakeLists.txt / target names staying stable.
# ---------------------------------------------------------------------------
set(KISSFFT_DIR "${CMAKE_SOURCE_DIR}/third_party/kissfft")
set(KISSFFT_BASE_URL "https://raw.githubusercontent.com/mborgerding/kissfft/master")
set(KISSFFT_FILES kiss_fft.c kiss_fft.h _kiss_fft_guts.h kiss_fft_log.h kiss_fftr.c kiss_fftr.h)
file(MAKE_DIRECTORY "${KISSFFT_DIR}")
foreach(kf IN LISTS KISSFFT_FILES)
  if(NOT EXISTS "${KISSFFT_DIR}/${kf}")
    message(STATUS "Downloading kissfft/${kf} ...")
    file(DOWNLOAD "${KISSFFT_BASE_URL}/${kf}" "${KISSFFT_DIR}/${kf}" STATUS KF_DL_STATUS)
    list(GET KF_DL_STATUS 0 KF_DL_CODE)
    if(NOT KF_DL_CODE EQUAL 0)
      message(FATAL_ERROR "Failed to download kissfft/${kf} — download the kissfft repo manually into ${KISSFFT_DIR}")
    endif()
  endif()
endforeach()

# ---------------------------------------------------------------------------
# Sources
# ---------------------------------------------------------------------------
file(GLOB_RECURSE MUISC_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/*.cpp")
set(KISSFFT_SOURCES "${KISSFFT_DIR}/kiss_fft.c" "${KISSFFT_DIR}/kiss_fftr.c")
set_source_files_properties(${KISSFFT_SOURCES} PROPERTIES LANGUAGE C)

add_executable(mousiki ${MUISC_SOURCES} ${KISSFFT_SOURCES})
target_include_directories(mousiki PRIVATE "${CMAKE_SOURCE_DIR}/src" "${MINIAUDIO_DIR}" "${KISSFFT_DIR}")

# ---------------------------------------------------------------------------
# Platform-specific audio backend wiring (compile-time selected)
# miniaudio auto-detects backends at runtime, but we pin the preferred
# backend per-platform here so the intended native API is what gets tried
# first: PipeWire on Linux, WASAPI on Windows, OpenSL ES on Android.
# ---------------------------------------------------------------------------
find_package(Threads REQUIRED)
target_link_libraries(mousiki PRIVATE Threads::Threads)

if(ANDROID)
  target_compile_definitions(mousiki PRIVATE MUISC_PLATFORM_ANDROID=1 MA_ENABLE_ONLY_SPECIFIC_BACKENDS=1 MA_ENABLE_OPENSL=1)
  target_link_libraries(mousiki PRIVATE OpenSLES log android)
elseif(WIN32)
  target_compile_definitions(mousiki PRIVATE MUISC_PLATFORM_WINDOWS=1 MA_ENABLE_ONLY_SPECIFIC_BACKENDS=1 MA_ENABLE_WASAPI=1)
  target_link_libraries(mousiki PRIVATE ole32 winmm)
elseif(APPLE)
  # miniaudio links CoreAudio at runtime via dlopen, so no -framework flags
  # are needed here. All backends stay compiled in; the runtime picks
  # ma_backend_coreaudio (see audio_backend.cpp).
  target_compile_definitions(mousiki PRIVATE MUISC_PLATFORM_MACOS=1)
  target_link_libraries(mousiki PRIVATE dl m)
elseif(UNIX AND NOT APPLE)
  target_compile_definitions(mousiki PRIVATE MUISC_PLATFORM_LINUX=1 MA_ENABLE_ONLY_SPECIFIC_BACKENDS=1 MA_ENABLE_PULSEAUDIO=1 MA_ENABLE_ALSA=1)
  # Note: miniaudio talks to PipeWire through its PulseAudio *and* ALSA
  # compatibility shims (pipewire-pulse / pipewire-alsa) — there is no
  # separate native "ma_backend_pipewire" enum in upstream miniaudio, so we
  # enable both shims and let the PipeWire session manager intercept them.
  target_link_libraries(mousiki PRIVATE dl m)
endif()

message(STATUS "mousiki: building for ${CMAKE_SYSTEM_NAME}")
