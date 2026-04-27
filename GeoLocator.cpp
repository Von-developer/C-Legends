#include "GeoLocator.h"
#include <iostream>
#include <cstring>
#include <sstream>

// ── GeoResult helpers ─────────────────────────────────────────────────────
std::string GeoResult::toLabel() const {
    std::ostringstream oss;
    oss << "city=\"" << city << "\","
        << "country=\"" << country << "\","
        << "cc=\"" << isoCode << "\"";
    return oss.str();
}

// ── Constructor ───────────────────────────────────────────────────────────
GeoLocator::GeoLocator(std::string_view dbPath) {
    std::string path(dbPath);
    int status = MMDB_open(path.c_str(), MMDB_MODE_MMAP, &mmdb_);
    if (status == MMDB_SUCCESS) {
        dbOpen_ = true;
        std::cout << "[GeoLocator] Opened: " << path
                  << "  (nodes=" << mmdb_.metadata.node_count << ")\n";
    } else {
        std::cerr << "[GeoLocator] DB not found at '" << path
                  << "' — geo lookups will return defaults.\n"
                  << "  Download GeoLite2-City.mmdb from "
                  << "https://dev.maxmind.com/geoip/geolite2-free-geolocation-data\n"
                  << "  and place it next to the binary.\n";
    }
}

// ── Destructor ────────────────────────────────────────────────────────────
GeoLocator::~GeoLocator() {
    if (dbOpen_) {
        MMDB_close(&mmdb_);
        dbOpen_ = false;
    }
}

// ── lookup() ──────────────────────────────────────────────────────────────
// Thread-safe: MMDB lookups are purely read operations on the mmap'd file.
GeoResult GeoLocator::lookup(std::string_view ipAddress) const {
    GeoResult result;

    // ── Always skip private / loopback (regardless of DB state) ──────────
    std::string ip(ipAddress);
    if (ip.empty()
        || ip.rfind("127.",     0) == 0
        || ip.rfind("10.",      0) == 0
        || ip.rfind("192.168.", 0) == 0
        || ip == "0.0.0.0"
        || ip == "255.255.255.255") {
        result.city    = "Private";
        result.country = "LAN";
        result.isoCode = "--";
        return result;
    }

    // No DB available — return Unknown defaults
    if (!dbOpen_) return result;

    int gai_err  = 0;
    int mmdb_err = MMDB_SUCCESS;
    MMDB_lookup_result_s res =
        MMDB_lookup_string(&mmdb_, ip.c_str(), &gai_err, &mmdb_err);

    if (!res.found_entry || mmdb_err != MMDB_SUCCESS) return result;

    result.found     = true;
    result.latitude  = getDouble(res.entry, "location", "latitude");
    result.longitude = getDouble(res.entry, "location", "longitude");
    result.city      = getString(res.entry, "city", "en");
    result.country   = getString(res.entry, "country", "en");
    result.isoCode   = getString(res.entry, "country", "iso_code");

    // Fallback for empty strings
    if (result.city.empty())    result.city    = "Unknown";
    if (result.country.empty()) result.country = "Unknown";
    if (result.isoCode.empty()) result.isoCode = "??";

    return result;
}

// ── Private helpers ───────────────────────────────────────────────────────
std::string GeoLocator::getString(MMDB_entry_s& entry,
                                   const char* key1,
                                   const char* key2) {
    MMDB_entry_data_s data;
    int rc;
    if (key2)
        rc = MMDB_get_value(&entry, &data, key1, "names", key2, nullptr);
    else
        rc = MMDB_get_value(&entry, &data, key1, nullptr);

    if (rc != MMDB_SUCCESS || !data.has_data ||
        data.type != MMDB_DATA_TYPE_UTF8_STRING)
        return {};

    return std::string(data.utf8_string, data.data_size);
}

double GeoLocator::getDouble(MMDB_entry_s& entry,
                              const char* key1,
                              const char* key2) {
    MMDB_entry_data_s data;
    int rc;
    if (key2)
        rc = MMDB_get_value(&entry, &data, key1, key2, nullptr);
    else
        rc = MMDB_get_value(&entry, &data, key1, nullptr);

    if (rc != MMDB_SUCCESS || !data.has_data) return 0.0;
    if (data.type == MMDB_DATA_TYPE_DOUBLE) return data.double_value;
    if (data.type == MMDB_DATA_TYPE_FLOAT)  return (double)data.float_value;
    return 0.0;
}
