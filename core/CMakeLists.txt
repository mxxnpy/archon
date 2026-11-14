# Portable core: protocol, crypto, controller session, agent server, C API.
# Builds on Windows, Linux, macOS, iOS, Android and (later) ESP-IDF.

add_library(monocypher STATIC third_party/monocypher/monocypher.c)
target_include_directories(monocypher PUBLIC third_party/monocypher)
set_target_properties(monocypher PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(archon_core STATIC
  src/bytes.cpp
  src/protocol.cpp
  src/crypto.cpp
  src/frame.cpp
  src/net.cpp
  src/controller_session.cpp
  src/client.cpp
  src/server.cpp
  src/trust_store.cpp
  src/c_api.cpp
)
target_include_directories(archon_core PUBLIC include)
target_link_libraries(archon_core PUBLIC monocypher)
set_target_properties(archon_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

find_package(Threads REQUIRED)
target_link_libraries(archon_core PUBLIC Threads::Threads)

if(WIN32)
  target_link_libraries(archon_core PUBLIC ws2_32 bcrypt)
  target_compile_definitions(archon_core PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
endif()

if(MSVC)
  target_compile_options(archon_core PRIVATE /W4 /permissive- /utf-8)
else()
  target_compile_options(archon_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(APPLE)
  target_link_libraries(archon_core PUBLIC "-framework Security")
endif()
