#include "LiveFileWatcher.h"
#include "LoginEvent.h"
#include "ErrorEvent.h"
#include "WarningEvent.h"
#include "ActivityEvent.h"
#include "Utility.h"

#include <fstream>
#include <sstream>
#include <iostream>
#include <regex>
#include <algorithm>
#include <cctype>
#include <iomanip>

// ── Constructor / Destructor ──────────────────────────────────────────────
LiveFileWatcher::LiveFileWatcher(const std::string&        filePath,
                                  LogManager&               mgr,
                                  std::chrono::milliseconds interval,
                                  std::string_view          geoDbPath)
    : watchPath(filePath), manager(mgr), pollInterval(interval),
      geoLocator(std::make_unique<GeoLocator>(geoDbPath)) {}

LiveFileWatcher::~LiveFileWatcher() {
    stop();
}

// ── start / stop ──────────────────────────────────────────────────────────
void LiveFileWatcher::start() {
    if (running.exchange(true)) return;   // already running

    // Seek to end of file so we only process *new* content from here on
    if (std::filesystem::exists(watchPath)) {
        lastPos = static_cast<std::streampos>(
            std::filesystem::file_size(watchPath));
    }

    manager.startProcessing();   // ensure consumer is live

    watchThread = std::jthread([this](std::stop_token st){
        watchLoop(st);
    });

    std::cout << "[LiveFileWatcher] Watching: " << watchPath
              << "  (poll every " << pollInterval.count() << " ms)\n";
}

void LiveFileWatcher::stop() {
    if (!running.exchange(false)) return;
    watchThread.request_stop();
    watchThread = std::jthread{};   // joins automatically
    manager.stopProcessing();
    std::cout << "[LiveFileWatcher] Stopped watching: " << watchPath << "\n";
}

// ── Watch loop ────────────────────────────────────────────────────────────
void LiveFileWatcher::watchLoop(std::stop_token st) {
    while (!st.stop_requested()) {
        std::this_thread::sleep_for(pollInterval);
        if (st.stop_requested()) break;

        // Check file existence
        std::error_code ec;
        if (!std::filesystem::exists(watchPath, ec)) continue;

        // Check if the file has grown
        const auto currentSize =
            static_cast<std::streampos>(std::filesystem::file_size(watchPath, ec));
        if (ec || currentSize <= lastPos) continue;

        // ── Open and seek to last known position ──────────────────────────
        std::ifstream file(watchPath, std::ios::binary);
        if (!file.is_open()) continue;

        file.seekg(lastPos);

        std::string line;
        while (std::getline(file, line)) {
            // Strip Windows CR if present
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (line.empty() || line.front() == '#') continue;

            // ── Extract IPv4 before parsing (zero-copy via string_view) ──
            std::string ip = Event::extractIPv4(std::string_view{line});

            // ── Parse into an Event* ──────────────────────────────────────
            Event* e = parseLine(line);
            if (!e) continue;

            // ── Enrich with IP address ────────────────────────────────────
            if (!ip.empty()) {
                e->setIPAddress(ip);

                // ── Geo-enrich: IP → lat/lon/city/country ────────────────
                // geoLocator is always non-null; falls back gracefully if
                // the .mmdb file was not found at startup.
                GeoResult geo = geoLocator->lookup(ip);
                e->setGeo(geo.latitude, geo.longitude,
                          geo.city, geo.country, geo.isoCode);
            }

            // ── Push to LogManager's consumer queue ───────────────────────
            manager.pushEvent(e);
        }

        // Update position to wherever the stream ended
        lastPos = file.tellg();
    }
}

// ── Helpers ───────────────────────────────────────────────────────────────
bool LiveFileWatcher::isMacLog() const {
    const auto& s = watchPath.string();
    return s.size() >= 4 && s.substr(s.size() - 4) == ".log";
}

std::string LiveFileWatcher::classifyLine(const std::string& process,
                                           const std::string& message) const {
    std::string msgL = message, procL = process;
    std::transform(msgL.begin(),  msgL.end(),  msgL.begin(),  ::tolower);
    std::transform(procL.begin(), procL.end(), procL.begin(), ::tolower);

    if (msgL.find("wake")  != std::string::npos ||
        msgL.find("sleep") != std::string::npos ||
        msgL.find("login") != std::string::npos ||
        msgL.find("auth")  != std::string::npos ||
        procL.find("loginwindow") != std::string::npos)
        return "Login";

    if (msgL.find("error") != std::string::npos ||
        msgL.find("fail")  != std::string::npos ||
        msgL.find("crash") != std::string::npos ||
        msgL.find("fault") != std::string::npos)
        return "Error";

    if (msgL.find("warn")     != std::string::npos ||
        msgL.find("critical") != std::string::npos ||
        msgL.find("exceeded") != std::string::npos)
        return "Warning";

    return "Activity";
}

// ── Line parser ───────────────────────────────────────────────────────────
Event* LiveFileWatcher::parseLine(const std::string& rawLine) const {
    // ── Static counter for unique IDs (thread-safe via atomic) ───────────
    static std::atomic<int> counter{0};
    auto makeID = []{
        std::ostringstream s;
        s << "W" << std::setw(5) << std::setfill('0') << (++counter);
        return s.str();
    };

    if (isMacLog()) {
        // macOS syslog: "Mon DD HH:MM:SS hostname process[pid]: message"
        static const std::regex logRe(
            R"(^(\w+\s+\d+\s+\d+:\d+:\d+)\s+(\S+)\s+([^\[]+)\[(\d+)\]:\s*(.*))");
        std::smatch m;
        if (!std::regex_match(rawLine, m, logRe)) return nullptr;

        std::string ts      = m[1].str();
        std::string host    = m[2].str();
        std::string process = m[3].str();
        std::string message = m[5].str();
        while (!process.empty() && process.back() == ' ') process.pop_back();

        // Apply date correction immediately
        std::string fullTs  = ensureISO8601(ts);
        std::string type    = classifyLine(process, message);
        std::string id      = makeID();
        std::string desc    = message.size() > 80
                              ? message.substr(0, 77) + "..."
                              : message;

        if (type == "Login") {
            bool ok = message.find("fail") == std::string::npos;
            auto* e = new LoginEvent(id, ts, host, desc, ok, process);
            e->setFullTimestamp(fullTs);
            return e;
        }
        if (type == "Error") {
            int code = 0;
            std::smatch cm;
            static const std::regex codeRe(R"(\b(\d{3,5})\b)");
            if (std::regex_search(message, cm, codeRe))
                code = std::stoi(cm[1].str());
            auto* e = new ErrorEvent(id, ts, host, desc, code, process);
            e->setFullTimestamp(fullTs);
            return e;
        }
        if (type == "Warning") {
            std::string ml = message;
            std::transform(ml.begin(), ml.end(), ml.begin(), ::tolower);
            std::string sev = ml.find("critical") != std::string::npos ? "HIGH"
                            : ml.find("warn")     != std::string::npos ? "MEDIUM"
                            : "LOW";
            auto* e = new WarningEvent(id, ts, host, desc, sev, process);
            e->setFullTimestamp(fullTs);
            return e;
        }
        // Activity
        std::string ml = message;
        std::transform(ml.begin(), ml.end(), ml.begin(), ::tolower);
        std::string action = ml.find("deny")  != std::string::npos ? "DENY"
                           : ml.find("write") != std::string::npos ? "WRITE"
                           : ml.find("read")  != std::string::npos ? "READ"
                           : "SYSTEM";
        auto* e = new ActivityEvent(id, ts, host, desc, action, process);
        e->setFullTimestamp(fullTs);
        return e;

    } else {
        // CSV format: EventID,Timestamp,User,Type,Desc,Extra1,Extra2
        std::stringstream ss(rawLine);
        std::string id, ts, user, type, desc, extra1, extra2;
        try {
            std::getline(ss, id,     ',');
            std::getline(ss, ts,     ',');
            std::getline(ss, user,   ',');
            std::getline(ss, type,   ',');
            std::getline(ss, desc,   ',');
            std::getline(ss, extra1, ',');
            std::getline(ss, extra2);
        } catch (...) { return nullptr; }

        std::string fullTs = ensureISO8601(ts);

        Event* e = nullptr;
        if (type == "Login")
            e = new LoginEvent(id, ts, user, desc, extra1 == "true", extra2);
        else if (type == "Error") {
            int code = 0;
            try { code = std::stoi(extra1); } catch (...) {}
            e = new ErrorEvent(id, ts, user, desc, code, extra2);
        } else if (type == "Warning")
            e = new WarningEvent(id, ts, user, desc, extra1, extra2);
        else if (type == "Activity")
            e = new ActivityEvent(id, ts, user, desc, extra1, extra2);

        if (e) e->setFullTimestamp(fullTs);
        return e;
    }
}
