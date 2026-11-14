add_executable(archon_agent WIN32
  src/main.cpp
  src/win_util.cpp
  src/wasapi_backend.cpp
  src/mdns.cpp
  src/ui.cpp
  app.rc
)
set_target_properties(archon_agent PROPERTIES OUTPUT_NAME "Archon")
target_link_libraries(archon_agent PRIVATE
  archon_core ole32 oleaut32 uuid crypt32 dnsapi iphlpapi shell32 shlwapi
  comctl32 windowscodecs advapi32 user32 gdi32 version propsys gdiplus dwmapi)
target_compile_definitions(archon_agent PRIVATE UNICODE _UNICODE)
if(MSVC)
  target_compile_options(archon_agent PRIVATE /W4 /permissive- /utf-8)
  target_link_options(archon_agent PRIVATE /MANIFEST:NO)
endif()

# Manual end-to-end check against the real audio stack (not part of ctest:
# needs an output device and changes/restores the system sounds volume).
add_executable(archon_wasapi_e2e tests/wasapi_e2e.cpp src/wasapi_backend.cpp src/win_util.cpp)
target_link_libraries(archon_wasapi_e2e PRIVATE archon_core ole32 oleaut32 uuid crypt32 iphlpapi shell32
  shlwapi windowscodecs advapi32 user32 gdi32 version propsys)
target_compile_definitions(archon_wasapi_e2e PRIVATE UNICODE _UNICODE)
if(MSVC)
  target_compile_options(archon_wasapi_e2e PRIVATE /W4 /utf-8)
endif()
