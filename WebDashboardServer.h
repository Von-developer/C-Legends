#pragma once
#ifndef WEBDASHBOARDSERVER_H
#define WEBDASHBOARDSERVER_H

/**
 * WebDashboardServer  (C++20, POSIX only)
 *
 * An HTTP/1.1 server that:
 *   1. Serves the static dashboard from dashboard/
 *   2. Exposes a REST JSON API consumed by the frontend JS
 *   3. Provides Admin-gated endpoints for file management
 *   4. Bridges to WebSocketServer for live-event push
 *
 * REST endpoints
 * ──────────────
 *  GET  /api/events?page=N&limit=50&type=Error&q=text
 *       Returns paginated, filtered event list as JSON.
 *
 *  GET  /api/stats
 *       Returns KPI totals, country breakdown, timeline buckets.
 *
 *  GET  /api/search?q=text&page=N
 *       Full-text search over all loaded events, paginated.
 *
 *  GET  /api/files
 *       [Admin] List log files available in the working directory.
 *
 *  POST /api/files/load   body: {"path":"Mac.log"}
 *       [Admin] Load a new log file into LogManager.
 *
 *  POST /api/files/remove body: {"path":"old.log"}
 *       [Admin] Unload events that came from a specific source file.
 *
 *  GET  /api/files/read?path=foo.log&offset=0&limit=200
 *       [Admin] Read raw lines from a log file (for in-dashboard editor).
 *
 *  POST /api/files/append body: {"path":"foo.log","line":"raw log line"}
 *       [Admin] Append a line to a log file (triggers LiveFileWatcher).
 *
 *  POST /api/auth         body: {"password":"secret"}
 *       Returns {"role":"admin"|"user","token":"..."} (simple token RBAC).
 *
 * RBAC
 * ────
 *  Two roles: "admin" (full access) and "user" (read-only GET endpoints).
 *  Bearer token in Authorization header: "Authorization: Bearer <token>".
 *  Admin password is set at construction time (default "clegends-admin").
 *  Tokens are 128-bit random hex strings stored in a set — no expiry for
 *  simplicity (production would use JWT).
 *
 * Static file serving
 * ───────────────────
 *  Any request whose path does not start with /api or /ws is served from
 *  the `dashboard/` directory relative to the binary's working directory.
 *  Content-Type is derived from extension.
 */

#include "LogManager.h"
#include "FileHandler.h"
#include "WebSocketServer.h"

#include <string>
#include <string_view>
#include <thread>
#include <mutex>
#include <atomic>
#include <unordered_set>
#include <functional>
#include <cstdint>

class WebDashboardServer {
public:
    struct Config {
        uint16_t    httpPort      = 8080;
        uint16_t    wsPort        = 9090;
        std::string dashboardDir  = "dashboard";
        std::string adminPassword = "clegends-admin";
        std::string logDir        = ".";    // directory scanned for log files
    };

    explicit WebDashboardServer(LogManager&   manager,
                                 FileHandler&  fileHandler,
                                 const Config& cfg);
    ~WebDashboardServer();

    // Non-copyable
    WebDashboardServer(const WebDashboardServer&)            = delete;
    WebDashboardServer& operator=(const WebDashboardServer&) = delete;

    void start();
    void stop();
    bool isRunning() const { return running_.load(); }

    // Called by LogManager consumer after each event is committed.
    // Serialises the event to JSON and broadcasts via WebSocketServer.
    void pushEvent(const Event* e);

private:
    LogManager&       manager_;
    FileHandler&      fileHandler_;
    Config            cfg_;
    WebSocketServer   ws_;

    std::atomic<bool> running_{false};
    std::jthread      acceptThread_;
    int               listenFd_{-1};

    // ── Token store (RBAC) ────────────────────────────────────────────────
    mutable std::mutex                    tokenMutex_;
    std::unordered_set<std::string>       adminTokens_;
    std::unordered_set<std::string>       userTokens_;

    // ── Accept / dispatch ─────────────────────────────────────────────────
    void acceptLoop(std::stop_token st);
    void handleClient(int fd);

    // ── Request parsing ───────────────────────────────────────────────────
    struct Request {
        std::string method;     // GET / POST
        std::string path;       // e.g. /api/events
        std::string query;      // everything after ?
        std::string body;
        std::string authToken;  // from Authorization: Bearer
        std::unordered_map<std::string, std::string> headers;
    };
    static Request parseRequest(const std::string& raw);
    static std::unordered_map<std::string,std::string> parseQuery(const std::string& q);

    // ── Response helpers ──────────────────────────────────────────────────
    static std::string jsonResponse(int status, const std::string& body);
    static std::string errorJson(const std::string& msg);
    static void sendResponse(int fd, const std::string& resp);

    // ── Route handlers ────────────────────────────────────────────────────
    std::string routeApiEvents(const Request& req);
    std::string routeApiStats(const Request& req);
    std::string routeApiSearch(const Request& req);
    std::string routeApiFiles(const Request& req);
    std::string routeApiFilesLoad(const Request& req);
    std::string routeApiFilesRemove(const Request& req);
    std::string routeApiFilesRead(const Request& req);
    std::string routeApiFilesAppend(const Request& req);
    std::string routeApiAuth(const Request& req);
    std::string routeApiRisk(const Request& req);
    std::string routeStatic(const Request& req);

    // ── RBAC helpers ──────────────────────────────────────────────────────
    bool isAdmin(const Request& req) const;
    bool isAuthenticated(const Request& req) const;
    std::string generateToken() const;

    // ── JSON serialisation helpers ────────────────────────────────────────
    static std::string eventToJson(const Event* e);
    static std::string escapeJson(const std::string& s);

    // ── Static file helpers ───────────────────────────────────────────────
    static std::string contentType(const std::string& path);
    static std::string readFile(const std::string& path);
};

#endif // WEBDASHBOARDSERVER_H
