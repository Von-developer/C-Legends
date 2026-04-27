#ifndef GEOLOCATOR_H
#define GEOLOCATOR_H

/**
 * GeoLocator  —  Thread-safe IPv4 → latitude/longitude/city/country lookup
 *
 * Uses the vendored MaxMind DB reader to query a local GeoLite2-City.mmdb
 * file.  All lookups are read-only and therefore safe to call concurrently
 * from multiple threads (LiveFileWatcher + consumer thread).
 *
 * Setup:
 *   1. Download GeoLite2-City.mmdb from MaxMind (free account required):
 *      https://dev.maxmind.com/geoip/geolite2-free-geolocation-data
 *   2. Place it next to your binary, OR pass the full path to the
 *      GeoLocator constructor.
 *
 * Graceful fallback:
 *   If the .mmdb file is not found, every lookup returns GeoResult with
 *   all fields set to defaults (0.0 / "Unknown").  The rest of the app
 *   continues to run normally.
 */

#include <string>
#include <string_view>
#include "vendor/maxminddb/maxminddb.h"

// ── Result struct returned by lookup() ───────────────────────────────────
struct GeoResult {
    double      latitude  = 0.0;
    double      longitude = 0.0;
    std::string city      = "Unknown";
    std::string country   = "Unknown";
    std::string isoCode   = "??";
    bool        found     = false;

    // Prometheus-friendly label string  e.g. city="London",cc="GB"
    std::string toLabel() const;
};

// ── GeoLocator ────────────────────────────────────────────────────────────
class GeoLocator {
public:
    // Default path: "GeoLite2-City.mmdb" (relative to working directory)
    explicit GeoLocator(std::string_view dbPath = "GeoLite2-City.mmdb");
    ~GeoLocator();

    // Non-copyable (MMDB_s owns a file descriptor)
    GeoLocator(const GeoLocator&)            = delete;
    GeoLocator& operator=(const GeoLocator&) = delete;

    // Thread-safe: MMDB lookups are read-only
    GeoResult lookup(std::string_view ipAddress) const;

    bool isOpen() const { return dbOpen_; }

private:
    MMDB_s mmdb_{};
    bool   dbOpen_ = false;

    // Helper: pull a UTF-8 string field from an entry
    static std::string getString(MMDB_entry_s& entry, const char* key1,
                                  const char* key2 = nullptr);
    // Helper: pull a double field
    static double getDouble(MMDB_entry_s& entry, const char* key1,
                             const char* key2 = nullptr);
};

#endif
