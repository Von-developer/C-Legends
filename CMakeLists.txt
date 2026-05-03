cmake_minimum_required(VERSION 3.20)
project(C_Legends VERSION 2.0 LANGUAGES C CXX)

# ── C++ standard ─────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# MacPorts prefix — required for find_package(CURL) on macOS with MacPorts
list(APPEND CMAKE_PREFIX_PATH "/opt/local")
# ── Build type default ────────────────────────────────────────────────────
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ── Compiler flags ────────────────────────────────────────────────────────
# -O2          : optimize for speed (light transmission)
# -fno-rtti    : saves binary size (we use dynamic_cast — remove if causing issues)
# -Wall/-Wextra: keep code clean
add_compile_options(
    $<$<COMPILE_LANGUAGE:CXX>:-Wall>
    $<$<COMPILE_LANGUAGE:CXX>:-Wextra>
    $<$<COMPILE_LANGUAGE:CXX>:-O2>
)

# ── Vendored libmaxminddb (C source, compiled as part of this build) ──────
# No system package required — everything is in vendor/maxminddb/
add_library(maxminddb_vendor STATIC
    vendor/maxminddb/maxminddb.c
)
target_include_directories(maxminddb_vendor PUBLIC
    ${CMAKE_SOURCE_DIR}/vendor/maxminddb
)
# maxminddb.c is C, not C++ — make sure it's compiled with cc not c++
set_target_properties(maxminddb_vendor PROPERTIES LINKER_LANGUAGE C)

# ── External dependencies for DShieldInformer ────────────────────────────
# nlohmann/json — header-only, fetched at configure time.
include(FetchContent)
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
)
FetchContent_MakeAvailable(nlohmann_json)

# libcurl — system package (macOS: MacPorts via /opt/local, or brew;
#                          Linux: apt install libcurl4-openssl-dev)
find_package(CURL REQUIRED)

# ── Core event / log sources ──────────────────────────────────────────────
set(CORE_SOURCES
    Event.cpp
    ActivityEvent.cpp
    ErrorEvent.cpp
    LoginEvent.cpp
    WarningEvent.cpp
    LogManager.cpp
    FileHandler.cpp
    ReportGenerator.cpp
)

# ── Live ingestion + geo + Prometheus + WebSocket + Dashboard ────────────
set(LIVE_SOURCES
    LiveFileWatcher.cpp
    GeoLocator.cpp
    PrometheusExporter.cpp
    WebSocketServer.cpp
    WebDashboardServer.cpp
    DShieldInformer.cpp
)

# ── Main executable ───────────────────────────────────────────────────────
add_executable(log_analyzer
    main.cpp
    ${CORE_SOURCES}
    ${LIVE_SOURCES}
)

target_include_directories(log_analyzer PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/vendor/maxminddb
)

target_link_libraries(log_analyzer PRIVATE
    maxminddb_vendor
    pthread       # std::jthread on Linux needs -lpthread
    CURL::libcurl
    nlohmann_json::nlohmann_json
)

# On Linux, link against libm for floating-point maths in the MMDB reader
if(UNIX AND NOT APPLE)
    target_link_libraries(log_analyzer PRIVATE m)
endif()

# ── Optional: install GeoLite2 DB next to binary ─────────────────────────
# Place GeoLite2-City.mmdb in the build directory (cmake --build .) and it
# will be picked up automatically at runtime.  Download from:
#   https://dev.maxmind.com/geoip/geolite2-free-geolocation-data
if(EXISTS ${CMAKE_SOURCE_DIR}/GeoLite2-City.mmdb)
    configure_file(
        ${CMAKE_SOURCE_DIR}/GeoLite2-City.mmdb
        ${CMAKE_BINARY_DIR}/GeoLite2-City.mmdb
        COPYONLY
    )
    message(STATUS "GeoLite2-City.mmdb found — geo lookups enabled.")
else()
    message(WARNING
        "GeoLite2-City.mmdb NOT found in ${CMAKE_SOURCE_DIR}/\n"
        "   Geo lookups will return 'Unknown' at runtime.\n"
        "   Download from https://dev.maxmind.com/geoip/geolite2-free-geolocation-data\n"
        "   and place it in ${CMAKE_SOURCE_DIR}/")
endif()

# ── UI dashboard helper ───────────────────────────────────────────────────
message(STATUS "UI dashboard → ${CMAKE_SOURCE_DIR}/dashboard/index.html")

# ── Print build summary ───────────────────────────────────────────────────
message(STATUS "")
message(STATUS "═══════════════════════════════════════")
message(STATUS " C-Legends v${PROJECT_VERSION} — Build Summary")
message(STATUS "═══════════════════════════════════════")
message(STATUS " Target      : log_analyzer")
message(STATUS " C++ standard: C++20")
message(STATUS " Build type  : ${CMAKE_BUILD_TYPE}")
message(STATUS " Prometheus  : http://0.0.0.0:9091/metrics  (starts with option 11)")
message(STATUS " Geo DB      : GeoLite2-City.mmdb  (place next to binary)")
message(STATUS " Web Server  : ./log_analyzer --serve Mac.log  → http://localhost:8080")
message(STATUS "               Options: --http-port N  --ws-port N  --admin-pass secret")
message(STATUS "═══════════════════════════════════════")
message(STATUS "")
