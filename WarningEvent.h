#ifndef WARNINGEVENT_H
#define WARNINGEVENT_H

#include "Event.h"

class WarningEvent : public Event {
private:
    std::string severity;
    std::string recommendedAction;

public:
    WarningEvent(const std::string& id, const std::string& ts,
                 const std::string& user, const std::string& desc,
                 const std::string& sev, const std::string& action);

    void        displayDetails()      const override;
    std::string getType()             const override { return "Warning"; }
    std::string getSeverity()         const { return severity; }
    std::string getRecommendedAction()const { return recommendedAction; }

    std::string getExtra1() const { return severity; }
    std::string getExtra2() const { return recommendedAction; }
};

#endif
