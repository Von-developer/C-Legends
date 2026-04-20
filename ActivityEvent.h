#ifndef ACTIVITYEVENT_H
#define ACTIVITYEVENT_H

#include "Event.h"

class ActivityEvent : public Event {
private:
    std::string actionType;
    std::string affectedResource;

public:
    ActivityEvent(const std::string& id, const std::string& ts,
                  const std::string& user, const std::string& desc,
                  const std::string& action, const std::string& resource);

    void        displayDetails()     const override;
    std::string getType()            const override { return "Activity"; }
    std::string getActionType()      const { return actionType; }
    std::string getAffectedResource()const { return affectedResource; }

    std::string getExtra1() const { return actionType; }
    std::string getExtra2() const { return affectedResource; }
};

#endif
