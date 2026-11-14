add_executable(archon_agent WIN32
  src/main.cpp
  src/win_util.cpp
  src/wasapi_backend.cpp
  src/mdns.cpp
  src/ui.cpp
  src/app_routing.cpp
  src/loopback_stream.cpp
  src/tab_pipe.cpp
  src/win_proc.cpp
  app.rc
)
set_target_properties(archon_agent PROPERTIES OUTPUT_NAME "Archon")
target_link_libraries(archon_agent PRIVATE
  archon_core archon_agent_common ole32 oleaut32 uuid crypt32 dnsapi iphlpapi shell32 shlwapi
  comctl32 windowscodecs advapi32 user32 gdi32 version propsys gdiplus dwmapi secur32 runtimeobject)
target_compile_definitions(archon_agent PRIVATE UNICODE _UNICODE)
if(MSVC)
  target_compile_options(archon_agent PRIVATE /W4 /permissive- /utf-8)
  target_link_options(archon_agent PRIVATE /MANIFEST:NO)
endif()

# Native-messaging host for the browser extension (1.4): a tiny console
# program the browser starts; it relays to Archon.exe over a per-user pipe.
add_executable(archon_tabs_host src/tabs_host_main.cpp src/win_proc.cpp ../agent-common/src/mini_json.cpp)
set_target_properties(archon_tabs_host PROPERTIES OUTPUT_NAME "archon-tabs-host")
target_include_directories(archon_tabs_host PRIVATE ../agent-common/src)
target_link_libraries(archon_tabs_host PRIVATE advapi32)
target_compile_definitions(archon_tabs_host PRIVATE UNICODE _UNICODE)
if(MSVC)
  target_compile_options(archon_tabs_host PRIVATE /W4 /permissive- /utf-8)
endif()

# Manual end-to-end check of the tabs bridge (host + pipe + TabSources). Built
# as Archon.exe in its own folder beside a copy of the host, because the host
# only talks to the Archon.exe next to it.
add_executable(archon_tabs_e2e tests/tabs_e2e.cpp src/tab_pipe.cpp src/win_proc.cpp src/win_util.cpp)
set_target_properties(archon_tabs_e2e PROPERTIES OUTPUT_NAME "Archon"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tabs-e2e")
target_link_libraries(archon_tabs_e2e PRIVATE archon_core archon_agent_common ole32 oleaut32 uuid crypt32 iphlpapi
  shell32 shlwapi windowscodecs advapi32 user32 gdi32 version propsys secur32)
target_include_directories(archon_tabs_e2e PRIVATE src)
target_compile_definitions(archon_tabs_e2e PRIVATE UNICODE _UNICODE)
add_dependencies(archon_tabs_e2e archon_tabs_host)
add_custom_command(TARGET archon_tabs_e2e POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:archon_tabs_host> $<TARGET_FILE_DIR:archon_tabs_e2e>)
if(MSVC)
  target_compile_options(archon_tabs_e2e PRIVATE /W4 /utf-8)
endif()

# Manual end-to-end check against the real audio stack (not part of ctest:
# needs an output device and changes/restores the system sounds volume).
add_executable(archon_wasapi_e2e tests/wasapi_e2e.cpp src/wasapi_backend.cpp src/win_util.cpp src/app_routing.cpp
  src/loopback_stream.cpp)
target_link_libraries(archon_wasapi_e2e PRIVATE archon_core archon_agent_common ole32 oleaut32 uuid crypt32 iphlpapi shell32
  shlwapi windowscodecs advapi32 user32 gdi32 version propsys secur32 runtimeobject)
target_compile_definitions(archon_wasapi_e2e PRIVATE UNICODE _UNICODE)
if(MSVC)
  target_compile_options(archon_wasapi_e2e PRIVATE /W4 /utf-8)
endif()

# Manual check of per-app output routing on this Windows build (pins only the
# probe process itself, then clears it).
add_executable(archon_routing_probe tests/routing_probe.cpp src/app_routing.cpp)
target_link_libraries(archon_routing_probe PRIVATE ole32 runtimeobject)
target_compile_definitions(archon_routing_probe PRIVATE UNICODE _UNICODE)
