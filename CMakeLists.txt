cmake_minimum_required(VERSION 3.20)
project(archon VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# Static CRT on Windows: the agent ships as a single exe, no VC++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

option(ARCHON_BUILD_TESTS "Build core tests" ON)
option(ARCHON_BUILD_AGENT "Build the desktop agent for this OS" ON)

add_subdirectory(core)

if(ARCHON_BUILD_TESTS)
  enable_testing()
  add_subdirectory(core/tests)
endif()

if(ARCHON_BUILD_AGENT AND WIN32)
  add_subdirectory(agent-windows)
endif()

# Reference command-line controller (testing without a phone; template for ESP32 firmware).
add_executable(archon_cli tools/archon_cli.cpp)
set_target_properties(archon_cli PROPERTIES OUTPUT_NAME "archon-cli")
target_link_libraries(archon_cli PRIVATE archon_core)
