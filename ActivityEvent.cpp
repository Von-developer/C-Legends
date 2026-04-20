#include "ActivityEvent.h"
#include <iostream>

ActivityEvent::ActivityEvent(const std::string& id, const std::string& ts,
                             const std::string& user, const std::string& desc,
                             const std::string& action, const std::string& resource)
    : Event(id, ts, user, desc), actionType(action), affectedResource(resource) {}

void ActivityEvent::displayDetails() const {
    std::cout << "---------------------------------------\n";
    std::cout << "Type        : Activity Event\n";
    std::cout << "ID          : " << eventID          << "\n";
    std::cout << "Timestamp   : " << timestamp        << "\n";
    std::cout << "User        : " << userName         << "\n";
    std::cout << "Description : " << description      << "\n";
    std::cout << "Action      : " << actionType       << "\n";
    std::cout << "Resource    : " << affectedResource << "\n";
}
