#ifndef LOGINEVENT_H
#define LOGINEVENT_H

#include "Event.h"

class LoginEvent : public Event {
private:
    bool        loginSuccess;
    std::string accessLevel;

public:
    LoginEvent(const std::string& id, const std::string& ts,
               const std::string& user, const std::string& desc,
               bool success, const std::string& level);

    void        displayDetails() const override;
    std::string getType()        const override { return "Login"; }
    bool        isSuccessful()   const { return loginSuccess; }
    std::string getAccessLevel() const { return accessLevel; }

    // For FileHandler serialization
    std::string getExtra1() const { return loginSuccess ? "true" : "false"; }
    std::string getExtra2() const { return accessLevel; }
};

#endif
