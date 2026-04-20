#include "Event.h"

Event::Event(const std::string& id, const std::string& ts,
             const std::string& user, const std::string& desc)
    : eventID(id), timestamp(ts), userName(user), description(desc) {}

bool Event::operator==(const Event& other) const {
    return eventID == other.eventID;
}

std::ostream& operator<<(std::ostream& os, const Event& e) {
    os << "[" << e.getType() << "] " << e.eventID
       << " | " << e.timestamp
       << " | " << e.userName
       << " | " << e.description;
    return os;
}
