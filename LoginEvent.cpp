#include "LoginEvent.h"
#include <iostream>

LoginEvent::LoginEvent(const std::string& id, const std::string& ts,
                       const std::string& user, const std::string& desc,
                       bool success, const std::string& level)
    : Event(id, ts, user, desc), loginSuccess(success), accessLevel(level) {}

void LoginEvent::displayDetails() const {
    std::cout << "---------------------------------------\n";
    std::cout << "Type        : Login Event\n";
    std::cout << "ID          : " << eventID      << "\n";
    std::cout << "Timestamp   : " << timestamp    << "\n";
    std::cout << "User        : " << userName     << "\n";
    std::cout << "Description : " << description  << "\n";
    std::cout << "Success     : " << (loginSuccess ? "Yes" : "No") << "\n";
    std::cout << "Access Level: " << accessLevel  << "\n";
}
