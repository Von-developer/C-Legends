#ifndef LIVEFILEWATCHER_H
#define LIVEFILEWATCHER_H

// ─────────────────────────────────────────────────────────────────────────────
//  LiveFileWatcher  (C++20)
//
//  Monitors a single log file for new lines without re-reading the entire file.
//  Uses a std::jthread (C++20) that polls std::filesystem::file_size at a
//  configurable interval.  When the file grows, it seeks to the last known
//  position and reads only the new bytes — zero-copy line slicing via
//  std::string_view where possible.
//
//  Producer side of the Producer-Consumer pattern:
//    • Parses each new line into an Event* (reuses FileHandler's parsers)
//    • Extracts any IPv4 address from the raw line
//    • Pushes the enriched Event* into LogManager's thread-safe queue
// ─────────────────────────────────────────────────────────────────────────────

#include "LogManager.h"
#include "GeoLocator.h"
#include <string>
#include <filesystem>
#include <thread>
#include <atomic>
#include <chrono>
#include <memory>

class LiveFileWatcher {
public:
    // poll_interval — how often to check the file for new content
    // geoDbPath     — path to GeoLite2-City.mmdb (empty = use default)
    explicit LiveFileWatcher(const std::string&        filePath,
                             LogManager&               manager,
                             std::chrono::milliseconds pollInterval
                                 = std::chrono::milliseconds(500),
                             std::string_view          geoDbPath
                                 = "GeoLite2-City.mmdb");
    ~LiveFileWatcher();

    // Non-copyable
    LiveFileWatcher(const LiveFileWatcher&)            = delete;
    LiveFileWatcher& operator=(const LiveFileWatcher&) = delete;

    void start();
    void stop();
    bool isRunning() const { return running.load(); }

private:
    std::filesystem::path     watchPath;
    LogManager&               manager;
    std::chrono::milliseconds pollInterval;
    std::unique_ptr<GeoLocator> geoLocator;   // owned geo lookup engine

    std::atomic<bool> running{false};
    std::jthread      watchThread;

    // Position of the last byte we have already consumed
    std::streampos lastPos{0};

    // ── Watcher loop (runs on watchThread) ──────────────────────────────
    void watchLoop(std::stop_token st);

    // ── Parse a single raw log line into an Event* ───────────────────────
    // Mirrors FileHandler logic; returns nullptr if the line is unparseable.
    Event* parseLine(const std::string& rawLine) const;

    // ── Helpers ──────────────────────────────────────────────────────────
    bool isMacLog()   const;
    std::string classifyLine(const std::string& process,
                             const std::string& message) const;
};

#endif
