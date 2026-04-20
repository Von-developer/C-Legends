#ifndef ERROREVENT_H
#define ERROREVENT_H

#include "Event.h"

class ErrorEvent : public Event {
private:
    int         errorCode;
    std::string sourceModule;

public:
    ErrorEvent(const std::string& id, const std::string& ts,
               const std::string& user, const std::string& desc,
               int code, const std::string& module);

    void        displayDetails() const override;
    std::string getType()        const override { return "Error"; }
    int         getErrorCode()   const { return errorCode; }
    std::string getSourceModule()const { return sourceModule; }

    std::string getExtra1() const { return std::to_string(errorCode); }
    std::string getExtra2() const { return sourceModule; }
};

#endif
