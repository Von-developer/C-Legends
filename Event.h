#ifndef EVENT_H
#define EVENT_H

#include <string>
#include <iostream>

class Event {
protected:
    std::string eventID;
    std::string timestamp;
    std::string userName;
    std::string description;

public:
    Event(const std::string& id, const std::string& ts,
          const std::string& user, const std::string& desc);
    virtual ~Event() {}

    // Pure virtual — each derived class implements its own
    virtual void displayDetails() const = 0;
    virtual std::string getType() const = 0;

    // Getters
    std::string getEventID()    const { return eventID; }
    std::string getTimestamp()  const { return timestamp; }
    std::string getUserName()   const { return userName; }
    std::string getDescription()const { return description; }

    // Operator overloading
    bool operator==(const Event& other) const;
    friend std::ostream& operator<<(std::ostream& os, const Event& e);
};

#endif
