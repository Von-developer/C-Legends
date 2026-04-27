#ifndef PROMETHEUSEXPORTER_H
#define PROMETHEUSEXPORTER_H

/**
 * PrometheusExporter  —  Zero-dependency Prometheus pull endpoint
 *
 * Starts a tiny HTTP/1.1 server on a configurable port (default 9091).
 * When Grafana's Prometheus datasource (or `curl`) hits GET /metrics, it
 * receives the Prometheus exposition-format text produced by
 * LogManager::buildPrometheusMetrics().
 *
 * This replaces the need for the external prometheus-cpp library — it uses
 * only POSIX sockets (available on every Linux/macOS target) and C++20
 * standard library.
 *
 * Architecture
 * ────────────
 *   • One std::jthread accepts connections in a loop (acceptLoop).
 *   • Each accepted connection is handled synchronously (keep-it-simple;
 *     log scrapes are infrequent).
 *   • The MetricsCallback is a std::function<std::string()> — the caller
 *     wires it to LogManager::buildPrometheusMetrics().
 *
 * Grafana wiring
 * ──────────────
 *   1. Add a Prometheus datasource pointing at http://<host>:9091
 *   2. In any panel, use the metric  clegends_event_total  or
 *      clegends_geo_hit  — see LogManager::buildPrometheusMetrics().
 *
 * "Light Transmission" optimisation
 * ───────────────────────────────────
 *   Metrics are built in RAM (LogManager::buildPrometheusMetrics) and sent
 *   directly over a socket — no disk I/O, no CSV write.  Grafana pulls on
 *   its own scrape interval (default 15 s) so the C++ side is never blocked.
 */

#include <functional>
#include <string>
#include <thread>
#include <atomic>

using MetricsCallback = std::function<std::string()>;

class PrometheusExporter {
public:
    explicit PrometheusExporter(MetricsCallback cb,
                                uint16_t        port = 9091);
    ~PrometheusExporter();

    // Non-copyable
    PrometheusExporter(const PrometheusExporter&)            = delete;
    PrometheusExporter& operator=(const PrometheusExporter&) = delete;

    void start();
    void stop();
    bool isRunning() const { return running_.load(); }
    uint16_t port()  const { return port_; }

private:
    MetricsCallback   cb_;
    uint16_t          port_;
    std::atomic<bool> running_{false};
    std::jthread      serverThread_;
    int               listenFd_{-1};

    void acceptLoop(std::stop_token st);
    void handleClient(int clientFd);
};

#endif
