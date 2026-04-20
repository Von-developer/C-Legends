#include "ErrorEvent.h"
#include <iostream>

ErrorEvent::ErrorEvent(const std::string& id, const std::string& ts,
                       const std::string& user, const std::string& desc,
                       int code, const std::string& module)
    : Event(id, ts, user, desc), errorCode(code), sourceModule(module) {}

void ErrorEvent::displayDetails() const {
    std::cout << "---------------------------------------\n";
    std::cout << "Type        : Error Event\n";
    std::cout << "ID          : " << eventID      << "\n";
    std::cout << "Timestamp   : " << timestamp    << "\n";
    std::cout << "User        : " << userName     << "\n";
    std::cout << "Description : " << description  << "\n";
    std::cout << "Error Code  : " << errorCode    << "\n";
    std::cout << "Module      : " << sourceModule << "\n";
}
