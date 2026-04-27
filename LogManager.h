#ifndef LOGMANAGER_H
#define LOGMANAGER_H

#include "Event.h"
#include "Utility.h"
#include <vector>
#include <string>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>

class LogManager {
private:
    // ── Persistent log store ──────────────────────────────────────────────
    std::vector<Event*> logs;
    mutable std::mutex  logsMutex;   // guards 'logs' for thread-safe access

    // ── Producer-Consumer queue ───────────────────────────────────────────
    std::queue<Event*>      eventQueue;
    std::mutex              queueMutex;
    std::condition_variable queueCV;
    std::atomic<bool>       running{false};
    std::jthread            consumerThread;   // C++20: auto-joins on destruction

    // ── Consumer loop (runs on consumerThread) ────────────────────────────
    void consumerLoop(std::stop_token st);

public:
    LogManager() = default;
    ~LogManager();

    // ── Thread-safe event ingestion (used by LiveFileWatcher / Producer) ──
    // Takes ownership of the heap-allocated Event*.
    void pushEvent(Event* e);

    // ── Start / stop the background consumer thread ───────────────────────
    void startProcessing();
    void stopProcessing();

    // ── Direct (non-queued) add — used by FileHandler batch load ──────────
    void addEvent(Event* e);
    bool removeEvent(const std::string& id);  // throws out_of_range if missing

    // ── Queries ───────────────────────────────────────────────────────────
    void displayAll()                                 const;
    void searchByType(const std::string& type)        const;
    void searchByUser(const std::string& user)        const;
    void filterWarnBySeverity(const std::string& sev) const;

    int  getCount()                      const { return (int)logs.size(); }
    const std::vector<Event*>& getLogs() const { return logs; }

    // Next auto-generated ID (e.g. "E005")
    std::string generateID() const;

    // ── Geo exports (for Grafana Geomap / Prometheus) ─────────────────────
    // Writes a GeoJSON FeatureCollection of events that have lat/lon data.
    void exportGeoJSON(const std::string& outPath) const;

    // Writes a CSV: timestamp,type,ip,latitude,longitude,city,country,isoCode
    // Grafana's "CSV data source" or InfluxDB line-protocol importer can use this.
    void exportGeoCSV(const std::string& outPath) const;

    // Returns Prometheus-format text for the /metrics scrape endpoint.
    // Each geo-enriched event becomes a labelled gauge sample.
    std::string buildPrometheusMetrics() const;
};

#endif
