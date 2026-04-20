#include <iostream>
#include <string>
#include <stdexcept>
#include <limits>

#include "LogManager.h"
#include "FileHandler.h"
#include "ReportGenerator.h"
#include "LoginEvent.h"
#include "ErrorEvent.h"
#include "WarningEvent.h"
#include "ActivityEvent.h"

// ── Helper: get a non-empty string from the user ──────────────────────────
static std::string prompt(const std::string& label) {
    std::string val;
    std::cout << label;
    std::getline(std::cin, val);
    return val;
}

// ── Add-event wizard ──────────────────────────────────────────────────────
static void addEventWizard(LogManager& manager) {
    std::cout << "\nEvent type:\n"
              << "  1. Login\n  2. Error\n  3. Warning\n  4. Activity\n"
              << "Choice: ";
    int choice = 0;
    std::cin >> choice;
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');

    std::string id   = manager.generateID();
    std::string ts   = prompt("Timestamp (YYYY-MM-DD HH:MM): ");
    std::string user = prompt("Username                     : ");
    std::string desc = prompt("Description                  : ");

    try {
        if (choice == 1) {
            std::string suc = prompt("Login successful? (true/false): ");
            std::string lvl = prompt("Access level                  : ");
            manager.addEvent(new LoginEvent(id, ts, user, desc, suc == "true", lvl));
        } else if (choice == 2) {
            std::string codeStr = prompt("Error code (number): ");
            std::string mod     = prompt("Source module      : ");
            int code = 0;
            try { code = std::stoi(codeStr); }
            catch (...) { throw std::invalid_argument("Error code must be a number."); }
            manager.addEvent(new ErrorEvent(id, ts, user, desc, code, mod));
        } else if (choice == 3) {
            std::string sev    = prompt("Severity (LOW/MEDIUM/HIGH): ");
            std::string action = prompt("Recommended action         : ");
            manager.addEvent(new WarningEvent(id, ts, user, desc, sev, action));
        } else if (choice == 4) {
            std::string act = prompt("Action type       : ");
            std::string res = prompt("Affected resource : ");
            manager.addEvent(new ActivityEvent(id, ts, user, desc, act, res));
        } else {
            throw std::invalid_argument("Invalid event type choice.");
        }
        std::cout << "Event " << id << " added.\n";
    } catch (const std::invalid_argument& ex) {
        std::cerr << "Input error: " << ex.what() << "\n";
    }
}

// ── Menu display ──────────────────────────────────────────────────────────
static void showMenu() {
    std::cout << "\n========================================\n";
    std::cout << "    System Log Analyzer — Main Menu\n";
    std::cout << "========================================\n";
    std::cout << " 1. View all log events\n";
    std::cout << " 2. Add a new log event\n";
    std::cout << " 3. Remove a log event by ID\n";
    std::cout << " 4. Search by event type\n";
    std::cout << " 5. Search by username\n";
    std::cout << " 6. Filter warnings by severity\n";
    std::cout << " 7. Generate summary report\n";
    std::cout << " 8. Save logs to file\n";
    std::cout << " 9. Load logs from file\n";
    std::cout << " 0. Exit\n";
    std::cout << "========================================\n";
    std::cout << "Choice: ";
}

// ── main ──────────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    std::string inputFile = (argc > 1) ? argv[1] : "logs.csv";
    LogManager      manager;
    FileHandler     fileHandler(inputFile);
    ReportGenerator reporter(manager);

    // Load saved data on startup
    try {
        fileHandler.loadFromFile(manager);
    } catch (const std::runtime_error& ex) {
        std::cerr << "Note: " << ex.what() << " — starting with empty log.\n";
    }

    int choice = -1;
    while (choice != 0) {
        showMenu();

        // Exception handling for bad menu input
        try {
            if (!(std::cin >> choice))
                throw std::invalid_argument("Please enter a number.");
            std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        } catch (const std::invalid_argument& ex) {
            std::cerr << "Invalid input: " << ex.what() << "\n";
            std::cin.clear();
            std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
            continue;
        }

        try {
            switch (choice) {
                case 1:
                    manager.displayAll();
                    break;
                case 2:
                    addEventWizard(manager);
                    break;
                case 3: {
                    std::string id = prompt("Enter Event ID to remove: ");
                    manager.removeEvent(id);   // throws out_of_range if not found
                    std::cout << "Event " << id << " removed.\n";
                    break;
                }
                case 4: {
                    std::string type = prompt("Type to search (Login/Error/Warning/Activity): ");
                    manager.searchByType(type);
                    break;
                }
                case 5: {
                    std::string user = prompt("Username to search: ");
                    manager.searchByUser(user);
                    break;
                }
                case 6: {
                    std::string sev = prompt("Severity (LOW/MEDIUM/HIGH): ");
                    manager.filterWarnBySeverity(sev);
                    break;
                }
                case 7:
                    reporter.displayReport();
                    break;
                case 8:
                    fileHandler.saveToFile(manager);
                    break;
                case 9:
                    fileHandler.loadFromFile(manager);
                    break;
                case 0:
                    fileHandler.saveToFile(manager);
                    std::cout << "Goodbye.\n";
                    break;
                default:
                    throw std::invalid_argument("Choice must be 0-9.");
            }
        } catch (const std::out_of_range& ex) {
            std::cerr << "Not found: " << ex.what() << "\n";
        } catch (const std::runtime_error& ex) {
            std::cerr << "File error: " << ex.what() << "\n";
        } catch (const std::invalid_argument& ex) {
            std::cerr << "Invalid input: " << ex.what() << "\n";
        }
    }

    return 0;
}
