#ifndef EVENT_H
#define EVENT_H

#include <string>
#include <iostream>
#include <regex>

class Event {
protected:
    std::string eventID;
    std::string timestamp;      // original timestamp from log source
    std::string userName;
    std::string description;
    std::string ipAddress;      // IPv4 extracted from log line (empty if none)
    std::string fullTimestamp;  // ISO 8601 normalized: YYYY-MM-DDTHH:MM:SSZ
    // Geo fields populated by GeoLocator after lookup
    double      latitude  = 0.0;
    double      longitude = 0.0;
    std::string geoCity;
    std::string geoCountry;
    std::string geoIsoCode;

public:
    Event(const std::string& id, const std::string& ts,
          const std::string& user, const std::string& desc,
          const std::string& ip = "",
          const std::string& fullTs = "");
    virtual ~Event() {}

    // Pure virtual — each derived class implements its own
    virtual void displayDetails() const = 0;
    virtual std::string getType() const = 0;

    // Getters
    std::string getEventID()       const { return eventID; }
    std::string getTimestamp()     const { return timestamp; }
    std::string getUserName()      const { return userName; }
    std::string getDescription()   const { return description; }
    std::string getIPAddress()     const { return ipAddress; }
    std::string getFullTimestamp() const { return fullTimestamp; }

    // Setters (used by parsers / watcher after construction)
    void setIPAddress(const std::string& ip)       { ipAddress = ip; }
    void setFullTimestamp(const std::string& fts)  { fullTimestamp = fts; }
    void setGeo(double lat, double lon,
                const std::string& city,
                const std::string& country,
                const std::string& iso) {
        latitude   = lat;
        longitude  = lon;
        geoCity    = city;
        geoCountry = country;
        geoIsoCode = iso;
    }

    // Geo getters
    double      getLatitude()   const { return latitude; }
    double      getLongitude()  const { return longitude; }
    std::string getGeoCity()    const { return geoCity; }
    std::string getGeoCountry() const { return geoCountry; }
    std::string getGeoIsoCode() const { return geoIsoCode; }
    bool        hasGeo()        const { return latitude != 0.0 || longitude != 0.0; }

    // ── Static helpers ────────────────────────────────────────────────────
    // Extract first IPv4 address found in a log line; returns "" if none.
    static std::string extractIPv4(std::string_view line);

    // Operator overloading
    bool operator==(const Event& other) const;
    friend std::ostream& operator<<(std::ostream& os, const Event& e);
};

#endif
