#include "ReportGenerator.h"
#include <iostream>
#include <map>

ReportGenerator::ReportGenerator(const LogManager& manager) : logRef(manager) {}

void ReportGenerator::countByType() const {
    std::map<std::string, int> counts;
    for (Event* e : logRef.getLogs())
        counts[e->getType()]++;

    std::cout << "\n  Event type breakdown:\n";
    for (auto& [type, count] : counts)
        std::cout << "    " << type << " : " << count << "\n";
}

void ReportGenerator::generateSummary() const {
    std::cout << "\n========================================\n";
    std::cout << "         SYSTEM LOG REPORT\n";
    std::cout << "========================================\n";
    std::cout << "  Total events : " << logRef.getCount() << "\n";
    countByType();
    std::cout << "========================================\n";
}

void ReportGenerator::displayReport() const {
    generateSummary();
}
