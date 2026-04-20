#ifndef FILEHANDLER_H
#define FILEHANDLER_H

#include "LogManager.h"
#include <string>

class FileHandler {
private:
    std::string fileName;

    // ── Parsers ──────────────────────────────────────────────────────────
    // CSV  format: EventID,Timestamp,User,Type,Desc,Extra1,Extra2
    Event* parseCSVLine(const std::string& line) const;

    // macOS syslog format: "Mon DD HH:MM:SS hostname process[pid]: message"
    Event* parseMacLogLine(const std::string& line) const;

    // Keyword classifier — decides event type from message text
    static std::string classifyMacLine(const std::string& process,
                                       const std::string& message);

    // True if fileName ends with .log
    bool isMacLog() const;

public:
    explicit FileHandler(const std::string& file = "logs.csv");

    // Throws std::runtime_error if file cannot be opened
    void loadFromFile(LogManager& manager) const;

    // Always saves in CSV format (Mac.log is read-only source)
    void saveToFile(const LogManager& manager) const;
};

#endif
