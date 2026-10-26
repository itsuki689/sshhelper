cmake_minimum_required(VERSION 3.24)
project(AirSFTPMonitor VERSION 2.0.0 LANGUAGES C CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ENABLE_ZLIB_COMPRESSION OFF CACHE BOOL "" FORCE)
set(ENABLE_DEBUG_LOGGING OFF CACHE BOOL "" FORCE)
set(CRYPTO_BACKEND WinCNG CACHE STRING "" FORCE)
set(ENABLE_UNICODE ON CACHE BOOL "Use wide Windows APIs in libssh2" FORCE)

FetchContent_Declare(
  libssh2
  URL https://codeload.github.com/libssh2/libssh2/zip/250045f77ad9c906f65d9071d134bf9f8f9652db
  URL_HASH SHA256=d8af35ecba6958bc2293951c448a58f4913c03d6614d5f37c1cc923c03108d43
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(libssh2)

set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
set(JSON_Install OFF CACHE BOOL "" FORCE)
FetchContent_Declare(json
  URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
  URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(json)

add_library(airsftp_core STATIC
  src/credential_store.cpp
  src/ssh_client.cpp
  src/host_trust.cpp
  src/transfer_engine.cpp
  src/folder_transfer.cpp
  src/telemetry.cpp
  src/local_telemetry.cpp
  src/restart_process.cpp
)
target_include_directories(airsftp_core PUBLIC src)
target_link_libraries(airsftp_core PUBLIC libssh2::libssh2 ws2_32 crypt32 bcrypt iphlpapi psapi wbemuuid ole32 oleaut32 advapi32 pdh)
target_compile_definitions(airsftp_core PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

add_executable(AirSFTP WIN32
  src/main.cpp
  src/app.cpp
  src/app_servers.cpp
  src/app_tray.cpp
  src/host_trust_ui.cpp
  src/transfer_picker.cpp
  resources/AirSFTP.rc
)
target_include_directories(AirSFTP PRIVATE resources)
target_link_libraries(AirSFTP PRIVATE airsftp_core d2d1 dwrite dwmapi comdlg32 shell32 ole32 comctl32 uuid)

if(MINGW)
  target_link_options(AirSFTP PRIVATE -static -static-libgcc -static-libstdc++ -municode)
endif()

add_library(sshhelper_api STATIC src/monitor_api.cpp src/mcp_server.cpp)
target_link_libraries(sshhelper_api PUBLIC airsftp_core nlohmann_json::nlohmann_json)
add_executable(sshhelper-tool src/tool_main.cpp)
target_link_libraries(sshhelper-tool PRIVATE sshhelper_api)
if(MINGW)
  target_link_options(sshhelper-tool PRIVATE -static -static-libgcc -static-libstdc++ -municode)
endif()

add_executable(telemetry_parser_test tests/telemetry_parser_test.cpp)
target_link_libraries(telemetry_parser_test PRIVATE airsftp_core)

add_executable(credential_store_test tests/credential_store_test.cpp)
target_link_libraries(credential_store_test PRIVATE airsftp_core)

add_executable(ssh_client_test tests/ssh_client_test.cpp)
target_link_libraries(ssh_client_test PRIVATE airsftp_core)

enable_testing()
add_executable(monitor_api_test tests/monitor_api_test.cpp)
target_link_libraries(monitor_api_test PRIVATE sshhelper_api)
add_test(NAME monitor_api COMMAND monitor_api_test)
set_tests_properties(monitor_api PROPERTIES TIMEOUT 15)
add_executable(mcp_server_test tests/mcp_server_test.cpp)
target_link_libraries(mcp_server_test PRIVATE sshhelper_api)
add_test(NAME mcp_server COMMAND mcp_server_test)
set_tests_properties(mcp_server PROPERTIES TIMEOUT 10)
find_program(SSHHELPER_TEST_NODE node)
if(SSHHELPER_TEST_NODE)
  add_test(NAME mcp_stdio COMMAND "${SSHHELPER_TEST_NODE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/mcp_stdio_test.mjs" "$<TARGET_FILE:sshhelper-tool>")
  set_tests_properties(mcp_stdio PROPERTIES TIMEOUT 20)
endif()
add_test(NAME telemetry_parser COMMAND telemetry_parser_test)
add_executable(local_telemetry_test tests/local_telemetry_test.cpp)
target_link_libraries(local_telemetry_test PRIVATE airsftp_core)
add_test(NAME local_telemetry COMMAND local_telemetry_test)
set_tests_properties(local_telemetry PROPERTIES TIMEOUT 30)
add_executable(restart_process_test tests/restart_process_test.cpp)
target_link_libraries(restart_process_test PRIVATE airsftp_core)
add_test(NAME restart_process COMMAND restart_process_test)
set_tests_properties(restart_process PROPERTIES TIMEOUT 15)
add_test(NAME credential_store COMMAND credential_store_test)
add_test(NAME ssh_client COMMAND ssh_client_test)
set_tests_properties(ssh_client PROPERTIES TIMEOUT 10)

if(MINGW)
  add_executable(host_trust_test tests/host_trust_test.cpp)
  target_link_libraries(host_trust_test PRIVATE sshhelper_api)
  foreach(api session_handshake hostkey_hash userauth_password_ex)
    target_link_options(host_trust_test PRIVATE "-Wl,--wrap=libssh2_${api}")
  endforeach()
  add_test(NAME host_trust COMMAND host_trust_test)
  set_tests_properties(host_trust PROPERTIES TIMEOUT 30)

  add_executable(transfer_test tests/transfer_test.cpp)
  target_link_libraries(transfer_test PRIVATE airsftp_core)
  # Isolate the test-only pinned host key from the application.
  set_target_properties(transfer_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/transfer-test")
  foreach(api session_handshake hostkey_hash userauth_password_ex sftp_init sftp_shutdown
              sftp_open_ex sftp_fstat_ex sftp_read sftp_write sftp_close_handle sftp_stat_ex
              sftp_mkdir_ex sftp_last_error sftp_readdir_ex sftp_seek64 sftp_fsync sftp_posix_rename_ex)
    target_link_options(transfer_test PRIVATE "-Wl,--wrap=libssh2_${api}")
  endforeach()
  add_test(NAME transfer COMMAND transfer_test)
  set_tests_properties(transfer PROPERTIES TIMEOUT 10)
  add_executable(folder_transfer_test tests/folder_transfer_test.cpp)
  target_link_libraries(folder_transfer_test PRIVATE airsftp_core)
  set_target_properties(folder_transfer_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/folder-test")
  foreach(api session_handshake hostkey_hash userauth_password_ex sftp_init sftp_shutdown
              sftp_open_ex sftp_fstat_ex sftp_read sftp_write sftp_close_handle sftp_stat_ex
              sftp_mkdir_ex sftp_last_error sftp_readdir_ex sftp_seek64 sftp_fsync sftp_posix_rename_ex)
    target_link_options(folder_transfer_test PRIVATE "-Wl,--wrap=libssh2_${api}")
  endforeach()
  add_test(NAME folder_transfer COMMAND folder_transfer_test)
  set_tests_properties(folder_transfer PROPERTIES TIMEOUT 15)

endif()

add_executable(app_windows_test tests/app_windows_test.cpp src/app.cpp src/app_servers.cpp src/app_tray.cpp src/host_trust_ui.cpp src/transfer_picker.cpp resources/AirSFTP.rc)
target_include_directories(app_windows_test PRIVATE resources)
target_link_libraries(app_windows_test PRIVATE airsftp_core d2d1 dwrite dwmapi comdlg32 shell32 ole32 comctl32 uuid)
set_target_properties(app_windows_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/window-test")
add_test(NAME app_windows COMMAND app_windows_test)
set_tests_properties(app_windows PROPERTIES TIMEOUT 15)

if(MINGW)
  add_executable(app_host_trust_test tests/app_host_trust_test.cpp src/app.cpp src/app_servers.cpp
    src/app_tray.cpp src/host_trust_ui.cpp src/transfer_picker.cpp resources/AirSFTP.rc)
  target_include_directories(app_host_trust_test PRIVATE resources)
  target_link_libraries(app_host_trust_test PRIVATE airsftp_core d2d1 dwrite dwmapi comdlg32 shell32 ole32 comctl32 uuid)
  set_target_properties(app_host_trust_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/host-trust-window-test")
  foreach(api session_handshake hostkey_hash userauth_password_ex)
    target_link_options(app_host_trust_test PRIVATE "-Wl,--wrap=libssh2_${api}")
  endforeach()
  add_test(NAME app_host_trust COMMAND app_host_trust_test)
  set_tests_properties(app_host_trust PROPERTIES TIMEOUT 20)
endif()

# Manual integration test; never connects to a server during CTest.
add_executable(live_transfer_test EXCLUDE_FROM_ALL tests/live_transfer_test.cpp)
target_link_libraries(live_transfer_test PRIVATE airsftp_core)
set_target_properties(live_transfer_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/live-test")

# Manual, read-only sensor diagnostics using an explicitly selected saved server.
add_executable(live_telemetry_test EXCLUDE_FROM_ALL tests/live_telemetry_test.cpp)
target_link_libraries(live_telemetry_test PRIVATE airsftp_core)
set_target_properties(live_telemetry_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/live-test")

install(TARGETS AirSFTP sshhelper-tool RUNTIME DESTINATION .)
