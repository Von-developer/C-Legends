#include "WebDashboardServer.h"
#include "LoginEvent.h"
#include "ErrorEvent.h"
#include "WarningEvent.h"
#include "ActivityEvent.h"

#include <iostream>
#include <sstream>
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <set>
#include <random>
#include <iomanip>
#include <cstring>
#include <cerrno>

// POSIX
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>

namespace fs = std::filesystem;

// ─────────────────────────────────────────────────────────────────────────
//  Constructor / Destructor
// ─────────────────────────────────────────────────────────────────────────
WebDashboardServer::WebDashboardServer(LogManager&   mgr,
                                        FileHandler&  fh,
                                        const Config& cfg)
    : manager_(mgr), fileHandler_(fh), cfg_(cfg), ws_(cfg.wsPort) {}

WebDashboardServer::~WebDashboardServer() {
    stop();
}

// ─────────────────────────────────────────────────────────────────────────
//  start / stop
// ─────────────────────────────────────────────────────────────────────────
void WebDashboardServer::start() {
    if (running_.exchange(true)) return;

    // Start WebSocket server first
    ws_.start();

    listenFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd_ < 0) {
        std::cerr << "[Dashboard] socket() failed: " << strerror(errno) << "\n";
        running_ = false; return;
    }
    int yes = 1;
    ::setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    ::fcntl(listenFd_, F_SETFL, O_NONBLOCK);

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(cfg_.httpPort);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (::bind(listenFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::cerr << "[Dashboard] bind() port " << cfg_.httpPort
                  << " failed: " << strerror(errno) << "\n";
        ::close(listenFd_); listenFd_ = -1; running_ = false; return;
    }
    ::listen(listenFd_, 32);

    acceptThread_ = std::jthread([this](std::stop_token st){ acceptLoop(st); });

    std::cout << "[Dashboard] HTTP  → http://0.0.0.0:" << cfg_.httpPort << "\n"
              << "[Dashboard] WS    → ws://0.0.0.0:"   << cfg_.wsPort   << "\n"
              << "[Dashboard] Admin password: " << cfg_.adminPassword    << "\n";
}

void WebDashboardServer::stop() {
    if (!running_.exchange(false)) return;
    ws_.stop();
    acceptThread_.request_stop();
    if (listenFd_ >= 0) { ::close(listenFd_); listenFd_ = -1; }
    acceptThread_ = std::jthread{};
    std::cout << "[Dashboard] Stopped.\n";
}

// ─────────────────────────────────────────────────────────────────────────
//  pushEvent — called from LogManager consumer after each committed event
// ─────────────────────────────────────────────────────────────────────────
void WebDashboardServer::pushEvent(const Event* e) {
    if (!e || !ws_.isRunning()) return;
    std::string json = R"({"type":"event","data":)" + eventToJson(e) + "}";
    ws_.broadcast(json);
}

// ─────────────────────────────────────────────────────────────────────────
//  Accept loop
// ─────────────────────────────────────────────────────────────────────────
void WebDashboardServer::acceptLoop(std::stop_token st) {
    while (!st.stop_requested()) {
        pollfd pfd{ listenFd_, POLLIN, 0 };
        if (::poll(&pfd, 1, 200) <= 0) continue;

        sockaddr_in caddr{};
        socklen_t clen = sizeof(caddr);
        int cfd = ::accept(listenFd_, reinterpret_cast<sockaddr*>(&caddr), &clen);
        if (cfd < 0) continue;

        int flags = ::fcntl(cfd, F_GETFL, 0);
        ::fcntl(cfd, F_SETFL, flags & ~O_NONBLOCK);

        // Set recv/send timeouts to avoid stuck connections
        struct timeval tv{ 5, 0 };
        ::setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        ::setsockopt(cfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        std::jthread([this, cfd]() mutable {
            handleClient(cfd);
        }).detach();
    }
}

// ─────────────────────────────────────────────────────────────────────────
//  handleClient — read request, route, send response
// ─────────────────────────────────────────────────────────────────────────
void WebDashboardServer::handleClient(int fd) {
    // Read request (up to 64KB)
    std::string rawReq;
    rawReq.reserve(2048);
    char buf[4096];
    while (true) {
        ssize_t n = ::recv(fd, buf, sizeof(buf) - 1, 0);
        if (n <= 0) { ::close(fd); return; }
        buf[n] = '\0';
        rawReq += buf;
        if (rawReq.find("\r\n\r\n") != std::string::npos) break;
        if (rawReq.size() > 65536) break;
    }

    // Read body if Content-Length present
    Request req = parseRequest(rawReq);
    auto it = req.headers.find("content-length");
    if (it != req.headers.end() && !it->second.empty()) {
        size_t needed = std::stoul(it->second);
        while (req.body.size() < needed && needed < 1048576) {
            ssize_t n = ::recv(fd, buf, std::min(sizeof(buf)-1, needed - req.body.size()), 0);
            if (n <= 0) break;
            buf[n] = '\0';
            req.body += buf;
        }
    }

    // ── Router ────────────────────────────────────────────────────────────
    std::string resp;
    try {
        if      (req.path == "/api/events")        resp = routeApiEvents(req);
        else if (req.path == "/api/stats")         resp = routeApiStats(req);
        else if (req.path == "/api/search")        resp = routeApiSearch(req);
        else if (req.path == "/api/files" && req.method == "GET")
                                                   resp = routeApiFiles(req);
        else if (req.path == "/api/files/load")    resp = routeApiFilesLoad(req);
        else if (req.path == "/api/files/remove")  resp = routeApiFilesRemove(req);
        else if (req.path == "/api/events/remove") resp = routeApiFilesRemove(req); // legacy dashboard UI alias
        else if (req.path == "/api/files/read")    resp = routeApiFilesRead(req);
        else if (req.path == "/api/files/append")  resp = routeApiFilesAppend(req);
        else if (req.path == "/api/auth")          resp = routeApiAuth(req);
        else if (req.path == "/api/risk")          resp = routeApiRisk(req);
        else                                       resp = routeStatic(req);
    } catch (const std::exception& ex) {
        resp = jsonResponse(500, R"({"error":")" + escapeJson(ex.what()) + R"("})");
    }

    sendResponse(fd, resp);
    ::close(fd);
}

// ─────────────────────────────────────────────────────────────────────────
//  Request parser
// ─────────────────────────────────────────────────────────────────────────
WebDashboardServer::Request WebDashboardServer::parseRequest(const std::string& raw) {
    Request req;
    std::istringstream ss(raw);
    std::string line;

    // First line: METHOD /path?query HTTP/1.1
    if (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::istringstream fl(line);
        std::string pathFull, proto;
        fl >> req.method >> pathFull >> proto;
        auto qpos = pathFull.find('?');
        if (qpos != std::string::npos) {
            req.path  = pathFull.substr(0, qpos);
            req.query = pathFull.substr(qpos + 1);
        } else {
            req.path = pathFull;
        }
    }

    // Headers
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) break; // end of headers
        auto colon = line.find(':');
        if (colon != std::string::npos) {
            std::string key = line.substr(0, colon);
            std::string val = line.substr(colon + 1);
            while (!val.empty() && val.front() == ' ') val.erase(val.begin());
            // Lowercase key for case-insensitive lookup
            std::transform(key.begin(), key.end(), key.begin(), ::tolower);
            req.headers[key] = val;
        }
    }

    // Body (rest of raw after headers)
    auto hdrEnd = raw.find("\r\n\r\n");
    if (hdrEnd != std::string::npos)
        req.body = raw.substr(hdrEnd + 4);

    // Extract Bearer token
    auto auth = req.headers.find("authorization");
    if (auth != req.headers.end()) {
        const std::string& av = auth->second;
        if (av.size() > 7 && av.substr(0, 7) == "Bearer ")
            req.authToken = av.substr(7);
    }

    return req;
}

std::unordered_map<std::string,std::string>
WebDashboardServer::parseQuery(const std::string& q) {
    std::unordered_map<std::string,std::string> m;
    std::istringstream ss(q);
    std::string token;
    while (std::getline(ss, token, '&')) {
        auto eq = token.find('=');
        if (eq != std::string::npos)
            m[token.substr(0, eq)] = token.substr(eq + 1);
    }
    return m;
}

// ─────────────────────────────────────────────────────────────────────────
//  JSON / response helpers
// ─────────────────────────────────────────────────────────────────────────
std::string WebDashboardServer::jsonResponse(int status, const std::string& body) {
    std::string statusLine;
    if      (status == 200) statusLine = "200 OK";
    else if (status == 201) statusLine = "201 Created";
    else if (status == 400) statusLine = "400 Bad Request";
    else if (status == 401) statusLine = "401 Unauthorized";
    else if (status == 403) statusLine = "403 Forbidden";
    else if (status == 404) statusLine = "404 Not Found";
    else                    statusLine = "500 Internal Server Error";

    return "HTTP/1.1 " + statusLine + "\r\n"
           "Content-Type: application/json\r\n"
           "Access-Control-Allow-Origin: *\r\n"
           "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
           "Access-Control-Allow-Headers: Authorization, Content-Type\r\n"
           "Content-Length: " + std::to_string(body.size()) + "\r\n"
           "\r\n" + body;
}

std::string WebDashboardServer::errorJson(const std::string& msg) {
    return R"({"error":")" + escapeJson(msg) + R"("})";
}

void WebDashboardServer::sendResponse(int fd, const std::string& resp) {
    const char* data = resp.c_str();
    size_t      rem  = resp.size();
    while (rem > 0) {
        ssize_t n = ::send(fd, data, rem, MSG_NOSIGNAL);
        if (n <= 0) break;
        data += n; rem -= static_cast<size_t>(n);
    }
}

// ─────────────────────────────────────────────────────────────────────────
//  RBAC
// ─────────────────────────────────────────────────────────────────────────
std::string WebDashboardServer::generateToken() const {
    static std::random_device rd;
    static std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint64_t> dist;
    std::ostringstream oss;
    oss << std::hex << std::setfill('0')
        << std::setw(16) << dist(gen)
        << std::setw(16) << dist(gen);
    return oss.str();
}

bool WebDashboardServer::isAdmin(const Request& req) const {
    std::lock_guard<std::mutex> lk(tokenMutex_);
    return adminTokens_.count(req.authToken) > 0;
}

bool WebDashboardServer::isAuthenticated(const Request& req) const {
    std::lock_guard<std::mutex> lk(tokenMutex_);
    return adminTokens_.count(req.authToken) > 0 ||
           userTokens_.count(req.authToken) > 0;
}

// ─────────────────────────────────────────────────────────────────────────
//  Route: POST /api/auth
// ─────────────────────────────────────────────────────────────────────────
std::string WebDashboardServer::routeApiAuth(const Request& req) {
    if (req.method == "OPTIONS")
        return jsonResponse(200, "{}");
    if (req.method != "POST")
        return jsonResponse(405, errorJson("Method Not Allowed"));

    // Simple JSON parse: extract "password" field
    std::string body = req.body;
    auto extract = [&](const std::string& key) -> std::string {
        auto pos = body.find("\"" + key + "\"");
        if (pos == std::string::npos) return "";
        pos = body.find(":", pos);
        if (pos == std::string::npos) return "";
        pos = body.find("\"", pos);
        if (pos == std::string::npos) return "";
        auto end = body.find("\"", pos + 1);
        if (end == std::string::npos) return "";
        return body.substr(pos + 1, end - pos - 1);
    };

    std::string password = extract("password");
    std::string token    = generateToken();

    if (password == cfg_.adminPassword) {
        std::lock_guard<std::mutex> lk(tokenMutex_);
        adminTokens_.insert(token);
        return jsonResponse(200,
            R"({"role":"admin","token":")" + token + R"("})");
    } else {
        // Any other password = read-only user token
        std::lock_guard<std::mutex> lk(tokenMutex_);
        userTokens_.insert(token);
        return jsonResponse(200,
            R"({"role":"user","token":")" + token + R"("})");
    }
}

// ─────────────────────────────────────────────────────────────────────────
//  Route: GET /api/events?page=1&limit=50&type=Error&q=text
// ─────────────────────────────────────────────────────────────────────────
std::string WebDashboardServer::routeApiEvents(const Request& req) {
    auto params = parseQuery(req.query);
    int  page   = params.count("page")  ? std::stoi(params["page"])  : 1;
    int  limit  = params.count("limit") ? std::stoi(params["limit"]) : 50;
    std::string typeFilter = params.count("type") ? params["type"] : "";
    std::string qFilter    = params.count("q")    ? params["q"]    : "";

    page  = std::max(1, page);
    limit = std::clamp(limit, 1, 200);

    const auto& logs = manager_.getLogs();
    std::lock_guard<std::mutex> lk(manager_.getLogsMutex());

    // Filter
    std::vector<const Event*> filtered;
    filtered.reserve(logs.size());
    for (const Event* e : logs) {
        if (!typeFilter.empty() && e->getType() != typeFilter) continue;
        if (!qFilter.empty()) {
            std::string hay = e->getDescription() + e->getUserName()
                            + e->getTimestamp()   + e->getType()
                            + e->getIPAddress();
            std::string needle = qFilter;
            std::transform(hay.begin(), hay.end(), hay.begin(), ::tolower);
            std::transform(needle.begin(), needle.end(), needle.begin(), ::tolower);
            if (hay.find(needle) == std::string::npos) continue;
        }
        filtered.push_back(e);
    }

    int total = static_cast<int>(filtered.size());
    int totalPages = (total + limit - 1) / limit;
    int offset = (page - 1) * limit;

    std::ostringstream json;
    json << R"({"total":)" << total
         << R"(,"page":)"  << page
         << R"(,"totalPages":)" << totalPages
         << R"(,"limit":)" << limit
         << R"(,"events":[)";

    bool first = true;
    for (int i = offset; i < std::min(offset + limit, total); ++i) {
        if (!first) json << ",";
        json << eventToJson(filtered[i]);
        first = false;
    }
    json << "]}";

    return jsonResponse(200, json.str());
}

// ─────────────────────────────────────────────────────────────────────────
//  Route: GET /api/stats
// ─────────────────────────────────────────────────────────────────────────
std::string WebDashboardServer::routeApiStats(const Request& req) {
    (void)req;
    const auto& logs = manager_.getLogs();
    std::lock_guard<std::mutex> lk(manager_.getLogsMutex());

    int total = 0, errors = 0, warnings = 0, logins = 0, activities = 0;
    std::unordered_map<std::string, int> countries, processes, days;

    constexpr size_t kIsoDateLength = 10;
    constexpr size_t kIsoYearMonthSep = 4;
    constexpr size_t kIsoMonthDaySep = 7;

    for (const Event* e : logs) {
        ++total;
        const std::string& t = e->getType();
        if      (t == "Error")    ++errors;
        else if (t == "Warning")  ++warnings;
        else if (t == "Login")    ++logins;
        else if (t == "Activity") ++activities;

        if (!e->getGeoCountry().empty() && e->getGeoCountry() != "Unknown")
            ++countries[e->getGeoCountry()];

        // Extract process from extra fields
        std::string proc;
        if (auto* le = dynamic_cast<const LoginEvent*>(e))    proc = le->getExtra2();
        else if (auto* ee = dynamic_cast<const ErrorEvent*>(e))   proc = ee->getExtra2();
        else if (auto* we = dynamic_cast<const WarningEvent*>(e)) proc = we->getExtra2();
        else if (auto* ae = dynamic_cast<const ActivityEvent*>(e)) proc = ae->getExtra2();
        if (!proc.empty()) ++processes[proc];

        // Day bucket from timestamp: detect "YYYY-MM-DD" via hyphens, else legacy "Jul  1"
        const std::string& ts = e->getTimestamp();
        std::string day;
        if (ts.size() >= kIsoDateLength &&
            ts[kIsoYearMonthSep] == '-' &&
            ts[kIsoMonthDaySep] == '-') {
            day = ts.substr(0, kIsoDateLength);
        } else if (ts.size() >= 6) {
            day = ts.substr(0, 6);
        } else {
            day = "?";
        }
        ++days[day];
    }

    std::ostringstream json;
    json << R"({"total":)"      << total
         << R"(,"errors":)"     << errors
         << R"(,"warnings":)"   << warnings
         << R"(,"logins":)"     << logins
         << R"(,"activities":)" << activities
         << R"(,"wsClients":)"  << ws_.clientCount();

    // Countries — sorted array [{name,count}] for frontend chart
    std::vector<std::pair<std::string,int>> countryVec(countries.begin(), countries.end());
    std::sort(countryVec.begin(), countryVec.end(),
              [](auto& a, auto& b){ return a.second > b.second; });
    if (countryVec.size() > 20) countryVec.resize(20);
    json << R"(,"countries":[)";
    bool first = true;
    for (auto& [k, v] : countryVec) {
        if (!first) json << ",";
        json << R"({"name":")" << escapeJson(k) << R"(","count":)" << v << "}";
        first = false;
    }
    json << "]";

    // Top processes (top 10)
    std::vector<std::pair<std::string,int>> procVec(processes.begin(), processes.end());
    std::sort(procVec.begin(), procVec.end(),
              [](auto& a, auto& b){ return a.second > b.second; });
    if (procVec.size() > 10) procVec.resize(10);
    json << R"(,"processes":[)";
    first = true;
    for (auto& [k, v] : procVec) {
        if (!first) json << ",";
        json << R"({"name":")" << escapeJson(k) << R"(","count":)" << v << "}";
        first = false;
    }
    json << "]";

    // Timeline buckets — sorted by key, use "hour" field for frontend compat
    std::vector<std::pair<std::string,int>> dayVec(days.begin(), days.end());
    std::sort(dayVec.begin(), dayVec.end(),
              [](auto& a, auto& b){ return a.first < b.first; });
    json << R"(,"timeline":[)";
    first = true;
    for (auto& [k, v] : dayVec) {
        if (!first) json << ",";
        json << R"({"hour":")" << escapeJson(k) << R"(","count":)" << v << "}";
        first = false;
    }
    json << "]}";

    return jsonResponse(200, json.str());
}

// ─────────────────────────────────────────────────────────────────────────
//  Route: GET /api/search?q=text&page=1&limit=15
// ─────────────────────────────────────────────────────────────────────────
std::string WebDashboardServer::routeApiSearch(const Request& req) {
    auto params = parseQuery(req.query);
    std::string q  = params.count("q") ? params["q"] : "";
    int page       = params.count("page")  ? std::stoi(params["page"])  : 1;
    int limit      = params.count("limit") ? std::stoi(params["limit"]) : 15;
    page  = std::max(1, page);
    limit = std::clamp(limit, 1, 100);

    if (q.empty())
        return jsonResponse(400, errorJson("q parameter required"));

    std::string qLow = q;
    std::transform(qLow.begin(), qLow.end(), qLow.begin(), ::tolower);

    const auto& logs = manager_.getLogs();
    std::lock_guard<std::mutex> lk(manager_.getLogsMutex());

    std::vector<const Event*> hits;
    for (const Event* e : logs) {
        std::string hay = e->getDescription() + " " + e->getUserName() + " "
                        + e->getTimestamp()   + " " + e->getType()     + " "
                        + e->getIPAddress()   + " " + e->getGeoCity()  + " "
                        + e->getGeoCountry();
        std::transform(hay.begin(), hay.end(), hay.begin(), ::tolower);
        if (hay.find(qLow) != std::string::npos)
            hits.push_back(e);
    }

    int total      = static_cast<int>(hits.size());
    int totalPages = (total + limit - 1) / limit;
    int offset     = (page - 1) * limit;

    std::ostringstream json;
    json << R"({"query":")" << escapeJson(q) << "\""
         << R"(,"total":)"      << total
         << R"(,"page":)"       << page
         << R"(,"totalPages":)" << totalPages
         << R"(,"limit":)"      << limit;

    // Count errors/warnings in result set for the Results Page aggregate stats
    int resErrors = 0, resWarnings = 0;
    for (const Event* e : hits) {
        if (e->getType() == "Error")   ++resErrors;
        if (e->getType() == "Warning") ++resWarnings;
    }
    json << R"(,"errors":)"   << resErrors
         << R"(,"warnings":)" << resWarnings
         << R"(,"events":[)";

    bool first = true;
    for (int i = offset; i < std::min(offset + limit, total); ++i) {
        if (!first) json << ",";
        json << eventToJson(hits[i]);
        first = false;
    }
    json << "]}";
    return jsonResponse(200, json.str());
}

// ─────────────────────────────────────────────────────────────────────────
//  Route: GET /api/files  [Admin]
// ─────────────────────────────────────────────────────────────────────────
std::string WebDashboardServer::routeApiFiles(const Request& req) {
    if (!isAdmin(req))
        return jsonResponse(403, errorJson("Admin access required"));

    std::ostringstream json;
    json << R"({"files":[)";
    bool first = true;

    try {
        for (auto& entry : fs::directory_iterator(cfg_.logDir)) {
            const auto& p = entry.path();
            std::string ext = p.extension().string();
            if (ext != ".log" && ext != ".csv") continue;

            if (!first) json << ",";
            json << R"({"name":")" << escapeJson(p.filename().string()) << "\""
                 << R"(,"path":")" << escapeJson(p.string())            << "\""
                 << R"(,"size":)"  << fs::file_size(p)
                 << R"(,"ext":")"  << escapeJson(ext) << "\""
                 << "}";
            first = false;
        }
    } catch (...) {}

    json << "]}";
    return jsonResponse(200, json.str());
}

// ─────────────────────────────────────────────────────────────────────────
//  Route: POST /api/files/load  [Admin]
//  body: {"path":"Mac.log"}
// ─────────────────────────────────────────────────────────────────────────
std::string WebDashboardServer::routeApiFilesLoad(const Request& req) {
    if (req.method == "OPTIONS") return jsonResponse(200, "{}");
    if (!isAdmin(req))
        return jsonResponse(403, errorJson("Admin access required"));

    // Extract path from body JSON
    auto extract = [&](const std::string& key) -> std::string {
        auto pos = req.body.find("\"" + key + "\"");
        if (pos == std::string::npos) return "";
        pos = req.body.find(":", pos);
        if (pos == std::string::npos) return "";
        pos = req.body.find("\"", pos);
        if (pos == std::string::npos) return "";
        auto end = req.body.find("\"", pos + 1);
        return req.body.substr(pos + 1, end - pos - 1);
    };

    std::string path = extract("path");
    if (path.empty())
        return jsonResponse(400, errorJson("path field required"));

    // Safety: no path traversal
    if (path.find("..") != std::string::npos)
        return jsonResponse(400, errorJson("Path traversal not allowed"));

    try {
        FileHandler fh(path);
        int before = manager_.getCount();
        fh.loadFromFile(manager_);
        int added = manager_.getCount() - before;

        // Broadcast stats update to all WS clients
        ws_.broadcast(R"({"type":"reload","added":)" + std::to_string(added) + "}");

        return jsonResponse(200,
            R"({"ok":true,"added":)" + std::to_string(added) +
            R"(,"path":")" + escapeJson(path) + R"("})");
    } catch (const std::exception& ex) {
        return jsonResponse(400, errorJson(ex.what()));
    }
}

// ─────────────────────────────────────────────────────────────────────────
//  Route: POST /api/files/remove  [Admin]
// ─────────────────────────────────────────────────────────────────────────
std::string WebDashboardServer::routeApiFilesRemove(const Request& req) {
    if (req.method == "OPTIONS") return jsonResponse(200, "{}");
    if (!isAdmin(req))
        return jsonResponse(403, errorJson("Admin access required"));

    auto extract = [&](const std::string& key) -> std::string {
        auto pos = req.body.find("\"" + key + "\"");
        if (pos == std::string::npos) return "";
        pos = req.body.find(":", pos);
        if (pos == std::string::npos) return "";
        pos = req.body.find("\"", pos);
        if (pos == std::string::npos) return "";
        auto end = req.body.find("\"", pos + 1);
        return req.body.substr(pos + 1, end - pos - 1);
    };

    std::string eventId = extract("id");
    if (eventId.empty())
        return jsonResponse(400, errorJson("id field required"));

    try {
        manager_.removeEvent(eventId);
        ws_.broadcast(R"({"type":"remove","id":")" + escapeJson(eventId) + "\"}");
        return jsonResponse(200, R"({"ok":true})");
    } catch (const std::out_of_range&) {
        return jsonResponse(404, errorJson("Event ID not found: " + eventId));
    }
}

// ─────────────────────────────────────────────────────────────────────────
//  Route: GET /api/files/read?path=foo.log&offset=0&limit=200  [Admin]
// ─────────────────────────────────────────────────────────────────────────
std::string WebDashboardServer::routeApiFilesRead(const Request& req) {
    if (!isAdmin(req))
        return jsonResponse(403, errorJson("Admin access required"));

    auto params  = parseQuery(req.query);
    std::string path = params.count("path") ? params["path"] : "";
    int offset   = params.count("offset") ? std::stoi(params["offset"]) : 0;
    int limit    = params.count("limit")  ? std::stoi(params["limit"])  : 200;
    limit = std::clamp(limit, 1, 1000);
    offset = std::max(0, offset);

    if (path.empty())
        return jsonResponse(400, errorJson("path required"));
    if (path.find("..") != std::string::npos)
        return jsonResponse(400, errorJson("Path traversal not allowed"));

    std::ifstream f(path);
    if (!f.is_open())
        return jsonResponse(404, errorJson("File not found: " + path));

    std::ostringstream json;
    json << R"({"path":")" << escapeJson(path) << R"(","lines":[)";

    std::string line;
    int lineNo = 0;
    bool first = true;
    while (std::getline(f, line)) {
        if (lineNo < offset) { ++lineNo; continue; }
        if (lineNo >= offset + limit) break;
        if (!first) json << ",";
        json << "{\"n\":" << lineNo
             << ",\"t\":\"" << escapeJson(line) << "\"}";
        first = false;
        ++lineNo;
    }
    json << "],\"total\":" << lineNo << "}";
    return jsonResponse(200, json.str());
}

// ─────────────────────────────────────────────────────────────────────────
//  Route: POST /api/files/append  [Admin]
//  body: {"path":"foo.log","line":"raw text"}
// ─────────────────────────────────────────────────────────────────────────
std::string WebDashboardServer::routeApiFilesAppend(const Request& req) {
    if (req.method == "OPTIONS") return jsonResponse(200, "{}");
    if (!isAdmin(req))
        return jsonResponse(403, errorJson("Admin access required"));

    auto extract = [&](const std::string& key) -> std::string {
        auto pos = req.body.find("\"" + key + "\"");
        if (pos == std::string::npos) return "";
        pos = req.body.find(":", pos);
        if (pos == std::string::npos) return "";
        pos = req.body.find("\"", pos);
        if (pos == std::string::npos) return "";
        auto end = req.body.find("\"", pos + 1);
        return req.body.substr(pos + 1, end - pos - 1);
    };

    std::string path = extract("path");
    std::string line = extract("line");

    if (path.empty() || line.empty())
        return jsonResponse(400, errorJson("path and line fields required"));
    if (path.find("..") != std::string::npos)
        return jsonResponse(400, errorJson("Path traversal not allowed"));

    std::ofstream f(path, std::ios::app);
    if (!f.is_open())
        return jsonResponse(400, errorJson("Cannot open file: " + path));

    f << line << "\n";
    f.flush();
    // LiveFileWatcher will pick this up on its next poll
    return jsonResponse(201, R"({"ok":true,"written":true})");
}

// ─────────────────────────────────────────────────────────────────────────
//  Route: GET /api/risk
//  Impossible-Travel detection: find IPs that appear in multiple distinct
//  cities/countries.  Returns a JSON array of flagged IPs with risk score,
//  city/country list, and event count.
//  Risk score (0–100): based on number of distinct countries * 20, capped
//  at 95 so there's always room for human judgment.
// ─────────────────────────────────────────────────────────────────────────
std::string WebDashboardServer::routeApiRisk(const Request& req) {
    (void)req;
    const auto& logs = manager_.getLogs();
    std::lock_guard<std::mutex> lk(manager_.getLogsMutex());

    // ip → { set<city>, set<country>, count, threatScore }
    struct IPInfo {
        std::set<std::string> cities;
        std::set<std::string> countries;
        int count        = 0;
        int threatScore  = 0;   // DShield-derived severity (0-95)
    };
    std::unordered_map<std::string, IPInfo> ipMap;

    // ── DShield threat-category scoring table ─────────────────────────────
    auto categoryScore = [](const std::string& cat) -> int {
        if (cat == "dshieldssh")    return 50;   // SSH brute-force attempts
        if (cat == "webscanner")    return 40;   // Web vulnerability scanning
        if (cat == "talos")         return 60;   // Cisco Talos blacklist
        if (cat == "tldns")         return 35;   // DNS abuse
        if (cat == "openresolver")  return 25;   // Open DNS resolver
        if (cat == "mastodon")      return 20;
        return 0;
    };

    for (const Event* e : logs) {
        const std::string& ip = e->getIPAddress();
        if (ip.empty()) continue;
        // Skip RFC-1918 / loopback addresses (private IPs aren't interesting for travel)
        if (ip.rfind("10.",    0) == 0) continue;
        if (ip.rfind("192.168",0) == 0) continue;
        if (ip.rfind("172.",   0) == 0) continue;
        if (ip.rfind("127.",   0) == 0) continue;

        auto& info = ipMap[ip];
        ++info.count;
        if (e->hasGeo()) {
            if (!e->getGeoCity().empty()    && e->getGeoCity()    != "Unknown")
                info.cities.insert(e->getGeoCity());
            if (!e->getGeoCountry().empty() && e->getGeoCountry() != "Unknown")
                info.countries.insert(e->getGeoCountry());
        }

        // ── DShield threat scoring ────────────────────────────────────────
        if (auto* ee = dynamic_cast<const ErrorEvent*>(e)) {
            const std::string& src = ee->getSourceModule();
            if (src == "DShieldTopAttacker") {
                // reports count (errorCode) → score: 1000 reports = 10 pts,
                // capped at 90.  Top attackers commonly hit 200k+ reports.
                int reports = ee->getErrorCode();
                int score   = std::min(90, reports / 1000);
                if (score > info.threatScore) info.threatScore = score;
            } else {
                // intelfeed categories (dshieldssh, webscanner, ...)
                int score = categoryScore(src);
                if (score > info.threatScore) info.threatScore = score;
            }
        }
    }

    // Collect flagged IPs.  Two flag conditions:
    //   1. Impossible-travel: ≥2 distinct countries
    //   2. DShield threat:    threatScore ≥ 30
    struct Flagged {
        std::string ip;
        int         riskScore;
        int         count;
        std::vector<std::string> cities;
        std::vector<std::string> countries;
    };
    std::vector<Flagged> flagged;
    flagged.reserve(ipMap.size() / 10 + 1);

    for (auto& [ip, info] : ipMap) {
        const bool travelFlag = info.countries.size() >= 2;
        const bool threatFlag = info.threatScore >= 30;
        if (!travelFlag && !threatFlag) continue;

        int travelScore = travelFlag
            ? std::min(95, static_cast<int>(info.countries.size()) * 20
                         + static_cast<int>(info.cities.size())    * 5)
            : 0;
        int score = std::max(travelScore, info.threatScore);

        flagged.push_back({
            ip, score, info.count,
            std::vector<std::string>(info.cities.begin(),    info.cities.end()),
            std::vector<std::string>(info.countries.begin(), info.countries.end())
        });
    }

    // Sort by risk score descending
    std::sort(flagged.begin(), flagged.end(),
              [](const Flagged& a, const Flagged& b){ return a.riskScore > b.riskScore; });

    std::ostringstream json;
    json << R"({"flagged":[)";
    bool first = true;
    for (auto& f : flagged) {
        if (!first) json << ",";
        first = false;
        json << "{\"ip\":\""         << escapeJson(f.ip) << "\""
             << ",\"risk\":"         << f.riskScore
             << ",\"riskScore\":"    << f.riskScore
             << ",\"count\":"        << f.count
             << ",\"cities\":[";
        bool ff = true;
        for (auto& c : f.cities) {
            if (!ff) json << ",";
            json << "\"" << escapeJson(c) << "\"";
            ff = false;
        }
        json << "],\"countries\":[";
        ff = true;
        for (auto& c : f.countries) {
            if (!ff) json << ",";
            json << "\"" << escapeJson(c) << "\"";
            ff = false;
        }
        json << "]}";
    }
    json << "],\"total\":" << flagged.size() << "}";
    return jsonResponse(200, json.str());
}

// ─────────────────────────────────────────────────────────────────────────
//  Route: static file serving
// ─────────────────────────────────────────────────────────────────────────
std::string WebDashboardServer::routeStatic(const Request& req) {
    // Normalise path
    std::string p = req.path;
    if (p == "/" || p.empty()) p = "/index.html";

    // Prevent path traversal
    if (p.find("..") != std::string::npos)
        return "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n\r\n";

    std::string filePath = cfg_.dashboardDir + p;

    // Try to read file
    std::string content = readFile(filePath);
    if (content.empty() && p != "/index.html")
        return "HTTP/1.1 404 Not Found\r\nContent-Length: 9\r\n\r\nNot Found";

    if (!content.empty() && p == "/index.html") {
        const std::string token = "__WS_PORT__";
        const std::string wsPort = std::to_string(cfg_.wsPort);
        size_t pos = 0;
        while ((pos = content.find(token, pos)) != std::string::npos) {
            content.replace(pos, token.size(), wsPort);
            pos += wsPort.size();
        }
    }

    std::string ct = contentType(filePath);
    return "HTTP/1.1 200 OK\r\n"
           "Content-Type: " + ct + "\r\n"
           "Content-Length: " + std::to_string(content.size()) + "\r\n"
           "Cache-Control: no-cache\r\n"
           "\r\n" + content;
}

// ─────────────────────────────────────────────────────────────────────────
//  JSON event serialiser
// ─────────────────────────────────────────────────────────────────────────
std::string WebDashboardServer::eventToJson(const Event* e) {
    std::string extra1, extra2;
    if (auto* le = dynamic_cast<const LoginEvent*>(e))    { extra1=le->getExtra1(); extra2=le->getExtra2(); }
    else if (auto* ee = dynamic_cast<const ErrorEvent*>(e))   { extra1=ee->getExtra1(); extra2=ee->getExtra2(); }
    else if (auto* we = dynamic_cast<const WarningEvent*>(e)) { extra1=we->getExtra1(); extra2=we->getExtra2(); }
    else if (auto* ae = dynamic_cast<const ActivityEvent*>(e)){ extra1=ae->getExtra1(); extra2=ae->getExtra2(); }

    std::ostringstream j;
    j << "{"
      << R"("id":")"        << escapeJson(e->getEventID())     << "\","
      << R"("ts":")"        << escapeJson(e->getTimestamp())   << "\","
      << R"("timestamp":")" << escapeJson(e->getTimestamp())   << "\","
      << R"("user":")"      << escapeJson(e->getUserName())    << "\","
      << R"("type":")"      << escapeJson(e->getType())        << "\","
      << R"("desc":")"      << escapeJson(e->getDescription()) << "\","
      << R"("message":")"   << escapeJson(e->getDescription()) << "\","
      << R"("ip":")"        << escapeJson(e->getIPAddress())   << "\","
      << R"("x1":")"        << escapeJson(extra1)              << "\","
      << R"("x2":")"        << escapeJson(extra2)              << "\","
      << R"("lat":)"        << e->getLatitude()                << ","
      << R"("lon":)"        << e->getLongitude()               << ","
      << R"("city":")"      << escapeJson(e->getGeoCity())     << "\","
      << R"("country":")"   << escapeJson(e->getGeoCountry())  << "\","
      << R"("iso":")"       << escapeJson(e->getGeoIsoCode())  << "\""
      << "}";
    return j.str();
}

std::string WebDashboardServer::escapeJson(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

// ─────────────────────────────────────────────────────────────────────────
//  Static file helpers
// ─────────────────────────────────────────────────────────────────────────
std::string WebDashboardServer::contentType(const std::string& path) {
    auto ext = fs::path(path).extension().string();
    if (ext == ".html") return "text/html; charset=utf-8";
    if (ext == ".css")  return "text/css";
    if (ext == ".js")   return "application/javascript";
    if (ext == ".json") return "application/json";
    if (ext == ".png")  return "image/png";
    if (ext == ".svg")  return "image/svg+xml";
    if (ext == ".ico")  return "image/x-icon";
    return "application/octet-stream";
}

std::string WebDashboardServer::readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return {};
    return std::string(std::istreambuf_iterator<char>(f),
                       std::istreambuf_iterator<char>());
}
