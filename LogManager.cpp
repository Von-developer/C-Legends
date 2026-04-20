#include "LogManager.h"
#include "WarningEvent.h"
#include <iostream>
#include <sstream>
#include <iomanip>
#include <stdexcept>

LogManager::~LogManager() {
    for (Event* e : logs) delete e;
    logs.clear();
}

void LogManager::addEvent(Event* e) {
    logs.push_back(e);
}

bool LogManager::removeEvent(const std::string& id) {
    for (auto it = logs.begin(); it != logs.end(); ++it) {
        if ((*it)->getEventID() == id) {
            delete *it;
            logs.erase(it);
            return true;
        }
    }
    throw std::out_of_range("Event ID not found: " + id);
}

void LogManager::displayAll() const {
    if (logs.empty()) {
        std::cout << "No log events on record.\n";
        return;
    }
    std::cout << "\n=== All Log Events (" << logs.size() << " total) ===\n";
    for (Event* e : logs) {
        e->displayDetails();   // polymorphic call
    }
    std::cout << "---------------------------------------\n";
}

void LogManager::searchByType(const std::string& type) const {
    // Uses the searchBy<T> template from Utility.h
    auto results = searchBy(logs, &Event::getType, type);
    if (results.empty()) {
        std::cout << "No events of type: " << type << "\n";
        return;
    }
    std::cout << "\n=== Results for type: " << type
              << " (" << results.size() << " found) ===\n";
    for (Event* e : results) e->displayDetails();
    std::cout << "---------------------------------------\n";
}

void LogManager::searchByUser(const std::string& user) const {
    auto results = searchBy(logs, &Event::getUserName, user);
    if (results.empty()) {
        std::cout << "No events for user: " << user << "\n";
        return;
    }
    std::cout << "\n=== Results for user: " << user
              << " (" << results.size() << " found) ===\n";
    for (Event* e : results) e->displayDetails();
    std::cout << "---------------------------------------\n";
}

void LogManager::filterWarnBySeverity(const std::string& sev) const {
    std::cout << "\n=== Warnings with severity: " << sev << " ===\n";
    bool found = false;
    for (Event* e : logs) {
        if (e->getType() == "Warning") {
            WarningEvent* w = dynamic_cast<WarningEvent*>(e);
            if (w && w->getSeverity() == sev) {
                w->displayDetails();
                found = true;
            }
        }
    }
    if (!found) std::cout << "None found.\n";
    std::cout << "---------------------------------------\n";
}

std::string LogManager::generateID() const {
    std::ostringstream ss;
    ss << "E" << std::setw(3) << std::setfill('0') << (logs.size() + 1);
    return ss.str();
}
