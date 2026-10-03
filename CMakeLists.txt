cmake_minimum_required(VERSION 3.16)
project(zigbee2supla VERSION 0.3.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

option(Z2S_BUILD_TESTS "Build unit tests" ON)
option(Z2S_SANITIZE "Build with AddressSanitizer and UBSan" OFF)

if(Z2S_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

find_package(OpenSSL REQUIRED)
find_package(nlohmann_json 3 REQUIRED)
find_package(Threads REQUIRED)
find_package(yaml-cpp REQUIRED)
# yaml-cpp 0.8 exports a namespaced target, older versions a plain one.
if(TARGET yaml-cpp::yaml-cpp)
  set(YAML_CPP_LIB yaml-cpp::yaml-cpp)
else()
  set(YAML_CPP_LIB yaml-cpp)
endif()
find_library(MOSQUITTO_LIB mosquitto REQUIRED)
find_path(MOSQUITTO_INCLUDE_DIR mosquitto.h REQUIRED)

# --- supla-common (SRPC protocol, vendored from supla-device) ---------------
add_library(supla_common STATIC
  third_party/supla-common/srpc.c
  third_party/supla-common/proto.c
  third_party/supla-common/lck.c
)
target_include_directories(supla_common PUBLIC third_party/supla-common)
# SUPLA_DEVICE selects the device-side, single-threaded build of srpc
# (no internal queues, no event handler).
target_compile_definitions(supla_common PUBLIC SUPLA_DEVICE)
target_compile_options(supla_common PRIVATE -w)

# --- core: platform independent gateway logic -------------------------------
add_library(z2s_core STATIC
  core/src/log.cpp
  core/src/channel.cpp
  core/src/identity_store.cpp
  core/src/supla_session.cpp
  core/src/gateway.cpp
  core/src/supla_ca_cert.cpp
)
target_include_directories(z2s_core PUBLIC core/include)
target_link_libraries(z2s_core PUBLIC supla_common nlohmann_json::nlohmann_json)
target_compile_definitions(z2s_core PUBLIC Z2S_VERSION="${PROJECT_VERSION}")
target_compile_options(z2s_core PRIVATE -Wall -Wextra)

# --- Linux platform (OpenSSL TLS transport) ---------------------------------
add_library(z2s_platform_linux STATIC
  platform/linux/openssl_transport.cpp
  platform/linux/websocket_client.cpp
)
target_include_directories(z2s_platform_linux PUBLIC platform/linux)
target_link_libraries(z2s_platform_linux PUBLIC z2s_core OpenSSL::SSL OpenSSL::Crypto)
target_compile_options(z2s_platform_linux PRIVATE -Wall -Wextra)

# --- Home Assistant integration (platform independent logic) ----------------
add_library(z2s_ha STATIC
  integrations/ha/ha_duplicates.cpp
)
target_include_directories(z2s_ha PUBLIC integrations/ha)
target_link_libraries(z2s_ha PUBLIC nlohmann_json::nlohmann_json)
target_compile_options(z2s_ha PRIVATE -Wall -Wextra)

# --- zigbee2mqtt backend -----------------------------------------------------
add_library(z2s_backend_z2m STATIC
  backends/z2m/z2m_mapper.cpp
  backends/z2m/z2m_backend.cpp
)
target_include_directories(z2s_backend_z2m PUBLIC backends/z2m ${MOSQUITTO_INCLUDE_DIR})
target_link_libraries(z2s_backend_z2m PUBLIC z2s_core ${MOSQUITTO_LIB} Threads::Threads)
target_compile_options(z2s_backend_z2m PRIVATE -Wall -Wextra)

# --- ZHA backend (Home Assistant WebSocket API) ------------------------------
add_library(z2s_backend_zha STATIC
  backends/zha/zha_mapper.cpp
  backends/zha/zha_backend.cpp
)
target_include_directories(z2s_backend_zha PUBLIC backends/zha)
target_link_libraries(z2s_backend_zha PUBLIC z2s_core z2s_platform_linux
  Threads::Threads)
target_compile_options(z2s_backend_zha PRIVATE -Wall -Wextra)

# --- Linux application -------------------------------------------------------
add_executable(zigbee2supla
  apps/linux/main.cpp
  apps/linux/config.cpp
  apps/linux/ha_sync.cpp
)
target_include_directories(zigbee2supla PRIVATE apps/linux)
target_link_libraries(zigbee2supla PRIVATE z2s_core z2s_platform_linux z2s_backend_z2m
  z2s_backend_zha z2s_ha ${YAML_CPP_LIB})
target_compile_options(zigbee2supla PRIVATE -Wall -Wextra)
install(TARGETS zigbee2supla RUNTIME DESTINATION bin)

if(Z2S_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
