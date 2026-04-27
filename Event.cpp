#include "Event.h"

Event::Event(const std::string& id, const std::string& ts,
             const std::string& user, const std::string& desc,
             const std::string& ip,
             const std::string& fullTs)
    : eventID(id), timestamp(ts), userName(user), description(desc),
      ipAddress(ip), fullTimestamp(fullTs) {}

bool Event::operator==(const Event& other) const {
    return eventID == other.eventID;
}

// ── Static helper: extract first IPv4 from a log line ─────────────────────
// Uses std::string_view for zero-copy inspection; allocates one std::string
// only when needed by std::regex (which requires random-access iterators).
//
// Pattern uses \b word-boundaries which are supported by all std::regex
// ECMAScript implementations — avoids lookbehind which is non-standard.
std::string Event::extractIPv4(std::string_view line) {
    static const std::regex ipRe(
        R"(\b(\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3})\b)");
    std::string s(line);
    std::smatch m;
    if (std::regex_search(s, m, ipRe))
        return m[1].str();
    return {};
}

std::ostream& operator<<(std::ostream& os, const Event& e) {
    os << "[" << e.getType() << "] " << e.eventID
       << " | " << (e.fullTimestamp.empty() ? e.timestamp : e.fullTimestamp)
       << " | " << e.userName
       << " | " << e.description;
    if (!e.ipAddress.empty())
        os << " | IP:" << e.ipAddress;
    return os;
}
