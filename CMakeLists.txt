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
# Prefer a system/vcpkg package; fall back to building it from source.
find_package(libwebsockets QUIET)
if(NOT libwebsockets_FOUND)
  set(LWS_WITHOUT_TESTAPPS   ON  CACHE BOOL "" FORCE)
  set(LWS_WITHOUT_TEST_SERVER ON CACHE BOOL "" FORCE)
  set(LWS_WITH_SHARED        OFF CACHE BOOL "" FORCE)
  set(LWS_WITH_SSL           OFF CACHE BOOL "" FORCE)  # localhost only
  FetchContent_Declare(libwebsockets
    GIT_REPOSITORY https://github.com/warmcat/libwebsockets.git
    GIT_TAG        v4.3.3)
  FetchContent_MakeAvailable(libwebsockets)
  set(LWS_TARGET websockets)
else()
  set(LWS_TARGET websockets)
endif()

# ── chiaki-ng core (git submodule) ───────────────────────────────────────────
# We build ONLY the C library (lib/), never the Qt GUI. The submodule lives at
# third_party/chiaki-ng — run `git submodule update --init --recursive` first.
set(CHIAKI_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/chiaki-ng")
if(NOT EXISTS "${CHIAKI_ROOT}/lib/CMakeLists.txt")
  message(FATAL_ERROR
    "chiaki-ng submodule not found at ${CHIAKI_ROOT}. "
    "Run: git submodule update --init --recursive")
endif()

# Turn off everything except the core library so we don't drag in Qt/SDL/etc.
set(CHIAKI_ENABLE_GUI      OFF CACHE BOOL "" FORCE)
set(CHIAKI_ENABLE_CLI      OFF CACHE BOOL "" FORCE)
set(CHIAKI_ENABLE_TESTS    OFF CACHE BOOL "" FORCE)
set(CHIAKI_ENABLE_ANDROID  OFF CACHE BOOL "" FORCE)
set(CHIAKI_LIB_ENABLE_OPUS ON  CACHE BOOL "" FORCE)
add_subdirectory("${CHIAKI_ROOT}/lib" chiaki-lib EXCLUDE_FROM_ALL)

# ── fc-remoteplay executable ─────────────────────────────────────────────────
add_executable(fc-remoteplay
  src/main.cpp
  src/ws_server.cpp
  src/session_bridge.cpp)

target_include_directories(fc-remoteplay PRIVATE
  "${CHIAKI_ROOT}/lib/include")

target_link_libraries(fc-remoteplay PRIVATE
  chiaki-lib               # API: target name exported by chiaki-ng lib/ (may be `chiaki` on some tags)
  ${LWS_TARGET}
  nlohmann_json::nlohmann_json)

if(WIN32)
  target_link_libraries(fc-remoteplay PRIVATE ws2_32 iphlpapi)
endif()

install(TARGETS fc-remoteplay RUNTIME DESTINATION bin)
