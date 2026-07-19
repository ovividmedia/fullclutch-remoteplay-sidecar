# SPDX-License-Identifier: AGPL-3.0-or-later
cmake_minimum_required(VERSION 3.20)
project(fc-remoteplay LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

include(FetchContent)

# ── nlohmann/json (header-only) ──────────────────────────────────────────────
FetchContent_Declare(nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG        v3.11.3)
FetchContent_MakeAvailable(nlohmann_json)

# ── libwebsockets ────────────────────────────────────────────────────────────
# Built from source, SSL off — we only serve ws:// on 127.0.0.1.
set(LWS_WITHOUT_TESTAPPS    ON  CACHE BOOL "" FORCE)
set(LWS_WITHOUT_TEST_SERVER ON  CACHE BOOL "" FORCE)
set(LWS_WITHOUT_TEST_CLIENT ON  CACHE BOOL "" FORCE)
set(LWS_WITHOUT_TEST_PING   ON  CACHE BOOL "" FORCE)
set(LWS_WITH_SHARED         OFF CACHE BOOL "" FORCE)
set(LWS_WITH_SSL            OFF CACHE BOOL "" FORCE)
set(LWS_WITH_MINIMAL_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(libwebsockets
  GIT_REPOSITORY https://github.com/warmcat/libwebsockets.git
  GIT_TAG        v4.3.3)
FetchContent_MakeAvailable(libwebsockets)

# ── chiaki-ng (git submodule) ────────────────────────────────────────────────
# We consume chiaki-ng's ROOT CMake with everything but the core lib disabled —
# that pulls in its vendored third-party (curl, jerasure, gf-complete, nanopb)
# and its cmake/Find* modules, producing the `chiaki-lib` target.
#
# External deps chiaki-lib still needs from the system / vcpkg / brew:
#   openssl, opus, json-c, libevent, miniupnpc, protobuf(+protoc), pkg-config
# (see vcpkg.json in this repo — it is chiaki-ng's manifest minus GUI deps)
set(CHIAKI_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/chiaki-ng")
if(NOT EXISTS "${CHIAKI_ROOT}/lib/CMakeLists.txt")
  message(FATAL_ERROR
    "chiaki-ng submodule not found at ${CHIAKI_ROOT}. "
    "Run: git submodule update --init --recursive")
endif()

# Disable everything a headless LAN capture sidecar doesn't need. Each of these
# is an AUTO/ON path in chiaki's root that pulls extra deps and can fail at
# configure. We need only: the core lib, opus (audio), and curl (lib links it
# unconditionally). RUDP is PSN-over-internet — we're LAN-only.
set(CHIAKI_ENABLE_GUI              OFF CACHE STRING "" FORCE)
set(CHIAKI_ENABLE_CLI             OFF CACHE BOOL "" FORCE)
set(CHIAKI_ENABLE_TESTS           OFF CACHE BOOL "" FORCE)
set(CHIAKI_ENABLE_ANDROID         OFF CACHE BOOL "" FORCE)
set(CHIAKI_ENABLE_BOREALIS        OFF CACHE BOOL "" FORCE)
set(CHIAKI_ENABLE_STEAM_SHORTCUT  OFF CACHE BOOL "" FORCE)
set(CHIAKI_ENABLE_SETSU           OFF CACHE STRING "" FORCE)
set(CHIAKI_ENABLE_STEAMDECK_NATIVE OFF CACHE STRING "" FORCE)
set(CHIAKI_ENABLE_SPEEX           OFF CACHE STRING "" FORCE)
set(CHIAKI_ENABLE_RUDP            OFF CACHE STRING "" FORCE)
set(CHIAKI_ENABLE_FFMPEG_DECODER  OFF CACHE STRING "" FORCE)
set(CHIAKI_ENABLE_PI_DECODER      OFF CACHE STRING "" FORCE)
set(CHIAKI_LIB_ENABLE_OPUS        ON  CACHE BOOL "" FORCE)
add_subdirectory("${CHIAKI_ROOT}" chiaki-ng EXCLUDE_FROM_ALL)

# ── fc-remoteplay executable ─────────────────────────────────────────────────
add_executable(fc-remoteplay
  src/main.cpp
  src/ws_server.cpp
  src/session_bridge.cpp)

target_link_libraries(fc-remoteplay PRIVATE
  chiaki-lib
  websockets
  nlohmann_json::nlohmann_json)

if(WIN32)
  target_link_libraries(fc-remoteplay PRIVATE ws2_32 iphlpapi)
endif()

install(TARGETS fc-remoteplay RUNTIME DESTINATION bin)
