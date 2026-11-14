add_executable(archon_tests
  main.cpp
  test_codec.cpp
  test_crypto.cpp
  test_fuzz.cpp
  test_integration.cpp
  test_trust_store.cpp
  test_features12.cpp
  adversarial/test_hostile_peer.cpp
  adversarial/test_pairing_policy.cpp
  adversarial/test_client_adv.cpp
  adversarial/test_concurrency_adv.cpp
  adversarial/test_codec_crypto_adv.cpp
  # Platform-neutral parts of the macOS agent: tested on every OS.
  ../../agent-macos/tests/test_portable.cpp
  ../../agent-macos/src/portable.cpp
)
target_link_libraries(archon_tests PRIVATE archon_core)
if(MSVC)
  target_compile_options(archon_tests PRIVATE /W4 /utf-8)
endif()
add_test(NAME archon_tests COMMAND archon_tests)
