#pragma once
#ifndef WEBSOCKETSERVER_H
#define WEBSOCKETSERVER_H

/**
 * WebSocketServer  (RFC 6455, C++20, POSIX only)
 *
 * A minimal but complete WebSocket server designed to push live log events
 * from LogManager to all connected browser dashboards.
 *
 * Architecture
 * ────────────
 *  • One accept-thread listens for new TCP connections.
 *  • Each accepted connection gets its own std::jthread (client handler).
 *  • A shared client registry (mutex-protected vector of fds) lets the
 *    broadcast() method fan-out a JSON frame to every live connection.
 *  • LogManager calls broadcast() from its consumerLoop after each event
 *    is committed — zero extra latency, no polling.
 *
 * WebSocket handshake
 * ───────────────────
 *  The server performs the HTTP Upgrade handshake (Sec-WebSocket-Accept key
 *  derivation via SHA-1 + Base64 — implemented inline, no OpenSSL needed).
 *
 * Frame encoding
 * ──────────────
 *  Only TEXT frames (opcode 0x1) are sent.  Incoming PING frames are answered
 *  with PONG.  CLOSE frames initiate a clean shutdown of that connection.
 *  Client messages (browser → server) are ignored (dashboard is read-only).
 *
 * Thread safety
 * ─────────────
 *  broadcast() acquires clientsMutex and iterates a snapshot, so it is safe
 *  to call from any thread (LogManager consumer, LiveFileWatcher, etc.).
 */

#include <string>
#include <string_view>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <functional>
#include <cstdint>

class WebSocketServer {
public:
    explicit WebSocketServer(uint16_t port = 9090);
    ~WebSocketServer();

    // Non-copyable
    WebSocketServer(const WebSocketServer&)            = delete;
    WebSocketServer& operator=(const WebSocketServer&) = delete;

    void start();
    void stop();
    bool isRunning() const { return running_.load(); }
    uint16_t port()  const { return port_; }

    // Broadcast a UTF-8 JSON string to every connected client.
    // Thread-safe — may be called from LogManager's consumer thread.
    void broadcast(std::string_view json);

    // Connected client count (approximate — may include connections mid-close)
    int clientCount() const;

private:
    uint16_t          port_;
    std::atomic<bool> running_{false};
    std::jthread      acceptThread_;
    int               listenFd_{-1};

    // ── Client registry ───────────────────────────────────────────────────
    mutable std::mutex       clientsMutex_;
    std::vector<int>         clients_;          // connected socket fds

    // ── Accept loop ───────────────────────────────────────────────────────
    void acceptLoop(std::stop_token st);

    // ── Per-client handler (runs on its own jthread) ──────────────────────
    void clientLoop(int fd);

    // ── WebSocket handshake helpers ───────────────────────────────────────
    // Returns true and sends HTTP 101 if the upgrade is valid.
    bool performHandshake(int fd, const std::string& requestBuf);

    // Derive Sec-WebSocket-Accept value from client key.
    static std::string makeAcceptKey(const std::string& clientKey);

    // ── Frame helpers ─────────────────────────────────────────────────────
    // Build a WebSocket TEXT frame (RFC 6455 §5.2).
    static std::vector<uint8_t> makeTextFrame(std::string_view payload);

    // Build a PONG frame from an incoming PING payload.
    static std::vector<uint8_t> makePongFrame(const std::string& payload);

    // Send all bytes in buf to fd (handles short writes).
    static bool sendAll(int fd, const uint8_t* buf, size_t len);

    // ── SHA-1 / Base64 (inline, no OpenSSL) ──────────────────────────────
    static std::array<uint8_t, 20> sha1(const std::string& input);
    static std::string             base64Encode(const uint8_t* data, size_t len);

    // Remove a client fd from the registry (called on disconnect).
    void removeClient(int fd);
};

#endif // WEBSOCKETSERVER_H
