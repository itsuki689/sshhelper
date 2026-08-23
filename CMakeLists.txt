cmake_minimum_required(VERSION 3.24)
project(AirSFTPMonitor VERSION 2.0.0 LANGUAGES C CXX)

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

FetchContent_Declare(
  libssh2
  URL https://codeload.github.com/libssh2/libssh2/zip/250045f77ad9c906f65d9071d134bf9f8f9652db
  URL_HASH SHA256=d8af35ecba6958bc2293951c448a58f4913c03d6614d5f37c1cc923c03108d43
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(libssh2)

add_library(airsftp_core STATIC
  src/ssh_client.cpp
  src/telemetry.cpp
)
target_include_directories(airsftp_core PUBLIC src)
target_link_libraries(airsftp_core PUBLIC libssh2::libssh2 ws2_32 crypt32 bcrypt)
target_compile_definitions(airsftp_core PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

add_executable(AirSFTP WIN32
  src/main.cpp
  src/app.cpp
)
target_link_libraries(AirSFTP PRIVATE airsftp_core d2d1 dwrite dwmapi comdlg32 shell32 ole32)

if(MINGW)
  target_link_options(AirSFTP PRIVATE -static -static-libgcc -static-libstdc++ -municode)
endif()

add_executable(telemetry_parser_test tests/telemetry_parser_test.cpp)
target_link_libraries(telemetry_parser_test PRIVATE airsftp_core)

enable_testing()
add_test(NAME telemetry_parser COMMAND telemetry_parser_test)

install(TARGETS AirSFTP RUNTIME DESTINATION .)
