#ifndef REPORTGENERATOR_H
#define REPORTGENERATOR_H

#include "LogManager.h"

class ReportGenerator {
private:
    const LogManager& logRef;

public:
    explicit ReportGenerator(const LogManager& manager);

    void generateSummary() const;
    void countByType()     const;
    void displayReport()   const;
};

#endif
