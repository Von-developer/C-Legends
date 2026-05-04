#ifndef DSHIELDINFORMER_H
#define DSHIELDINFORMER_H

// ─────────────────────────────────────────────────────────────────────────────
//  DShieldInformer  (C++20)
//
//  Pulls live threat intelligence from the SANS Internet Storm Center
//  (DShield) public REST API and converts each entry into an ErrorEvent
//  pushed through LogManager's thread-safe queue.
//
//  Two endpoints are polled per cycle:
//    - https://isc.sans.edu/api/intelfeed?json
//        Notable IPs with threat-category descriptions (daily-updated).
//    - https://isc.sans.edu/api/topips/records/100?json
//        Top 100 attacking IPs with attack volume and unique-target counts.
//
//  Each new IP is geo-enriched via GeoLocator and pushed via
//  LogManager::pushEvent(). The existing onNewEvent callback wired by
//  WebDashboardServer means terminal log AND live WebSocket dashboard
//  broadcast happen automatically — no extra wiring required here.
//
//  Rate limiting:
//    - Custom User-Agent set on every request (API blocks defaults).
//    - HTTP 429 response: respects Retry-After header if present, else
//      sleeps 300 seconds then resumes.
//    - Network/timeout/JSON errors: logged to stderr, cycle skipped, no crash.
//
//  Deduplication:
//    - In-memory set of seen IPs cleared every 24 hours.
//    - Same IP never produces two events within a 24-hour window.
// ─────────────────────────────────────────────────────────────────────────────

#include <string>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>
#include <unordered_set>

class LogManager;
class GeoLocator;

class DShieldInformer {
public:
    DShieldInformer(LogManager& manager, GeoLocator& geo);
    ~DShieldInformer();

    // Non-copyable (owns curl global state + jthread)
    DShieldInformer(const DShieldInformer&)            = delete;
    DShieldInformer& operator=(const DShieldInformer&) = delete;

    // Single blocking fetch of both endpoints. Safe to call from any thread.
    // Returns the number of new (non-duplicate) events pushed.
    int  fetchOnce();

    // Launch background polling thread. No-op if already running.
    void startLiveFeed(int intervalSeconds);

    // Request the polling thread to stop and join. No-op if not running.
    void stop();

    bool isRunning() const { return running_.load(); }

private:
    LogManager& manager_;
    GeoLocator& geo_;

    std::jthread feedThread_;
    std::atomic<bool> running_{false};

    // Deduplication
    std::mutex                      seenMutex_;
    std::unordered_set<std::string> seenIPs_;
    std::chrono::steady_clock::time_point lastSeenReset_{
        std::chrono::steady_clock::now()};

    // Set by the libcurl header callback when a 429 response carries
    // a Retry-After value; -1 means "no header seen on this request".
    long lastRetryAfter_{-1};

    // Counter for new events added during the most recent fetchOnce()
    std::atomic<int> lastFetchAdded_{0};

    // ── Internal helpers ──────────────────────────────────────────────────
    void feedLoop(std::stop_token st, int intervalSeconds);

    // Returns true on success (HTTP 200 + body fetched into outBody).
    // Returns false on transport error, non-200 status, or rate-limit.
    bool fetchEndpoint(const std::string& url, std::string& outBody);

    void enrichAndPush(const std::string& ip,
                       const std::string& user,
                       const std::string& desc,
                       int                errorCode,
                       const std::string& sourceModule);

    bool isDuplicate(const std::string& ip);

    static std::string nowISO8601();
    static std::string nextID();

    // Stop-token-aware sleep that wakes early on stop request.
    static void sleepInterruptible(std::stop_token st,
                                   std::chrono::seconds duration);
};

#endif // DSHIELDINFORMER_H
