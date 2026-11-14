cmake_minimum_required(VERSION 3.22)
project(archon_android LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Shared portable core (repo root /core).
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../../../../../core ${CMAKE_BINARY_DIR}/archon_core)

add_library(archon_jni SHARED archon_jni.cpp)
target_link_libraries(archon_jni PRIVATE archon_core log)
target_link_options(archon_jni PRIVATE -Wl,--gc-sections -Wl,-z,max-page-size=16384)
target_compile_options(archon_jni PRIVATE -Wall -Wextra -ffunction-sections -fdata-sections)
