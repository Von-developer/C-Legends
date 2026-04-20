#ifndef LOGMANAGER_H
#define LOGMANAGER_H

#include "Event.h"
#include "Utility.h"
#include <vector>
#include <string>

class LogManager {
private:
    std::vector<Event*> logs;

public:
    LogManager() {}
    ~LogManager();

    void addEvent(Event* e);
    bool removeEvent(const std::string& id);   // throws out_of_range if not found

    void displayAll()                              const;
    void searchByType(const std::string& type)     const;
    void searchByUser(const std::string& user)     const;
    void filterWarnBySeverity(const std::string& sev) const;

    int  getCount()                                const { return (int)logs.size(); }
    const std::vector<Event*>& getLogs()           const { return logs; }

    // Next auto-generated ID (e.g. "E005")
    std::string generateID() const;
};

#endif
