#include "LogManager.h"
#include "WarningEvent.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <stdexcept>
#include <chrono>
#include <map>

// ── Destructor ────────────────────────────────────────────────────────────
LogManager::~LogManager() {
    stopProcessing();                       // drain queue, stop consumer thread
    std::lock_guard<std::mutex> lk(logsMutex);
    for (Event* e : logs) delete e;
    logs.clear();
}

// ── Consumer loop ─────────────────────────────────────────────────────────
// Runs on consumerThread.  Wakes whenever the queue has items or a stop is
// requested.  Uses std::stop_token (C++20) so jthread auto-requests stop on
// destruction.
void LogManager::consumerLoop(std::stop_token st) {
    while (!st.stop_requested()) {
        std::unique_lock<std::mutex> lk(queueMutex);
        // Wait up to 200 ms for work or a stop signal
        queueCV.wait_for(lk, std::chrono::milliseconds(200),
                         [&]{ return !eventQueue.empty() || st.stop_requested(); });

        // Drain everything currently in the queue
        while (!eventQueue.empty()) {
            Event* e = eventQueue.front();
            eventQueue.pop();
            lk.unlock();

            // ── Enrich: normalize timestamp to ISO 8601 ──────────────────
            if (e->getFullTimestamp().empty())
                e->setFullTimestamp(ensureISO8601(e->getTimestamp()));

            // ── Structured live output to console ───────────────────────
            std::cout << "[LIVE] " << *e << "\n";

            // ── Move into the persistent log store (thread-safe) ─────────
            {
                std::lock_guard<std::mutex> storeLk(logsMutex);
                logs.push_back(e);
            }

            lk.lock();   // re-acquire before checking queue again
        }
    }
}

// ── Start / stop processing ───────────────────────────────────────────────
void LogManager::startProcessing() {
    if (running.exchange(true)) return;   // already running
    consumerThread = std::jthread([this](std::stop_token st){
        consumerLoop(st);
    });
    std::cout << "[LogManager] Consumer thread started.\n";
}

void LogManager::stopProcessing() {
    if (!running.exchange(false)) return;
    consumerThread.request_stop();
    queueCV.notify_all();
    // jthread joins automatically when reassigned / destroyed
    consumerThread = std::jthread{};
    std::cout << "[LogManager] Consumer thread stopped.\n";
}

// ── Thread-safe producer push ─────────────────────────────────────────────
void LogManager::pushEvent(Event* e) {
    {
        std::lock_guard<std::mutex> lk(queueMutex);
        eventQueue.push(e);
    }
    queueCV.notify_one();
}

// ── Direct (non-queued) add — used by FileHandler batch load ──────────────
void LogManager::addEvent(Event* e) {
    std::lock_guard<std::mutex> lk(logsMutex);
    logs.push_back(e);
}

bool LogManager::removeEvent(const std::string& id) {
    std::lock_guard<std::mutex> lk(logsMutex);
    for (auto it = logs.begin(); it != logs.end(); ++it) {
        if ((*it)->getEventID() == id) {
            delete *it;
            logs.erase(it);
            return true;
        }
    }
    throw std::out_of_range("Event ID not found: " + id);
}

void LogManager::displayAll() const {
    std::lock_guard<std::mutex> lk(logsMutex);
    if (logs.empty()) {
        std::cout << "No log events on record.\n";
        return;
    }
    std::cout << "\n=== All Log Events (" << logs.size() << " total) ===\n";
    for (Event* e : logs) {
        e->displayDetails();   // polymorphic call
    }
    std::cout << "---------------------------------------\n";
}

void LogManager::searchByType(const std::string& type) const {
    std::lock_guard<std::mutex> lk(logsMutex);
    auto results = searchBy(logs, &Event::getType, type);
    if (results.empty()) {
        std::cout << "No events of type: " << type << "\n";
        return;
    }
    std::cout << "\n=== Results for type: " << type
              << " (" << results.size() << " found) ===\n";
    for (Event* e : results) e->displayDetails();
    std::cout << "---------------------------------------\n";
}

void LogManager::searchByUser(const std::string& user) const {
    std::lock_guard<std::mutex> lk(logsMutex);
    auto results = searchBy(logs, &Event::getUserName, user);
    if (results.empty()) {
        std::cout << "No events for user: " << user << "\n";
        return;
    }
    std::cout << "\n=== Results for user: " << user
              << " (" << results.size() << " found) ===\n";
    for (Event* e : results) e->displayDetails();
    std::cout << "---------------------------------------\n";
}

void LogManager::filterWarnBySeverity(const std::string& sev) const {
    std::lock_guard<std::mutex> lk(logsMutex);
    std::cout << "\n=== Warnings with severity: " << sev << " ===\n";
    bool found = false;
    for (Event* e : logs) {
        if (e->getType() == "Warning") {
            WarningEvent* w = dynamic_cast<WarningEvent*>(e);
            if (w && w->getSeverity() == sev) {
                w->displayDetails();
                found = true;
            }
        }
    }
    if (!found) std::cout << "None found.\n";
    std::cout << "---------------------------------------\n";
}

std::string LogManager::generateID() const {
    std::lock_guard<std::mutex> lk(logsMutex);
    std::ostringstream ss;
    ss << "E" << std::setw(3) << std::setfill('0') << (logs.size() + 1);
    return ss.str();
}

// ── exportGeoJSON ─────────────────────────────────────────────────────────
// Produces a GeoJSON FeatureCollection consumable by Grafana's Geomap Panel.
// Each feature's geometry is a Point([lon, lat]) and its properties carry
// the event metadata Grafana uses for tooltip labels and colour coding.
void LogManager::exportGeoJSON(const std::string& outPath) const {
    std::lock_guard<std::mutex> lk(logsMutex);

    std::ofstream f(outPath);
    if (!f.is_open()) {
        std::cerr << "[LogManager] Cannot write GeoJSON to: " << outPath << "\n";
        return;
    }

    f << "{\n  \"type\": \"FeatureCollection\",\n  \"features\": [\n";
    bool first = true;
    for (const Event* e : logs) {
        if (!e->hasGeo()) continue;
        if (!first) f << ",\n";
        first = false;

        // Escape any quotes in strings
        auto esc = [](const std::string& s) {
            std::string out;
            out.reserve(s.size());
            for (char c : s) { if (c == '"') out += "\\\""; else out += c; }
            return out;
        };

        f << "    {\n"
          << "      \"type\": \"Feature\",\n"
          << "      \"geometry\": {\n"
          << "        \"type\": \"Point\",\n"
          << "        \"coordinates\": ["
          << std::fixed << std::setprecision(6)
          << e->getLongitude() << ", " << e->getLatitude() << "]\n"
          << "      },\n"
          << "      \"properties\": {\n"
          << "        \"id\": \""        << esc(e->getEventID())       << "\",\n"
          << "        \"type\": \""      << esc(e->getType())          << "\",\n"
          << "        \"timestamp\": \"" << esc(e->getFullTimestamp()) << "\",\n"
          << "        \"ip\": \""        << esc(e->getIPAddress())     << "\",\n"
          << "        \"city\": \""      << esc(e->getGeoCity())       << "\",\n"
          << "        \"country\": \""   << esc(e->getGeoCountry())    << "\",\n"
          << "        \"cc\": \""        << esc(e->getGeoIsoCode())    << "\",\n"
          << "        \"desc\": \""      << esc(e->getDescription())   << "\"\n"
          << "      }\n"
          << "    }";
    }
    f << "\n  ]\n}\n";
    std::cout << "[LogManager] GeoJSON written to: " << outPath << "\n";
}

// ── exportGeoCSV ──────────────────────────────────────────────────────────
// CSV with a header row — importable into InfluxDB, Grafana CSV datasource,
// or any spreadsheet tool for ad-hoc analysis.
void LogManager::exportGeoCSV(const std::string& outPath) const {
    std::lock_guard<std::mutex> lk(logsMutex);

    std::ofstream f(outPath);
    if (!f.is_open()) {
        std::cerr << "[LogManager] Cannot write geo CSV to: " << outPath << "\n";
        return;
    }

    f << "timestamp,eventID,type,ip,latitude,longitude,city,country,isoCode,description\n";
    int count = 0;
    for (const Event* e : logs) {
        if (!e->hasGeo()) continue;
        f << e->getFullTimestamp()   << ","
          << e->getEventID()         << ","
          << e->getType()            << ","
          << e->getIPAddress()       << ","
          << std::fixed << std::setprecision(6)
          << e->getLatitude()        << ","
          << e->getLongitude()       << ","
          << "\"" << e->getGeoCity()    << "\","
          << "\"" << e->getGeoCountry() << "\","
          << e->getGeoIsoCode()      << ","
          << "\"" << e->getDescription() << "\"\n";
        count++;
    }
    std::cout << "[LogManager] Geo CSV: " << count
              << " geo-enriched events → " << outPath << "\n";
}

// ── buildPrometheusMetrics ────────────────────────────────────────────────
// Returns the Prometheus exposition format consumed by PrometheusExporter
// (and directly by Grafana if you point a Prometheus datasource at /metrics).
//
// Metrics exposed:
//   clegends_event_total{type,cc,city}  counter — events seen per geo bucket
//   clegends_geo_hit{ip,lat,lon,city,cc} gauge=1 — latest event per IP
std::string LogManager::buildPrometheusMetrics() const {
    std::lock_guard<std::mutex> lk(logsMutex);

    // Count events per (type, country) bucket
    std::map<std::string, uint64_t> counters;
    // Latest geo hit per IP
    struct GeoHit { double lat, lon; std::string city, cc, type; };
    std::map<std::string, GeoHit> geoHits;

    for (const Event* e : logs) {
        std::string key = e->getType() + "|" + e->getGeoIsoCode() + "|" + e->getGeoCity();
        counters[key]++;
        if (e->hasGeo() && !e->getIPAddress().empty()) {
            geoHits[e->getIPAddress()] = {
                e->getLatitude(), e->getLongitude(),
                e->getGeoCity(), e->getGeoIsoCode(), e->getType()
            };
        }
    }

    std::ostringstream out;
    out << std::fixed << std::setprecision(6);

    // clegends_event_total
    out << "# HELP clegends_event_total Total log events ingested per type/geo bucket\n"
        << "# TYPE clegends_event_total counter\n";
    for (auto& [key, cnt] : counters) {
        // split key on '|'
        size_t p1 = key.find('|');
        size_t p2 = key.find('|', p1 + 1);
        std::string type    = key.substr(0, p1);
        std::string cc      = key.substr(p1 + 1, p2 - p1 - 1);
        std::string city    = key.substr(p2 + 1);
        out << "clegends_event_total{type=\"" << type
            << "\",cc=\"" << cc
            << "\",city=\"" << city << "\"} "
            << cnt << "\n";
    }

    // clegends_geo_hit
    out << "# HELP clegends_geo_hit Latest geo-enriched event per source IP\n"
        << "# TYPE clegends_geo_hit gauge\n";
    for (auto& [ip, h] : geoHits) {
        out << "clegends_geo_hit{ip=\"" << ip
            << "\",lat=\"" << h.lat
            << "\",lon=\"" << h.lon
            << "\",city=\"" << h.city
            << "\",cc=\"" << h.cc
            << "\",type=\"" << h.type << "\"} 1\n";
    }

    // Total event count
    out << "# HELP clegends_total_events Total events in LogManager store\n"
        << "# TYPE clegends_total_events gauge\n"
        << "clegends_total_events " << logs.size() << "\n";

    return out.str();
}
