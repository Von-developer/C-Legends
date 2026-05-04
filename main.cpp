#include <iostream>
#include <string>
#include <stdexcept>
#include <limits>
#include <chrono>
#include <thread>
#include <csignal>
#include <condition_variable>
#include <mutex>
#include <atomic>

#include "LogManager.h"
#include "FileHandler.h"
#include "ReportGenerator.h"
#include "LiveFileWatcher.h"
#include "PrometheusExporter.h"
#include "WebDashboardServer.h"
#include "GeoLocator.h"
#include "DShieldInformer.h"
#include "LoginEvent.h"
#include "ErrorEvent.h"
#include "WarningEvent.h"
#include "ActivityEvent.h"

// ── Signal handling for daemon mode ──────────────────────────────────────
static std::mutex              g_shutdownMtx;
static std::condition_variable g_shutdownCV;
static std::atomic<bool>       g_shutdown{false};

static void signalHandler(int) {
    g_shutdown = true;
    g_shutdownCV.notify_all();
}

// ── Daemon mode ───────────────────────────────────────────────────────────
// Called when argv contains "--daemon".
// Loads the log file, starts the Prometheus endpoint and live file watcher,
// then blocks until SIGTERM or SIGINT — no interactive menu, no stdin reads.
static int runDaemon(const std::string& logFile) {
    std::signal(SIGTERM, signalHandler);
    std::signal(SIGINT,  signalHandler);

    std::cout << "[daemon] C-Legends starting\n"
              << "[daemon] Log file : " << logFile << "\n"
              << "[daemon] Metrics  : http://0.0.0.0:9091/metrics\n";

    LogManager      manager;
    FileHandler     fileHandler(logFile);
    GeoLocator      geoLocator;                   // shared with DShieldInformer
    DShieldInformer dshield(manager, geoLocator);
    PrometheusExporter prometheus(
        [&manager]{ return manager.buildPrometheusMetrics(); },
        9091
    );

    // Load existing log data
    try {
        fileHandler.loadFromFile(manager);
    } catch (const std::runtime_error& ex) {
        std::cerr << "[daemon] Note: " << ex.what() << " — starting with empty log.\n";
    }

    // Start Prometheus scrape endpoint
    prometheus.start();

    // Start live file watcher (polls for new lines every 500 ms)
    LiveFileWatcher watcher(logFile, manager, std::chrono::milliseconds(500));
    watcher.start();

    // Start DShield live threat feed (poll every 5 minutes)
    dshield.startLiveFeed(300);

    std::cout << "[daemon] Ready — serving " << manager.getCount()
              << " events. Waiting for SIGTERM...\n";
    std::cout.flush();

    // Block until signal
    std::unique_lock<std::mutex> lk(g_shutdownMtx);
    g_shutdownCV.wait(lk, []{ return g_shutdown.load(); });

    std::cout << "[daemon] Shutting down cleanly...\n";
    dshield.stop();           // must be before manager.stopProcessing()
    watcher.stop();
    prometheus.stop();
    return 0;
}

// ── Web-serve mode ────────────────────────────────────────────────────────
// Usage: ./log_analyzer --serve [logfile] [--http-port 8080] [--ws-port 9090]
//                                         [--admin-pass secret]
//
// Starts the full web dashboard + WebSocket live stream + LiveFileWatcher.
// Navigate to http://localhost:8080 to open the dashboard.
static int runServe(int argc, char* argv[]) {
    std::signal(SIGTERM, signalHandler);
    std::signal(SIGINT,  signalHandler);

    // Parse optional arguments
    std::string logFile    = "Mac.log";
    uint16_t    httpPort   = 8080;
    uint16_t    wsPort     = 9090;
    std::string adminPass  = "clegends-admin";
    std::string logDir     = ".";

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--serve") {
            if (i + 1 < argc && argv[i+1][0] != '-') logFile = argv[++i];
        } else if (arg == "--http-port" && i + 1 < argc) {
            httpPort = static_cast<uint16_t>(std::stoi(argv[++i]));
        } else if (arg == "--ws-port" && i + 1 < argc) {
            wsPort = static_cast<uint16_t>(std::stoi(argv[++i]));
        } else if (arg == "--admin-pass" && i + 1 < argc) {
            adminPass = argv[++i];
        } else if (arg == "--log-dir" && i + 1 < argc) {
            logDir = argv[++i];
        }
    }

    std::cout << "[serve] C-Legends Web Dashboard\n"
              << "[serve] Log file   : " << logFile   << "\n"
              << "[serve] Dashboard  : http://0.0.0.0:" << httpPort << "\n"
              << "[serve] WebSocket  : ws://0.0.0.0:"   << wsPort   << "\n";

    LogManager  manager;
    FileHandler fileHandler(logFile);
    GeoLocator  geoLocator;                      // shared with DShieldInformer

    // Wire WS push before loading (so batch-loaded events don't flood WS)
    // We'll connect it after the dashboard server is constructed below.

    // Load existing log data
    try {
        fileHandler.loadFromFile(manager);
    } catch (const std::runtime_error& ex) {
        std::cerr << "[serve] Note: " << ex.what() << " — starting with empty log.\n";
    }

    // Build and start the web dashboard server
    WebDashboardServer::Config cfg;
    cfg.httpPort      = httpPort;
    cfg.wsPort        = wsPort;
    cfg.dashboardDir  = "dashboard";
    cfg.adminPassword = adminPass;
    cfg.logDir        = logDir;

    WebDashboardServer dashServer(manager, fileHandler, cfg);

    // Wire LogManager → WebSocket push
    manager.onNewEvent = [&dashServer](const Event* e) {
        dashServer.pushEvent(e);
    };

    dashServer.start();

    // Start live file watcher (polls for new lines → pushEvent → WS broadcast)
    LiveFileWatcher watcher(logFile, manager, std::chrono::milliseconds(500));
    watcher.start();

    // Start DShield live threat feed → pushEvent → WS broadcast
    DShieldInformer dshield(manager, geoLocator);
    dshield.startLiveFeed(300);

    std::cout << "[serve] Ready — " << manager.getCount()
              << " events loaded. Open http://localhost:" << httpPort
              << " in your browser.\n"
              << "[serve] DShield live threats streaming (every 5 min).\n"
              << "[serve] Press Ctrl+C to stop.\n";
    std::cout.flush();

    // Block until SIGTERM/SIGINT
    std::unique_lock<std::mutex> lk(g_shutdownMtx);
    g_shutdownCV.wait(lk, []{ return g_shutdown.load(); });

    std::cout << "[serve] Shutting down...\n";
    dshield.stop();           // must be before watcher/manager teardown
    watcher.stop();
    dashServer.stop();
    return 0;
}

// ── Helper: get a string from the user ───────────────────────────────────
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

// ── Live watch session (interactive) ─────────────────────────────────────
static void startLiveWatch(LogManager& manager, const std::string& filePath) {
    std::cout << "\n[Live Watch] Starting on: " << filePath << "\n";
    std::cout << "[Live Watch] Press ENTER at any time to stop...\n\n";

    LiveFileWatcher watcher(filePath, manager, std::chrono::milliseconds(500));
    watcher.start();

    std::string dummy;
    std::getline(std::cin, dummy);

    watcher.stop();
    std::cout << "[Live Watch] Session ended.\n";
}

// ── Menu display ──────────────────────────────────────────────────────────
static void showMenu(bool prometheusRunning) {
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
    std::cout << "10. Start live file watch (real-time ingestion)\n";
    std::cout << "11. Export geo data (GeoJSON + CSV)\n";
    std::cout << "12. " << (prometheusRunning
                            ? "Stop  Prometheus /metrics endpoint"
                            : "Start Prometheus /metrics endpoint (port 9091)") << "\n";
    std::cout << "13. Fetch DShield threats now (one-shot)\n";
    std::cout << "14. Start/Stop DShield live threat feed (every 5 min)\n";
    std::cout << " 0. Exit\n";
    std::cout << "========================================\n";
    std::cout << "Choice: ";
}

// ── main ──────────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    // ── Mode dispatch based on first flag ────────────────────────────────
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--daemon") {
            std::string logFile = (i + 1 < argc) ? argv[i + 1] : "Mac.log";
            return runDaemon(logFile);
        }
        if (arg == "--serve") {
            return runServe(argc, argv);
        }
    }

    // ── Interactive mode (normal terminal use) ────────────────────────────
    std::string inputFile = (argc > 1) ? argv[1] : "logs.csv";
    LogManager       manager;
    FileHandler      fileHandler(inputFile);
    ReportGenerator  reporter(manager);
    GeoLocator       geoLocator;
    DShieldInformer  dshield(manager, geoLocator);

    PrometheusExporter prometheus(
        [&manager]{ return manager.buildPrometheusMetrics(); },
        9091
    );

    try {
        fileHandler.loadFromFile(manager);
    } catch (const std::runtime_error& ex) {
        std::cerr << "Note: " << ex.what() << " — starting with empty log.\n";
    }

    int choice = -1;
    while (choice != 0) {
        showMenu(prometheus.isRunning());

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
                case 1:  manager.displayAll();           break;
                case 2:  addEventWizard(manager);        break;
                case 3: {
                    std::string id = prompt("Enter Event ID to remove: ");
                    manager.removeEvent(id);
                    std::cout << "Event " << id << " removed.\n";
                    break;
                }
                case 4: {
                    std::string type = prompt("Type (Login/Error/Warning/Activity): ");
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
                case 7:  reporter.displayReport();       break;
                case 8:  fileHandler.saveToFile(manager); break;
                case 9:  fileHandler.loadFromFile(manager); break;
                case 10: {
                    std::string watchFile = prompt(
                        "File to watch (Enter for current [" + inputFile + "]): ");
                    if (watchFile.empty()) watchFile = inputFile;
                    startLiveWatch(manager, watchFile);
                    break;
                }
                case 11: {
                    manager.exportGeoJSON("geo_export.json");
                    manager.exportGeoCSV("geo_export.csv");
                    std::cout << "Files written: geo_export.json, geo_export.csv\n";
                    break;
                }
                case 12: {
                    if (prometheus.isRunning()) {
                        prometheus.stop();
                    } else {
                        prometheus.start();
                        std::cout << "Grafana → Add Prometheus datasource → "
                                  << "http://localhost:9091\n";
                    }
                    break;
                }
                case 13: {
                    std::cout << "Fetching DShield threats (this may take a few seconds)...\n";
                    int added = dshield.fetchOnce();
                    std::cout << "DShield fetch complete: " << added
                              << " new event(s) added.\n";
                    break;
                }
                case 14: {
                    if (dshield.isRunning()) {
                        dshield.stop();
                    } else {
                        dshield.startLiveFeed(300);
                    }
                    break;
                }
                case 0:
                    dshield.stop();
                    prometheus.stop();
                    fileHandler.saveToFile(manager);
                    std::cout << "Goodbye.\n";
                    break;
                default:
                    throw std::invalid_argument("Choice must be 0-14.");
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
