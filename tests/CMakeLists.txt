find_package(GTest REQUIRED)

add_executable(z2s_tests
  channel_test.cpp
  config_test.cpp
  ${CMAKE_SOURCE_DIR}/apps/linux/config.cpp
  ${CMAKE_SOURCE_DIR}/apps/linux/ha_sync.cpp
  ha_duplicates_test.cpp
  ha_sync_test.cpp
  proto_contract_test.cpp
  identity_store_test.cpp
  openssl_transport_test.cpp
  z2m_mapper_test.cpp
  z2m_backend_test.cpp
  session_test.cpp
  fake_supla_server.cpp
)
target_link_libraries(z2s_tests PRIVATE
  z2s_core z2s_backend_z2m z2s_platform_linux z2s_ha ${YAML_CPP_LIB}
  GTest::gtest_main)
target_include_directories(z2s_tests PRIVATE ${CMAKE_SOURCE_DIR}/apps/linux)
target_compile_definitions(z2s_tests PRIVATE
  Z2S_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")

include(GoogleTest)
gtest_discover_tests(z2s_tests)
