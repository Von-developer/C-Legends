#ifndef UTILITY_H
#define UTILITY_H

#include "Event.h"
#include <vector>
#include <string>

// Generic search template — works for any getter that returns a comparable type
// Example usage:
//   searchBy(logs, &Event::getType, std::string("Error"))
template <typename T>
std::vector<Event*> searchBy(const std::vector<Event*>& logs,
                              T (Event::*getter)() const,
                              const T& target) {
    std::vector<Event*> results;
    for (Event* e : logs) {
        if ((e->*getter)() == target) {
            results.push_back(e);
        }
    }
    return results;
}

#endif
