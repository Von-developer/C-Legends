#include "WarningEvent.h"
#include <iostream>

WarningEvent::WarningEvent(const std::string& id, const std::string& ts,
                           const std::string& user, const std::string& desc,
                           const std::string& sev, const std::string& action)
    : Event(id, ts, user, desc), severity(sev), recommendedAction(action) {}

void WarningEvent::displayDetails() const {
    std::cout << "---------------------------------------\n";
    std::cout << "Type        : Warning Event\n";
    std::cout << "ID          : " << eventID           << "\n";
    std::cout << "Timestamp   : " << timestamp         << "\n";
    std::cout << "User        : " << userName          << "\n";
    std::cout << "Description : " << description       << "\n";
    std::cout << "Severity    : " << severity          << "\n";
    std::cout << "Action      : " << recommendedAction << "\n";
}
