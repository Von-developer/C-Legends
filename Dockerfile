# ─────────────────────────────────────────────────────────────────────────────
#  C-Legends  —  Multi-stage Dockerfile
#
#  Stage 1 (builder): compiles all C++20 sources
#  Stage 2 (runtime): lean Ubuntu image that only runs the binary
#
#  Build:  docker build -t clegends-engine .
#  Run:    docker run -p 9091:9091 clegends-engine
# ─────────────────────────────────────────────────────────────────────────────

# ── Stage 1: build ────────────────────────────────────────────────────────
FROM ubuntu:22.04 AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
        g++ \
        make \
        && rm -rf /var/lib/apt/lists/*

WORKDIR /src

# Copy all source files
COPY *.cpp *.h ./
COPY vendor/ ./vendor/

# Compile everything in one shot — matches the manual build command
RUN g++ -std=c++20 -O2 \
        -I. -Ivendor/maxminddb \
        vendor/maxminddb/maxminddb.c \
        main.cpp \
        Event.cpp \
        LogManager.cpp \
        FileHandler.cpp \
        LiveFileWatcher.cpp \
        GeoLocator.cpp \
        PrometheusExporter.cpp \
        ActivityEvent.cpp \
        ErrorEvent.cpp \
        LoginEvent.cpp \
        WarningEvent.cpp \
        ReportGenerator.cpp \
        -o log_analyzer \
        -lpthread \
    && strip log_analyzer

# ── Stage 2: runtime ──────────────────────────────────────────────────────
FROM ubuntu:22.04 AS runtime

# libstdc++ is already in the base image; we only need the C++ runtime
RUN apt-get update && apt-get install -y --no-install-recommends \
        libstdc++6 \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# Copy compiled binary from build stage
COPY --from=builder /src/log_analyzer ./

# Copy sample log data so the container has something to analyse on startup
COPY logs.csv  ./logs.csv
COPY Mac.log   ./Mac.log

# Optional: drop GeoLite2-City.mmdb here at build time for geo lookups
# COPY GeoLite2-City.mmdb ./
# Or mount it at runtime:
#   docker run -v /path/to/GeoLite2-City.mmdb:/app/GeoLite2-City.mmdb ...

# ── Ports ──────────────────────────────────────────────────────────────────
# 9091 — Prometheus /metrics scrape endpoint
EXPOSE 9091

# ── Entrypoint ─────────────────────────────────────────────────────────────
# Run in non-interactive mode: load Mac.log, start Prometheus exporter,
# then block on the live watcher.
# The wrapper script handles the menu-less "daemon" use-case.
COPY docker-entrypoint.sh ./
RUN chmod +x docker-entrypoint.sh

ENTRYPOINT ["./docker-entrypoint.sh"]
