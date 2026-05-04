#include "FileHandler.h"
#include "LoginEvent.h"
#include "ErrorEvent.h"
#include "WarningEvent.h"
#include "ActivityEvent.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <stdexcept>
#include <algorithm>
#include <cctype>
#include <regex>
#include <iomanip>
#include <chrono>
#include <ctime>
#include <filesystem>

FileHandler::FileHandler(const std::string& file) : fileName(file) {}

bool FileHandler::isMacLog() const {
    // ends with .log
    return fileName.size() >= 4 &&
           fileName.substr(fileName.size() - 4) == ".log";
}

// ── Keyword classifier ────────────────────────────────────────────────────
std::string FileHandler::classifyMacLine(const std::string& process,
                                          const std::string& message) {
    // Convert both to lowercase for case-insensitive matching
    std::string msgL = message, procL = process;
    std::transform(msgL.begin(),  msgL.end(),  msgL.begin(),  ::tolower);
    std::transform(procL.begin(), procL.end(), procL.begin(), ::tolower);

    // Login / session events
    if (msgL.find("wake") != std::string::npos ||
        msgL.find("sleep") != std::string::npos ||
        msgL.find("login")  != std::string::npos ||
        msgL.find("logout") != std::string::npos ||
        msgL.find("auth")   != std::string::npos ||
        procL.find("loginwindow") != std::string::npos)
        return "Login";

    // Error events
    if (msgL.find("error")  != std::string::npos ||
        msgL.find("fail")   != std::string::npos ||
        msgL.find("crash")  != std::string::npos ||
        msgL.find("abort")  != std::string::npos ||
        msgL.find("fault")  != std::string::npos)
        return "Error";

    // Warning events
    if (msgL.find("warn")     != std::string::npos ||
        msgL.find("caution")  != std::string::npos ||
        msgL.find("critical") != std::string::npos ||
        msgL.find("exceeded") != std::string::npos)
        return "Warning";

    // Sandbox / denied → Activity (security-related activity)
    if (msgL.find("deny")    != std::string::npos ||
        msgL.find("denied")  != std::string::npos ||
        procL.find("sandbox") != std::string::npos)
        return "Activity";

    // Default: general activity
    return "Activity";
}

// ── macOS syslog parser ────────────────────────────────────────────────────
// Format: "Mon DD HH:MM:SS hostname process[pid]: message"
//  e.g.:  "Jul  1 09:00:55 authorMacBook-Pro kernel[0]: Wake reason: ?"
Event* FileHandler::parseMacLogLine(const std::string& rawLine) const {
    // Strip Windows-style \r if present
    std::string line = rawLine;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) throw std::runtime_error("empty line");

    // Use regex to split the fixed syslog header
    // Group 1: timestamp (Mon DD HH:MM:SS)
    // Group 2: hostname
    // Group 3: process name (without [pid])
    // Group 4: pid
    // Group 5: message
    static const std::regex logRe(
        R"(^(\w+\s+\d+\s+\d+:\d+:\d+)\s+(\S+)\s+([^\[]+)\[(\d+)\]:\s*(.*))");

    std::smatch m;
    if (!std::regex_match(line, m, logRe))
        throw std::runtime_error("Line does not match syslog format");

    std::string timestamp = m[1].str();   // "Jul  1 09:00:55"
    std::string hostname  = m[2].str();   // "authorMacBook-Pro"
    std::string process   = m[3].str();   // "kernel"
    std::string pid       = m[4].str();   // "0"
    std::string message   = m[5].str();   // actual log message

    // Trim trailing whitespace from process name
    while (!process.empty() && process.back() == ' ') process.pop_back();

    // Auto-classify event type
    std::string type = classifyMacLine(process, message);

    // Build a stable unique ID: process[pid] is not unique across lines,
    // so we use a static counter
    static int counter = 0;
    std::ostringstream idss;
    idss << "M" << std::setw(5) << std::setfill('0') << (++counter);
    std::string id = idss.str();

    std::string user = hostname;   // hostname acts as "user" in this context
    std::string desc = message.size() > 80
                       ? message.substr(0, 77) + "..."
                       : message;

    if (type == "Login") {
        bool success = (message.find("fail") == std::string::npos &&
                        message.find("Fail") == std::string::npos);
        return new LoginEvent(id, timestamp, user, desc,
                              success, process);
    } else if (type == "Error") {
        // Try to pull a numeric code from the message
        int code = 0;
        std::smatch cm;
        static const std::regex codeRe(R"(\b(\d{3,5})\b)");
        if (std::regex_search(message, cm, codeRe))
            code = std::stoi(cm[1].str());
        return new ErrorEvent(id, timestamp, user, desc, code, process);
    } else if (type == "Warning") {
        // Severity based on keywords
        std::string sevMsg = message;
        std::transform(sevMsg.begin(), sevMsg.end(), sevMsg.begin(), ::tolower);
        std::string sev = "LOW";
        if (sevMsg.find("critical") != std::string::npos ||
            sevMsg.find("exceeded") != std::string::npos) sev = "HIGH";
        else if (sevMsg.find("warn") != std::string::npos)  sev = "MEDIUM";
        return new WarningEvent(id, timestamp, user, desc,
                                sev, process);
    } else {
        // Activity — action = deny/read/write/general, resource = process
        std::string action = "SYSTEM";
        std::string msgL = message;
        std::transform(msgL.begin(), msgL.end(), msgL.begin(), ::tolower);
        if (msgL.find("deny") != std::string::npos) action = "DENY";
        else if (msgL.find("write") != std::string::npos) action = "WRITE";
        else if (msgL.find("read")  != std::string::npos) action = "READ";
        return new ActivityEvent(id, timestamp, user, desc,
                                 action, process);
    }
}

// ── CSV parser (unchanged from original) ─────────────────────────────────
Event* FileHandler::parseCSVLine(const std::string& line) const {
    std::stringstream ss(line);
    std::string id, ts, user, type, desc, extra1, extra2;

    if (!std::getline(ss, id,     ',')) throw std::runtime_error("Bad CSV: " + line);
    if (!std::getline(ss, ts,     ',')) throw std::runtime_error("Bad CSV: " + line);
    if (!std::getline(ss, user,   ',')) throw std::runtime_error("Bad CSV: " + line);
    if (!std::getline(ss, type,   ',')) throw std::runtime_error("Bad CSV: " + line);
    if (!std::getline(ss, desc,   ',')) throw std::runtime_error("Bad CSV: " + line);
    if (!std::getline(ss, extra1, ',')) throw std::runtime_error("Bad CSV: " + line);
    if (!std::getline(ss, extra2))      throw std::runtime_error("Bad CSV: " + line);

    if (type == "Login") {
        return new LoginEvent(id, ts, user, desc,
                              extra1 == "true", extra2);
    } else if (type == "Error") {
        int code = 0;
        try { code = std::stoi(extra1); } catch (...) { code = -1; }
        return new ErrorEvent(id, ts, user, desc, code, extra2);
    } else if (type == "Warning") {
        return new WarningEvent(id, ts, user, desc, extra1, extra2);
    } else if (type == "Activity") {
        return new ActivityEvent(id, ts, user, desc, extra1, extra2);
    }
    throw std::runtime_error("Unknown event type: " + type);
}

// ── loadFromFile ──────────────────────────────────────────────────────────
void FileHandler::loadFromFile(LogManager& manager) const {
    std::ifstream file(fileName);
    if (!file.is_open())
        throw std::runtime_error("Cannot open file: " + fileName);

    bool macMode = isMacLog();
    std::string line;
    int loaded = 0, skipped = 0;

    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;
        try {
            Event* e = macMode ? parseMacLogLine(line) : parseCSVLine(line);
            manager.addEvent(e);
            loaded++;
        } catch (const std::exception&) {
            skipped++;
        }
    }

    std::cout << "Loaded " << loaded << " events";
    if (skipped > 0) std::cout << " (" << skipped << " skipped)";
    std::cout << " from " << fileName << "\n";
}

// ── saveToFile — always writes CSV ───────────────────────────────────────
void FileHandler::saveToFile(const LogManager& manager) const {
    // When source was a .log file, save to a matching .csv
    std::string outFile = fileName;
    if (isMacLog()) {
        outFile = fileName.substr(0, fileName.size() - 4) + "_exported.csv";
    }

    // ── Pre-save rotation ────────────────────────────────────────────────
    // If existing file is larger than 5 MB, archive it with a timestamp
    // before overwriting.  Prevents unbounded growth across runs.
    constexpr std::uintmax_t ROTATE_THRESHOLD = 5 * 1024 * 1024;   // 5 MB
    std::error_code ec;
    if (std::filesystem::exists(outFile, ec)) {
        auto sz = std::filesystem::file_size(outFile, ec);
        if (!ec && sz > ROTATE_THRESHOLD) {
            auto t = std::chrono::system_clock::to_time_t(
                         std::chrono::system_clock::now());
            std::tm tm{};
            localtime_r(&t, &tm);
            char stamp[32];
            std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);

            std::string archive = outFile + "." + stamp + ".archive";
            std::filesystem::rename(outFile, archive, ec);
            if (!ec) {
                std::cout << "[FileHandler] Rotated old log → "
                          << archive << " (" << (sz / 1024) << " KB)\n";
            }
        }
    }

    std::ofstream file(outFile);
    if (!file.is_open())
        throw std::runtime_error("Cannot write to file: " + outFile);

    for (Event* e : manager.getLogs()) {
        std::string x1, x2;
        if (auto* le = dynamic_cast<LoginEvent*>(e))    { x1=le->getExtra1(); x2=le->getExtra2(); }
        else if (auto* ee = dynamic_cast<ErrorEvent*>(e))   { x1=ee->getExtra1(); x2=ee->getExtra2(); }
        else if (auto* we = dynamic_cast<WarningEvent*>(e)) { x1=we->getExtra1(); x2=we->getExtra2(); }
        else if (auto* ae = dynamic_cast<ActivityEvent*>(e)){ x1=ae->getExtra1(); x2=ae->getExtra2(); }

        file << e->getEventID()     << ","
             << e->getTimestamp()   << ","
             << e->getUserName()    << ","
             << e->getType()        << ","
             << e->getDescription() << ","
             << x1 << "," << x2    << "\n";
    }
    std::cout << "Saved " << manager.getCount()
              << " events to " << outFile << "\n";
}
