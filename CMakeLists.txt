cmake_minimum_required(VERSION 3.16)
project(mousiki VERSION 5.0.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

# A quicker build for development: cmake -DCMAKE_BUILD_TYPE=Dev ..  (-O1 instead of -O3: a file compiles in about 2/3 of the
# time; the program is a little slower, which the tests and the UI smoke test do not mind). Release stays the default.
if(MSVC)
  set(CMAKE_CXX_FLAGS_DEV "/O1" CACHE STRING "C++ flags for the Dev build type")
  set(CMAKE_C_FLAGS_DEV "/O1" CACHE STRING "C flags for the Dev build type")
else()
  set(CMAKE_CXX_FLAGS_DEV "-O1" CACHE STRING "C++ flags for the Dev build type")
  set(CMAKE_C_FLAGS_DEV "-O1" CACHE STRING "C flags for the Dev build type")
endif()

# ccache (when it is installed): an unchanged file is not compiled again -- after a change to a header, a switch of the branch or a clean
# build the object files come out of the cache. Switch off with -DMOUSIKI_USE_CCACHE=OFF.
option(MOUSIKI_USE_CCACHE "Use ccache for the compiler when it is installed (much faster rebuilds)" ON)
set(MOUSIKI_CCACHE_ACTIVE OFF)
if(MOUSIKI_USE_CCACHE)
  find_program(MOUSIKI_CCACHE_EXE ccache)
  if(MOUSIKI_CCACHE_EXE)
    if(MSVC AND CMAKE_GENERATOR MATCHES "Visual Studio")
      # The Visual Studio generators have no compiler launcher: ccache is put in as cl.exe (a copy next to the build, found through
      # CLToolPath). /Zi is not compatible with ccache, /Z7 is (the Release build has no debug information at all). Needs CMake 3.25.
      if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.25)
        cmake_policy(SET CMP0141 NEW)
        file(COPY_FILE "${MOUSIKI_CCACHE_EXE}" "${CMAKE_BINARY_DIR}/cl.exe" ONLY_IF_DIFFERENT)
        set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<$<CONFIG:Debug,RelWithDebInfo>:Embedded>")
        set(CMAKE_VS_GLOBALS "CLToolExe=cl.exe" "CLToolPath=${CMAKE_BINARY_DIR}" "UseMultiToolTask=true")
        set(MOUSIKI_CCACHE_ACTIVE ON)
      else()
        message(STATUS "ccache found, but the Visual Studio generator needs CMake 3.25 or newer for it: not used")
      endif()
    elseif(NOT CMAKE_GENERATOR MATCHES "Xcode")
      set(CMAKE_C_COMPILER_LAUNCHER "${MOUSIKI_CCACHE_EXE}")
      set(CMAKE_CXX_COMPILER_LAUNCHER "${MOUSIKI_CCACHE_EXE}")
      if(MSVC)   # (Ninja with cl.exe)
        foreach(_cfg DEBUG RELWITHDEBINFO)
          string(REPLACE "/Zi" "/Z7" CMAKE_CXX_FLAGS_${_cfg} "${CMAKE_CXX_FLAGS_${_cfg}}")
          string(REPLACE "/Zi" "/Z7" CMAKE_C_FLAGS_${_cfg} "${CMAKE_C_FLAGS_${_cfg}}")
        endforeach()
      endif()
      set(MOUSIKI_CCACHE_ACTIVE ON)
    endif()
    if(MOUSIKI_CCACHE_ACTIVE)
      message(STATUS "ccache: ${MOUSIKI_CCACHE_EXE}")
    endif()
  else()
    message(STATUS "ccache not found (installing it makes rebuilds much faster)")
  endif()
endif()

# Visual Studio generators compile the files of one project one after the other unless cl.exe is told to use all cores (/MP).
# (Ninja and Makefiles start several compilers by themselves, so they do not need it. With ccache as cl.exe it is UseMultiToolTask
# instead: ccache cannot take several files in one call.)
if(MSVC AND CMAKE_GENERATOR MATCHES "Visual Studio" AND NOT MOUSIKI_CCACHE_ACTIVE)
  add_compile_options($<$<COMPILE_LANGUAGE:C,CXX>:/MP>)
endif()

# ---------------------------------------------------------------------------
# Third-party code — vendored in third_party/, so configuring needs no network.
#   miniaudio.h         : single-file audio library (public domain / MIT-0), v0.11.26
#   kissfft/            : small real-input FFT for the spectrum visualizer (BSD-3-Clause)
#   miniz/              : deflate (MIT / public domain), compresses the oscilloscope picture sent over Kitty graphics
#   chromaprint/        : AcoustID fingerprinting library, 1.6.2 (MIT/LGPL-2.1) —
#                         compiled into the fpcalc helper below, which is why the
#                         project needs neither a prebuilt fpcalc.exe nor any
#                         download to produce one.
# ---------------------------------------------------------------------------
set(MINIAUDIO_DIR "${CMAKE_SOURCE_DIR}/third_party")
set(MINIAUDIO_HEADER "${MINIAUDIO_DIR}/miniaudio.h")
set(KISSFFT_DIR "${CMAKE_SOURCE_DIR}/third_party/kissfft")
set(KISSFFT_FILES kiss_fft.c kiss_fft.h _kiss_fft_guts.h kiss_fft_log.h kiss_fftr.c kiss_fftr.h)
set(CHROMAPRINT_DIR "${CMAKE_SOURCE_DIR}/third_party/chromaprint")
set(CHROMAPRINT_KISSFFT_DIR "${CHROMAPRINT_DIR}/kissfft")

if(NOT EXISTS "${MINIAUDIO_HEADER}")
  message(FATAL_ERROR "Missing ${MINIAUDIO_HEADER} — third_party/ is part of the repository; restore it (e.g. `git checkout -- third_party`).")
endif()
foreach(kf IN LISTS KISSFFT_FILES)
  if(NOT EXISTS "${KISSFFT_DIR}/${kf}")
    message(FATAL_ERROR "Missing ${KISSFFT_DIR}/${kf} — third_party/ is part of the repository; restore it (e.g. `git checkout -- third_party`).")
  endif()
endforeach()
if(NOT EXISTS "${CHROMAPRINT_DIR}/chromaprint.h")
  message(FATAL_ERROR "Missing ${CHROMAPRINT_DIR}/chromaprint.h — third_party/ is part of the repository; restore it (e.g. `git checkout -- third_party`).")
endif()

# The Chromaprint library, exactly the file list upstream's src/CMakeLists.txt
# uses — minus the FFT backends that need FFmpeg/FFTW/Accelerate, which are not
# vendored (see third_party/chromaprint/README.md). The glob is safe because
# those files are simply not in the tree.
file(GLOB CHROMAPRINT_SOURCES CONFIGURE_DEPENDS
     "${CHROMAPRINT_DIR}/*.cpp" "${CHROMAPRINT_DIR}/utils/*.cpp")
file(GLOB CHROMAPRINT_KISSFFT_SOURCES CONFIGURE_DEPENDS
     "${CHROMAPRINT_KISSFFT_DIR}/kiss_fft.c" "${CHROMAPRINT_KISSFFT_DIR}/kiss_fftr.c")
if(NOT CHROMAPRINT_SOURCES OR NOT CHROMAPRINT_KISSFFT_SOURCES)
  message(FATAL_ERROR "Chromaprint sources missing under ${CHROMAPRINT_DIR} — third_party/ is part of the repository.")
endif()

# ---------------------------------------------------------------------------
# Sources
# ---------------------------------------------------------------------------
# Windows: the System Media Transport Controls (media pop-up, lock screen, media keys) use C++/WinRT from the Windows SDK
# (src/media_session_win.cpp; MSVC only -- MinGW and an SDK without it build without it). Switch off if the build fails:
#   cmake -DMOUSIKI_WINDOWS_SMTC=OFF ...
option(MOUSIKI_WINDOWS_SMTC "Windows: media keys and the media pop-up (System Media Transport Controls, MSVC + Windows SDK)" ON)

# macOS: the Now Playing / media key support is Objective-C++ (src/media_session_mac.mm), built with ARC.
if(APPLE)
  enable_language(OBJCXX)
  set(MEDIA_MAC_SOURCE "${CMAKE_SOURCE_DIR}/src/media_session_mac.mm")
  set_source_files_properties("${MEDIA_MAC_SOURCE}" PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
endif()
file(GLOB_RECURSE MUISC_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/*.cpp")
set(KISSFFT_SOURCES "${KISSFFT_DIR}/kiss_fft.c" "${KISSFFT_DIR}/kiss_fftr.c")
set_source_files_properties(${KISSFFT_SOURCES} PROPERTIES LANGUAGE C)
if(MSVC)   # kissfft is vendored unchanged (third_party/): its size_t -> int conversions (C4267) are known and harmless, so MSVC is not asked about them
  set_source_files_properties(${KISSFFT_SOURCES} PROPERTIES COMPILE_OPTIONS "/wd4267")
endif()
# miniz: the deflate used to compress the oscilloscope picture for Kitty graphics (src/terminal_gfx.cpp)
set(MINIZ_DIR "${CMAKE_SOURCE_DIR}/third_party/miniz")
if(NOT EXISTS "${MINIZ_DIR}/miniz.c")
  message(FATAL_ERROR "Missing ${MINIZ_DIR}/miniz.c — third_party/ is part of the repository; restore it (e.g. `git checkout -- third_party`).")
endif()
set(MINIZ_SOURCES "${MINIZ_DIR}/miniz.c")
set_source_files_properties(${MINIZ_SOURCES} PROPERTIES LANGUAGE C)

# Everything but main() is one OBJECT library, compiled once and linked into the player AND into the test program
# (mousiki_tests), which used to compile all of it a second time. Include paths, definitions and libraries are PUBLIC on it,
# so main.cpp and the tests are built with the same settings.
set(MOUSIKI_MAIN_SOURCE "${CMAKE_SOURCE_DIR}/src/main.cpp")
set(MUISC_CORE_SOURCES ${MUISC_SOURCES})
list(REMOVE_ITEM MUISC_CORE_SOURCES "${MOUSIKI_MAIN_SOURCE}")
add_library(mousiki_core OBJECT ${MUISC_CORE_SOURCES} ${KISSFFT_SOURCES} ${MINIZ_SOURCES})
add_executable(mousiki "${MOUSIKI_MAIN_SOURCE}")
target_link_libraries(mousiki PRIVATE mousiki_core)
target_include_directories(mousiki_core PUBLIC "${CMAKE_SOURCE_DIR}/src" "${MINIAUDIO_DIR}" "${KISSFFT_DIR}" "${MINIZ_DIR}")
# The version shown on the ABOUT APP tabs (src/version.h) comes from project(... VERSION ...) above,
# so a release only has to change that one line.
target_compile_definitions(mousiki_core PUBLIC MOUSIKI_VERSION="${PROJECT_VERSION}")

# ---------------------------------------------------------------------------
# App icon (Windows only): mousiki.rc + mousiki.ico in the source root -- the
# packaging pipeline copies them in before building. Guarded so a plain
# source checkout without an icon still builds.
if(WIN32 AND EXISTS "${CMAKE_SOURCE_DIR}/mousiki.rc")
    target_sources(mousiki PRIVATE "${CMAKE_SOURCE_DIR}/mousiki.rc")
endif()

# ---------------------------------------------------------------------------
# fpcalc — the fingerprint helper the meta editor's AcoustID fetch calls
# (scripts/fetch_meta.py runs `fpcalc -json -length 120 <file>`).
#
# Built from the vendored Chromaprint sources, so a fresh checkout builds the
# whole feature: no prebuilt binary to download, nothing to install. The only
# difference from upstream's tool is that this one *spawns* ffmpeg for decoding
# (already a hard mousiki dependency) instead of linking FFmpeg's development
# libraries — see third_party/chromaprint/README.md.
# ---------------------------------------------------------------------------
add_executable(fpcalc_tool ${CMAKE_SOURCE_DIR}/tools/fpcalc.cpp
                           ${CHROMAPRINT_SOURCES} ${CHROMAPRINT_KISSFFT_SOURCES})
target_include_directories(fpcalc_tool PRIVATE "${CHROMAPRINT_DIR}" "${CHROMAPRINT_KISSFFT_DIR}")
# The target is called fpcalc_tool so the CMake name is unambiguous, but the
# program must be called fpcalc: fetch_meta.py looks it up by file name.
set_target_properties(fpcalc_tool PROPERTIES OUTPUT_NAME fpcalc)
# Chromaprint's matcher scales rand() by 0.001f / RAND_MAX, which Clang (macOS) reports as a harmless int -> float rounding warning;
# the vendored file stays untouched.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  target_compile_options(fpcalc_tool PRIVATE -Wno-implicit-const-int-float-conversion)
endif()
target_compile_definitions(fpcalc_tool PRIVATE
    # Chromaprint is compiled straight into this executable, so its API must not
    # be __declspec(dllimport) — that is what CHROMAPRINT_NODLL switches off.
    CHROMAPRINT_NODLL
    # Selects the vendored config.h, which upstream's CMake would generate.
    HAVE_CONFIG_H
    # utils.h computes window functions with M_PI, which MSVC hides otherwise.
    _USE_MATH_DEFINES
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    _CRT_SECURE_NO_WARNINGS)
set_source_files_properties(${CHROMAPRINT_KISSFFT_SOURCES} PROPERTIES LANGUAGE C)
if(MSVC)
  set_source_files_properties(${CHROMAPRINT_KISSFFT_SOURCES} PROPERTIES COMPILE_OPTIONS "/wd4267")   # (the same known conversion warning)
endif()
if(MSVC)
  # Vendored upstream code, not ours: fingerprint_matcher.cpp trips C4244 while
  # instantiating std::pair inside MSVC's own <utility>. Silencing it on the
  # vendored files only keeps warnings from tools/fpcalc.cpp visible.
  set_source_files_properties(${CHROMAPRINT_SOURCES} PROPERTIES COMPILE_OPTIONS "/wd4244")
endif()
if(WIN32)
  # CommandLineToArgvW(): the helper parses its own UTF-16 command line so that
  # non-ASCII file names survive (main()'s char** goes through the ANSI code page).
  target_link_libraries(fpcalc_tool PRIVATE shell32)
endif()

# ---------------------------------------------------------------------------
# Platform-specific audio backend wiring (compile-time selected)
# ---------------------------------------------------------------------------
find_package(Threads REQUIRED)
target_link_libraries(mousiki_core PUBLIC Threads::Threads)

# The lyrics / metadata / search helpers are invoked as script paths, so
# scripts/ has to sit next to the executable (on every platform -- the Linux
# and macOS builds look for it there too). Multi-config generators (Visual
# Studio, Xcode) put the exe in build/Release/, one level deeper than a
# single-config build. This has to be declared before the fpcalc copy step at
# the bottom, which drops the helper into that same scripts/ folder.
if(EXISTS "${CMAKE_SOURCE_DIR}/scripts")
  add_custom_command(TARGET mousiki POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_directory
              "${CMAKE_SOURCE_DIR}/scripts"
              "$<TARGET_FILE_DIR:mousiki>/scripts")
endif()

if(ANDROID)
  target_compile_definitions(mousiki_core PUBLIC MUISC_PLATFORM_ANDROID=1 MA_ENABLE_ONLY_SPECIFIC_BACKENDS=1 MA_ENABLE_OPENSL=1)
  target_link_libraries(mousiki_core PUBLIC OpenSLES log android)
elseif(WIN32)
  target_compile_definitions(mousiki_core PUBLIC MUISC_PLATFORM_WINDOWS=1 MA_ENABLE_ONLY_SPECIFIC_BACKENDS=1 MA_ENABLE_WASAPI=1)
  target_link_libraries(mousiki_core PUBLIC ole32 winmm ws2_32 iphlpapi shell32 gdi32 user32)   # gdi32 / user32: the font list (terminal_font.cpp)
  if(NOT MOUSIKI_WINDOWS_SMTC)
    target_compile_definitions(mousiki_core PUBLIC MOUSIKI_NO_SMTC=1)
  endif()   # ws2_32: the remote app's web server

  target_compile_definitions(mousiki_core PUBLIC
      # <windows.h> otherwise defines min/max as macros, which collide with
      # std::min/std::max all over app.cpp.
      NOMINMAX
      WIN32_LEAN_AND_MEAN
      # M_PI is not in the C++ standard; MSVC only exposes it behind this,
      # and fft_visualizer.cpp's Hann window needs it.
      _USE_MATH_DEFINES
      # getenv/putenv/localtime are "deprecated" by MSVC in favour of the _s
      # variants; the code uses them deliberately and portably.
      _CRT_SECURE_NO_WARNINGS
      _CRT_NONSTDC_NO_WARNINGS)

  if(MSVC)
    # /utf-8 tells MSVC that the sources are UTF-8 *and* that narrow string
    # literals should stay UTF-8. Without it the box-drawing and block
    # characters baked into the UI get re-encoded to the build machine's ANSI
    # code page and render as mojibake at runtime.
    target_compile_options(mousiki_core PUBLIC /utf-8 /EHsc /W3
                           # the app_*.cpp files are big and full of templates / string tables and exceed the
                           # default section limit in debug builds.
                           /bigobj)
  endif()
elseif(APPLE)
  target_compile_definitions(mousiki_core PUBLIC MUISC_PLATFORM_MACOS=1 MA_ENABLE_ONLY_SPECIFIC_BACKENDS=1 MA_ENABLE_COREAUDIO=1)
  target_link_libraries(mousiki_core PUBLIC "-framework CoreAudio" "-framework AudioToolbox" "-framework CoreFoundation" "-framework AudioUnit"
                        "-framework Foundation" "-framework MediaPlayer")
  target_sources(mousiki_core PRIVATE "${MEDIA_MAC_SOURCE}")
elseif(UNIX AND NOT APPLE)
  target_compile_definitions(mousiki_core PUBLIC MUISC_PLATFORM_LINUX=1 MA_ENABLE_ONLY_SPECIFIC_BACKENDS=1 MA_ENABLE_PULSEAUDIO=1 MA_ENABLE_ALSA=1)
  # Note: miniaudio talks to PipeWire through its PulseAudio *and* ALSA
  # compatibility shims (pipewire-pulse / pipewire-alsa) — there is no
  # separate native "ma_backend_pipewire" enum in upstream miniaudio.
  target_link_libraries(mousiki_core PUBLIC dl m)
endif()

# fetch_meta.py looks for fpcalc in its own directory first, so the freshly
# built helper goes next to the scripts copied above — same place the old
# prebuilt scripts/fpcalc.exe used to live, now a build product instead of a
# checked-in binary. add_dependencies() guarantees it is built before mousiki.
if(TARGET fpcalc_tool)
  add_dependencies(mousiki fpcalc_tool)
  add_custom_command(TARGET mousiki POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              "$<TARGET_FILE:fpcalc_tool>"
              "$<TARGET_FILE_DIR:mousiki>/scripts/$<TARGET_FILE_NAME:fpcalc_tool>")
endif()


# ---------------------------------------------------------------------------
# Radio mode (src_radio/): part of the SAME program as the music player. SHIFT and + (the '*' character) switches
# modes: the radio ends completely (audio device, threads, caches), the player is only suspended while the radio
# runs -- see src/main.cpp and src/mode_switch.h.
# The radio has its own audio path (ffmpeg -> ring buffer -> its own miniaudio device) and shares only the UI
# plumbing (settings, terminal I/O, process spawning) and the visualizers with the player -- not Player, not
# StreamingPcm. Its data files (stations.txt, radio_config.txt) now live in the project root.
# ---------------------------------------------------------------------------
file(GLOB RADIO_MODE_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src_radio/*.cpp")
list(FILTER RADIO_MODE_SOURCES EXCLUDE REGEX "radio_standalone\\.cpp$")
if(NOT RADIO_MODE_SOURCES)
  message(FATAL_ERROR "src_radio/ is missing: the radio mode's sources belong in src_radio/ (see CMakeLists.txt).")
endif()
target_sources(mousiki_core PRIVATE ${RADIO_MODE_SOURCES})
target_include_directories(mousiki_core PUBLIC "${CMAKE_SOURCE_DIR}/src_radio")
# MA_ENABLE_NULL: lets MOUSIKI_RADIO_NULL=1 run the radio headless (tests, CI).
target_compile_definitions(mousiki_core PUBLIC MA_ENABLE_NULL=1)
if(EXISTS "${CMAKE_SOURCE_DIR}/stations.txt")
  add_custom_command(TARGET mousiki POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/stations.txt"
              "$<TARGET_FILE_DIR:mousiki>/stations.txt")
endif()
# radio_config.txt: the radio's OWN settings (colours), separate from the player's config.txt.
if(EXISTS "${CMAKE_SOURCE_DIR}/radio_config.txt")
  add_custom_command(TARGET mousiki POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/radio_config.txt"
              "$<TARGET_FILE_DIR:mousiki>/radio_config.txt")
endif()

# ---------------------------------------------------------------------------
# Audio book mode (src_audio_book/): the same program, too. Its App instance, the notes / bookmark / chapter-mark overlays,
# the .cue and notes files, the cutting of chapters and the book pictures live here; the shared plumbing stays in src/.
# ---------------------------------------------------------------------------
file(GLOB AUDIO_BOOK_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src_audio_book/*.cpp")
if(NOT AUDIO_BOOK_SOURCES)
  message(FATAL_ERROR "src_audio_book/ is missing: the audio book mode's sources belong in src_audio_book/ (see CMakeLists.txt).")
endif()
target_sources(mousiki_core PRIVATE ${AUDIO_BOOK_SOURCES})
target_include_directories(mousiki_core PUBLIC "${CMAKE_SOURCE_DIR}/src_audio_book")

# Compiler warnings for GCC / Clang (MinGW included; MSVC has /W3 above): -Wall -Wextra on Mousiki's own sources only,
# so the vendored libraries (miniaudio, kissfft, miniz, chromaprint) stay quiet. Warnings never stop the build.
option(MOUSIKI_WARNINGS "GCC/Clang: build Mousiki's own sources with -Wall -Wextra" ON)
if(MOUSIKI_WARNINGS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  set(MOUSIKI_OWN_SOURCES ${MUISC_SOURCES} ${RADIO_MODE_SOURCES} ${AUDIO_BOOK_SOURCES})
  list(FILTER MOUSIKI_OWN_SOURCES EXCLUDE REGEX "(^|/)src/miniaudio_impl\\.cpp$")
  set_property(SOURCE ${MOUSIKI_OWN_SOURCES} APPEND PROPERTY COMPILE_OPTIONS -Wall -Wextra)
endif()

# Optional stand-alone radio executable (development / tests): the radio only, no mode switch.
option(MOUSIKI_BUILD_RADIO_STANDALONE "Also build the radio as its own executable (mousiki_radio)" OFF)
if(MOUSIKI_BUILD_RADIO_STANDALONE)
  set(RADIO_SOURCES ${RADIO_MODE_SOURCES} src_radio/radio_standalone.cpp
      src/history.cpp src/cache_manager.cpp src/youtube_source.cpp src/online_source.cpp
      src/terminal_ui.cpp src/sphere_visualizer.cpp src/indic_handler.cpp src/process_util.cpp src/console_log.cpp
      src/fft_visualizer.cpp src/hifi_bars.cpp src/audio_backend.cpp src/miniaudio_impl.cpp src/keyboard_layout.cpp
      src/spectrogram.cpp src/scope_window.cpp src/themes.cpp src/border_gradient.cpp
      # shared with the player: the oscilloscope and the terminal picture output (Kitty / Sixel)
      src/oscilloscope_visualizer.cpp src/terminal_gfx.cpp src/remote_control.cpp src/remote_server.cpp src/remote_overlay.cpp src/qr_code.cpp src/terminal_font.cpp
      # the player's settings code: Share Profiles, Remote App, the shared oscilloscope / spectrogram rows
      src/settings.cpp src/translit.cpp src/lyrics_fetcher.cpp src/TextSanitizer.cpp src/osci_settings.cpp src/update_check.cpp src/open_url.cpp src/media_session.cpp src/media_session_mpris.cpp
      # the help wizard (SHIFT+w): its pages, and the disk its welcome page spins
      src/wizard.cpp src/disk_art.cpp)
  if(WIN32)
    list(APPEND RADIO_SOURCES src/win_compat.cpp src/media_session_win.cpp)
  endif()
  add_executable(mousiki_radio ${RADIO_SOURCES} ${KISSFFT_SOURCES} ${MINIZ_SOURCES})
  target_include_directories(mousiki_radio PRIVATE "${CMAKE_SOURCE_DIR}/src" "${CMAKE_SOURCE_DIR}/src_radio"
                             "${MINIAUDIO_DIR}" "${KISSFFT_DIR}" "${MINIZ_DIR}")
  target_link_libraries(mousiki_radio PRIVATE Threads::Threads)
  target_compile_definitions(mousiki_radio PRIVATE MOUSIKI_VERSION="${PROJECT_VERSION}")
  # MA_ENABLE_NULL: lets MOUSIKI_RADIO_NULL=1 run the radio headless (tests, CI).
  if(ANDROID)
    target_compile_definitions(mousiki_radio PRIVATE MUISC_PLATFORM_ANDROID=1 MA_ENABLE_ONLY_SPECIFIC_BACKENDS=1 MA_ENABLE_OPENSL=1 MA_ENABLE_NULL=1)
    target_link_libraries(mousiki_radio PRIVATE OpenSLES log android)
  elseif(WIN32)
    target_compile_definitions(mousiki_radio PRIVATE MUISC_PLATFORM_WINDOWS=1 MA_ENABLE_ONLY_SPECIFIC_BACKENDS=1 MA_ENABLE_WASAPI=1 MA_ENABLE_NULL=1
                               NOMINMAX WIN32_LEAN_AND_MEAN _USE_MATH_DEFINES _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS)
    target_link_libraries(mousiki_radio PRIVATE ole32 winmm ws2_32 iphlpapi shell32 gdi32 user32)
    if(NOT MOUSIKI_WINDOWS_SMTC)
      target_compile_definitions(mousiki_radio PRIVATE MOUSIKI_NO_SMTC=1)
    endif()
    if(MSVC)
      target_compile_options(mousiki_radio PRIVATE /utf-8 /EHsc /W3)
    endif()
  elseif(APPLE)
    target_compile_definitions(mousiki_radio PRIVATE MUISC_PLATFORM_MACOS=1 MA_ENABLE_ONLY_SPECIFIC_BACKENDS=1 MA_ENABLE_COREAUDIO=1 MA_ENABLE_NULL=1)
    target_link_libraries(mousiki_radio PRIVATE "-framework CoreAudio" "-framework AudioToolbox" "-framework CoreFoundation" "-framework AudioUnit"
                          "-framework Foundation" "-framework MediaPlayer")
    target_sources(mousiki_radio PRIVATE "${MEDIA_MAC_SOURCE}")
  else()
    target_compile_definitions(mousiki_radio PRIVATE MUISC_PLATFORM_LINUX=1 MA_ENABLE_ONLY_SPECIFIC_BACKENDS=1 MA_ENABLE_PULSEAUDIO=1 MA_ENABLE_ALSA=1 MA_ENABLE_NULL=1)
    target_link_libraries(mousiki_radio PRIVATE dl m)
  endif()
endif()

message(STATUS "mousiki: building for ${CMAKE_SYSTEM_NAME}")


# ---------------------------------------------------------------------------
# Automated tests (tests/): cmake -DMOUSIKI_BUILD_TESTS=ON ..  then  ctest  (or run mousiki_tests directly).
# The test program is built from the same sources and settings as the player (everything but src/main.cpp), so it checks the real code.
# ---------------------------------------------------------------------------
option(MOUSIKI_BUILD_TESTS "Build the automated tests (mousiki_tests)" OFF)
if(MOUSIKI_BUILD_TESTS)
  enable_testing()
  file(GLOB MOUSIKI_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/tests/*.cpp")
  add_executable(mousiki_tests ${MOUSIKI_TEST_SOURCES})
  target_link_libraries(mousiki_tests PRIVATE mousiki_core)   # the player's objects, compiled once (see mousiki_core above)
  if(MOUSIKI_WARNINGS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set_property(SOURCE ${MOUSIKI_TEST_SOURCES} APPEND PROPERTY COMPILE_OPTIONS -Wall -Wextra)
  endif()
  target_include_directories(mousiki_tests PRIVATE "${CMAKE_SOURCE_DIR}/tests")
  add_test(NAME mousiki_tests COMMAND mousiki_tests)

  # Python tests (Linux / macOS; need `pip install pyte` for the UI test, else CTest reports them as skipped).
  # Build the player too (cmake --build ... --target mousiki mousiki_tests) so the UI test has a program to run.
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_Interpreter_FOUND AND NOT WIN32)
    add_test(NAME ui_smoke COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/tests/ui_smoke.py" $<TARGET_FILE:mousiki>
             --scripts "${CMAKE_SOURCE_DIR}/scripts")
    set_tests_properties(ui_smoke PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 900)
  endif()
  if(Python3_Interpreter_FOUND)
    add_test(NAME installer_checksum COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/tests/test_install_kanji_reader.py" "${CMAKE_SOURCE_DIR}/scripts")
    add_test(NAME kanji_reading_text COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/tests/test_kanji_reading_text.py" "${CMAKE_SOURCE_DIR}/scripts")
    add_test(NAME fetch_selftest_offline COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/scripts/selftest_fetch.py" --offline --only scripts,pins)
  endif()
endif()
