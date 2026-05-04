#include "DShieldInformer.h"

#include "LogManager.h"
#include "GeoLocator.h"
#include "ErrorEvent.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <ctime>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

// ── Configuration ────────────────────────────────────────────────────────
// Set CONTACT_EMAIL to a real email so DShield admins can reach out if your
// usage of their API ever causes a problem.  Change before deploying.
[[maybe_unused]] constexpr const char* CONTACT_EMAIL = "jangustau93@gmail.com";
constexpr const char* USER_AGENT    =
    "C-Legends-LogAnalyzer/2.0 (github.com/user/c-legends; "
    "contact: jangustau93@gmail.com)";

constexpr const char* URL_INTELFEED =
    "https://isc.sans.edu/api/intelfeed?json";
constexpr const char* URL_TOPIPS =
    "https://isc.sans.edu/api/topips/records/100?json";

constexpr long  CURL_TIMEOUT_SECONDS  = 10;
constexpr int   DEFAULT_RETRY_AFTER   = 300;   // 5 minutes if no header given

// Cap intelfeed processing — full feed is 100k+ IPs/cycle which would
// flood LogManager queue and bloat logs.csv. Top N is plenty for dashboard.
// Set to 0 to disable cap (process all entries).
constexpr int   MAX_INTELFEED_ENTRIES = 1000;

// ── libcurl callbacks ────────────────────────────────────────────────────
// Body callback: appends incoming bytes onto a std::string we pass via
// CURLOPT_WRITEDATA.
size_t writeBodyCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    auto* buf = static_cast<std::string*>(userp);
    buf->append(static_cast<char*>(contents), size * nmemb);
    return size * nmemb;
}

// Header callback: extracts the Retry-After value (if present) into a long
// stored at userp.  Header format is "Retry-After: <seconds>\r\n" per RFC 7231.
// We accept only the delta-seconds form (HTTP-date is rare in practice).
size_t writeHeaderCallback(char* buffer, size_t size, size_t nitems, void* userp) {
    const size_t total = size * nitems;
    std::string  line(buffer, total);

    // Lower-case the header name for case-insensitive matching
    std::string lower = line;
    for (char& c : lower) c = static_cast<char>(std::tolower(c));

    if (lower.rfind("retry-after:", 0) == 0) {
        auto colon = line.find(':');
        if (colon != std::string::npos) {
            std::string val = line.substr(colon + 1);
            // Trim whitespace + CRLF
            while (!val.empty() && (val.front() == ' ' || val.front() == '\t'))
                val.erase(0, 1);
            while (!val.empty() &&
                   (val.back() == '\r' || val.back() == '\n' ||
                    val.back() == ' '  || val.back() == '\t'))
                val.pop_back();
            try {
                long secs = std::stol(val);
                *static_cast<long*>(userp) = secs;
            } catch (...) {
                // Leave as -1 if value isn't a plain integer
            }
        }
    }
    return total;
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────
//  Construction / destruction
// ─────────────────────────────────────────────────────────────────────────
DShieldInformer::DShieldInformer(LogManager& manager, GeoLocator& geo)
    : manager_(manager), geo_(geo) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

DShieldInformer::~DShieldInformer() {
    stop();
    curl_global_cleanup();
}

// ─────────────────────────────────────────────────────────────────────────
//  Public API
// ─────────────────────────────────────────────────────────────────────────
int DShieldInformer::fetchOnce() {
    // Ensure consumer is live so pushEvent() actually drains.
    manager_.startProcessing();

    lastFetchAdded_.store(0);

    // ── Endpoint A: intelfeed ────────────────────────────────────────────
    {
        std::string body;
        if (fetchEndpoint(URL_INTELFEED, body)) {
            try {
                auto j = nlohmann::json::parse(body);
                if (j.is_array()) {
                    int processed = 0;
                    for (const auto& entry : j) {
                        if (MAX_INTELFEED_ENTRIES > 0 &&
                            processed >= MAX_INTELFEED_ENTRIES) break;
                        if (!entry.contains("ip")) continue;
                        std::string ip   = entry.value("ip", "");
                        std::string desc = entry.value("description", "");
                        if (ip.empty() || isDuplicate(ip)) continue;
                        enrichAndPush(
                            ip,
                            "dshield-intelfeed",
                            "DShield Intel: " + desc,
                            0,
                            desc);
                        ++processed;
                    }
                }
            } catch (const nlohmann::json::exception& ex) {
                std::cerr << "[DShield] intelfeed JSON parse error: "
                          << ex.what() << "\n";
            }
        }
    }

    // ── Endpoint B: topips ───────────────────────────────────────────────
    {
        std::string body;
        if (fetchEndpoint(URL_TOPIPS, body)) {
            try {
                auto j = nlohmann::json::parse(body);

                // Response can be either {"topips":[...]} or a bare array.
                const nlohmann::json* arr = nullptr;
                if (j.is_object() && j.contains("topips") &&
                    j["topips"].is_array()) {
                    arr = &j["topips"];
                } else if (j.is_array()) {
                    arr = &j;
                }

                if (arr) {
                    for (const auto& entry : *arr) {
                        // The "source" field is the attacker IP. Some
                        // historical responses wrapped it in <ipaddress>
                        // for XML; JSON uses "source" directly.
                        std::string ip = entry.value("source", "");
                        if (ip.empty()) ip = entry.value("ip", "");
                        if (ip.empty() || isDuplicate(ip)) continue;

                        int reports = 0;
                        if (entry.contains("reports")) {
                            try { reports = entry["reports"].get<int>(); }
                            catch (...) {
                                try {
                                    reports = std::stoi(
                                        entry["reports"].get<std::string>());
                                } catch (...) {}
                            }
                        }
                        int targets = 0;
                        if (entry.contains("targets")) {
                            try { targets = entry["targets"].get<int>(); }
                            catch (...) {
                                try {
                                    targets = std::stoi(
                                        entry["targets"].get<std::string>());
                                } catch (...) {}
                            }
                        }

                        std::string desc =
                            "DShield Top Attacker: " +
                            std::to_string(reports) + " reports, " +
                            std::to_string(targets) + " targets";

                        enrichAndPush(ip, "dshield-topips", desc, reports,
                                      "DShieldTopAttacker");
                    }
                }
            } catch (const nlohmann::json::exception& ex) {
                std::cerr << "[DShield] topips JSON parse error: "
                          << ex.what() << "\n";
            }
        }
    }

    return lastFetchAdded_.load();
}

void DShieldInformer::startLiveFeed(int intervalSeconds) {
    if (running_.exchange(true)) {
        std::cout << "[DShield] Live feed already running.\n";
        return;
    }
    if (intervalSeconds < 60) intervalSeconds = 60;   // sanity floor

    feedThread_ = std::jthread(
        [this, intervalSeconds](std::stop_token st) {
            feedLoop(st, intervalSeconds);
        });

    std::cout << "[DShield] Live threat feed started (interval = "
              << intervalSeconds << "s).\n";
}

void DShieldInformer::stop() {
    if (!running_.exchange(false)) return;
    feedThread_.request_stop();
    if (feedThread_.joinable()) feedThread_.join();
    std::cout << "[DShield] Live threat feed stopped.\n";
}

// ─────────────────────────────────────────────────────────────────────────
//  Internals
// ─────────────────────────────────────────────────────────────────────────
void DShieldInformer::feedLoop(std::stop_token st, int intervalSeconds) {
    while (!st.stop_requested()) {
        try {
            int added = fetchOnce();
            std::cout << "[DShield] Cycle complete: " << added
                      << " new event(s) pushed.\n";
        } catch (const std::exception& ex) {
            std::cerr << "[DShield] Unexpected error in feedLoop: "
                      << ex.what() << "\n";
        }

        sleepInterruptible(st, std::chrono::seconds(intervalSeconds));
    }
}

bool DShieldInformer::fetchEndpoint(const std::string& url,
                                    std::string&       outBody) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        std::cerr << "[DShield] curl_easy_init() failed.\n";
        return false;
    }

    outBody.clear();
    lastRetryAfter_ = -1;

    curl_easy_setopt(curl, CURLOPT_URL,            url.c_str());
    curl_easy_setopt(curl, CURLOPT_USERAGENT,      USER_AGENT);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,        CURL_TIMEOUT_SECONDS);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  writeBodyCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,      &outBody);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, writeHeaderCallback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA,     &lastRetryAfter_);

    CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        std::cerr << "[DShield] HTTP transport error for " << url
                  << ": " << curl_easy_strerror(rc) << "\n";
        return false;
    }

    if (status == 429) {
        long wait = (lastRetryAfter_ > 0) ? lastRetryAfter_
                                          : DEFAULT_RETRY_AFTER;
        std::cerr << "[DShield] Rate limited (HTTP 429). Sleeping "
                  << wait << "s before continuing.\n";
        // Block the calling (feed) thread; respects no stop_token here
        // so callers using fetchOnce() from CLI will simply wait.
        std::this_thread::sleep_for(std::chrono::seconds(wait));
        return false;
    }

    if (status < 200 || status >= 300) {
        std::cerr << "[DShield] Unexpected HTTP status " << status
                  << " for " << url << "\n";
        return false;
    }

    return true;
}

void DShieldInformer::enrichAndPush(const std::string& ip,
                                    const std::string& user,
                                    const std::string& desc,
                                    int                errorCode,
                                    const std::string& sourceModule) {
    auto* e = new ErrorEvent(nextID(), nowISO8601(), user, desc,
                             errorCode, sourceModule);
    e->setIPAddress(ip);
    e->setFullTimestamp(nowISO8601());

    GeoResult geo = geo_.lookup(ip);
    if (geo.found) {
        e->setGeo(geo.latitude, geo.longitude,
                  geo.city, geo.country, geo.isoCode);
    }

    manager_.pushEvent(e);
    lastFetchAdded_.fetch_add(1);
}

bool DShieldInformer::isDuplicate(const std::string& ip) {
    std::lock_guard<std::mutex> lk(seenMutex_);

    auto now = std::chrono::steady_clock::now();
    if (now - lastSeenReset_ > std::chrono::hours(24)) {
        seenIPs_.clear();
        lastSeenReset_ = now;
    }

    auto [_, inserted] = seenIPs_.insert(ip);
    return !inserted;
}

std::string DShieldInformer::nowISO8601() {
    auto t  = std::chrono::system_clock::to_time_t(
                  std::chrono::system_clock::now());
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

std::string DShieldInformer::nextID() {
    static std::atomic<int> counter{0};
    int n = counter.fetch_add(1) + 1;
    char buf[16];
    std::snprintf(buf, sizeof(buf), "D%05d", n);
    return buf;
}

void DShieldInformer::sleepInterruptible(std::stop_token st,
                                         std::chrono::seconds duration) {
    std::mutex                  m;
    std::condition_variable_any cv;
    std::unique_lock<std::mutex> lk(m);
    cv.wait_for(lk, st, duration, [&]{ return st.stop_requested(); });
}
