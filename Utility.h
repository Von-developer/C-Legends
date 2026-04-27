#ifndef UTILITY_H
#define UTILITY_H

#include "Event.h"
#include <vector>
#include <string>
#include <string_view>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <regex>

// ── Generic search template ───────────────────────────────────────────────
// Works for any getter that returns a comparable type.
// Example usage:  searchBy(logs, &Event::getType, std::string("Error"))
template <typename T>
std::vector<Event*> searchBy(const std::vector<Event*>& logs,
                              T (Event::*getter)() const,
                              const T& target) {
    std::vector<Event*> results;
    for (Event* e : logs) {
        if ((e->*getter)() == target)
            results.push_back(e);
    }
    return results;
}

// ── Date-correction utility ───────────────────────────────────────────────
// Takes a timestamp string (may be "HH:MM:SS", "HH:MM:SS.mmm", or already
// a full date+time).  If only a time portion is present, prepends today's
// date to produce an ISO 8601 string: "YYYY-MM-DDTHH:MM:SSZ".
// Uses std::string_view for zero-copy inspection; allocates only when needed.
inline std::string ensureISO8601(std::string_view ts) {
    // Regex: starts with two digits followed by ':' → time-only format
    static const std::regex timeOnlyRe(R"(^\d{2}:\d{2}:\d{2})");
    std::string s(ts);

    if (std::regex_search(s, timeOnlyRe)) {
        // Capture today's date from the system clock
        const auto now   = std::chrono::system_clock::now();
        const auto ttime = std::chrono::system_clock::to_time_t(now);
        std::tm tmBuf{};
#if defined(_WIN32)
        localtime_s(&tmBuf, &ttime);
#else
        localtime_r(&ttime, &tmBuf);
#endif
        std::ostringstream oss;
        oss << std::put_time(&tmBuf, "%Y-%m-%dT");
        // Keep only HH:MM:SS from original (strip sub-seconds if present)
        oss << s.substr(0, 8) << "Z";
        return oss.str();
    }

    // Already has a date component — normalise separators lightly and return
    return s;
}

#endif
