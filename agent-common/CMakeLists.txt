# Pieces shared by the desktop agents (Windows, macOS): "listen on the phone"
# streaming (Opus encode + sealed UDP). Not used by the phone apps, which decode
# with the platform's own Opus decoder.

include(FetchContent)
# libopus 1.5.2 (BSD-3-Clause), official Xiph release, pinned by hash.
FetchContent_Declare(opus
  URL https://github.com/xiph/opus/releases/download/v1.5.2/opus-1.5.2.tar.gz
  URL_HASH SHA256=65c1d2f78b9f2fb20082c38cbe47c951ad5839345876e46941612ee87f9a7ce1
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
# Same static C runtime as the agent (single exe, no VC++ redistributable).
set(OPUS_STATIC_RUNTIME ON CACHE BOOL "" FORCE)
# Every Apple Silicon CPU has NEON: use it without runtime detection.
if(APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
  set(OPUS_PRESUME_NEON ON CACHE BOOL "" FORCE)
endif()
FetchContent_MakeAvailable(opus)

add_library(archon_agent_common STATIC src/opus_streamer.cpp)
target_include_directories(archon_agent_common PUBLIC src)
target_link_libraries(archon_agent_common PUBLIC archon_core opus)
if(MSVC)
  target_compile_options(archon_agent_common PRIVATE /W4 /utf-8)
else()
  target_compile_options(archon_agent_common PRIVATE -Wall -Wextra)
endif()

if(ARCHON_BUILD_TESTS)
  add_executable(archon_stream_tests tests/stream_tests.cpp ../core/tests/main.cpp)
  target_link_libraries(archon_stream_tests PRIVATE archon_agent_common)
  if(MSVC)
    target_compile_options(archon_stream_tests PRIVATE /W4 /utf-8)
  endif()
  add_test(NAME archon_stream_tests COMMAND archon_stream_tests)
endif()
