#include "PrometheusExporter.h"

#include <iostream>
#include <sstream>
#include <cstring>
#include <cerrno>

/* POSIX socket headers */
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>

// ── Constructor / Destructor ──────────────────────────────────────────────
PrometheusExporter::PrometheusExporter(MetricsCallback cb, uint16_t port)
    : cb_(std::move(cb)), port_(port) {}

PrometheusExporter::~PrometheusExporter() {
    stop();
}

// ── start ─────────────────────────────────────────────────────────────────
void PrometheusExporter::start() {
    if (running_.exchange(true)) return;

    // ── Create TCP listening socket ────────────────────────────────────────
    listenFd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd_ < 0) {
        std::cerr << "[Prometheus] socket() failed: " << strerror(errno) << "\n";
        running_ = false;
        return;
    }

    // SO_REUSEADDR so rapid restarts don't hit TIME_WAIT
    int yes = 1;
    setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    // Set non-blocking so stop() can interrupt accept()
    fcntl(listenFd_, F_SETFL, O_NONBLOCK);

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port_);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(listenFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::cerr << "[Prometheus] bind() on port " << port_
                  << " failed: " << strerror(errno) << "\n";
        close(listenFd_);
        listenFd_ = -1;
        running_  = false;
        return;
    }

    listen(listenFd_, 8);
    std::cout << "[Prometheus] Scrape endpoint → http://0.0.0.0:"
              << port_ << "/metrics\n";

    serverThread_ = std::jthread([this](std::stop_token st){
        acceptLoop(st);
    });
}

// ── stop ──────────────────────────────────────────────────────────────────
void PrometheusExporter::stop() {
    if (!running_.exchange(false)) return;
    serverThread_.request_stop();
    if (listenFd_ >= 0) { close(listenFd_); listenFd_ = -1; }
    serverThread_ = std::jthread{};
    std::cout << "[Prometheus] Exporter stopped.\n";
}

// ── acceptLoop ────────────────────────────────────────────────────────────
void PrometheusExporter::acceptLoop(std::stop_token st) {
    while (!st.stop_requested()) {
        sockaddr_in clientAddr{};
        socklen_t   addrLen = sizeof(clientAddr);

        int clientFd = accept(listenFd_,
                              reinterpret_cast<sockaddr*>(&clientAddr),
                              &addrLen);
        if (clientFd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // No connection ready — poll briefly
                struct timeval tv{ 0, 100'000 };   /* 100 ms */
                fd_set rfds;
                FD_ZERO(&rfds);
                if (listenFd_ >= 0) FD_SET(listenFd_, &rfds);
                select(listenFd_ + 1, &rfds, nullptr, nullptr, &tv);
                continue;
            }
            if (!st.stop_requested())
                std::cerr << "[Prometheus] accept() error: " << strerror(errno) << "\n";
            break;
        }
        handleClient(clientFd);
    }
}

// ── handleClient ──────────────────────────────────────────────────────────
// Reads the HTTP request (enough to confirm it's GET /metrics), then sends
// the Prometheus exposition format body.
void PrometheusExporter::handleClient(int clientFd) {
    // Set a short receive timeout so a slow client can't stall the thread
    struct timeval tv{ 2, 0 };
    setsockopt(clientFd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    // Read request (we only need the first line)
    char buf[1024];
    ssize_t n = recv(clientFd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) { close(clientFd); return; }
    buf[n] = '\0';

    // Only serve GET /metrics — return 404 for anything else
    bool isMetrics = (strncmp(buf, "GET /metrics", 12) == 0 ||
                      strncmp(buf, "GET / ",       6)  == 0);

    std::string body;
    std::string status;

    if (isMetrics) {
        body   = cb_();           // call LogManager::buildPrometheusMetrics()
        status = "200 OK";
    } else {
        body   = "# C-Legends Prometheus endpoint\n# Use GET /metrics\n";
        status = "404 Not Found";
    }

    std::ostringstream resp;
    resp << "HTTP/1.1 " << status << "\r\n"
         << "Content-Type: text/plain; version=0.0.4; charset=utf-8\r\n"
         << "Content-Length: " << body.size() << "\r\n"
         << "Connection: close\r\n"
         << "\r\n"
         << body;

    std::string respStr = resp.str();
    send(clientFd, respStr.data(), respStr.size(), MSG_NOSIGNAL);
    close(clientFd);
}
