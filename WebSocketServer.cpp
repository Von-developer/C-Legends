#include "WebSocketServer.h"

#include <iostream>
#include <sstream>
#include <algorithm>
#include <cstring>
#include <cerrno>
#include <array>
#include <cassert>

// POSIX
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>

// ─────────────────────────────────────────────────────────────────────────
//  SHA-1 (FIPS 180-4) — minimal inline implementation
//  We only need it for the WebSocket handshake Accept key.
// ─────────────────────────────────────────────────────────────────────────
static constexpr uint32_t rotl32(uint32_t v, int n) {
    return (v << n) | (v >> (32 - n));
}

std::array<uint8_t, 20> WebSocketServer::sha1(const std::string& input) {
    // Initial hash values
    uint32_t h[5] = {
        0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u
    };

    // Pre-processing: add padding
    std::vector<uint8_t> msg(input.begin(), input.end());
    uint64_t bitLen = static_cast<uint64_t>(input.size()) * 8;
    msg.push_back(0x80);
    while (msg.size() % 64 != 56) msg.push_back(0x00);
    for (int i = 7; i >= 0; --i)
        msg.push_back(static_cast<uint8_t>((bitLen >> (i * 8)) & 0xFF));

    // Process each 512-bit (64-byte) block
    for (size_t offset = 0; offset < msg.size(); offset += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            w[i] = (uint32_t(msg[offset + i*4    ]) << 24)
                 | (uint32_t(msg[offset + i*4 + 1]) << 16)
                 | (uint32_t(msg[offset + i*4 + 2]) <<  8)
                 | (uint32_t(msg[offset + i*4 + 3]));
        }
        for (int i = 16; i < 80; ++i)
            w[i] = rotl32(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);

        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];

        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if      (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999u; }
            else if (i < 40) { f =  b ^ c ^ d;          k = 0x6ED9EBA1u; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
            else             { f =  b ^ c ^ d;          k = 0xCA62C1D6u; }

            uint32_t tmp = rotl32(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rotl32(b, 30); b = a; a = tmp;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }

    std::array<uint8_t, 20> digest;
    for (int i = 0; i < 5; ++i) {
        digest[i*4    ] = (h[i] >> 24) & 0xFF;
        digest[i*4 + 1] = (h[i] >> 16) & 0xFF;
        digest[i*4 + 2] = (h[i] >>  8) & 0xFF;
        digest[i*4 + 3] =  h[i]        & 0xFF;
    }
    return digest;
}

// ─────────────────────────────────────────────────────────────────────────
//  Base64 encode
// ─────────────────────────────────────────────────────────────────────────
std::string WebSocketServer::base64Encode(const uint8_t* data, size_t len) {
    static const char* TABLE =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);

    for (size_t i = 0; i < len; i += 3) {
        uint32_t v  = (uint32_t(data[i]) << 16);
        if (i + 1 < len) v |= (uint32_t(data[i+1]) << 8);
        if (i + 2 < len) v |= uint32_t(data[i+2]);

        out += TABLE[(v >> 18) & 0x3F];
        out += TABLE[(v >> 12) & 0x3F];
        out += (i + 1 < len) ? TABLE[(v >> 6) & 0x3F] : '=';
        out += (i + 2 < len) ? TABLE[v & 0x3F]        : '=';
    }
    return out;
}

// ─────────────────────────────────────────────────────────────────────────
//  Sec-WebSocket-Accept derivation (RFC 6455 §1.3)
// ─────────────────────────────────────────────────────────────────────────
std::string WebSocketServer::makeAcceptKey(const std::string& clientKey) {
    static const std::string MAGIC = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    std::string combined = clientKey + MAGIC;
    auto digest = sha1(combined);
    return base64Encode(digest.data(), digest.size());
}

// ─────────────────────────────────────────────────────────────────────────
//  Constructor / Destructor
// ─────────────────────────────────────────────────────────────────────────
WebSocketServer::WebSocketServer(uint16_t port) : port_(port) {}

WebSocketServer::~WebSocketServer() {
    stop();
}

// ─────────────────────────────────────────────────────────────────────────
//  start / stop
// ─────────────────────────────────────────────────────────────────────────
void WebSocketServer::start() {
    if (running_.exchange(true)) return;

    listenFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd_ < 0) {
        std::cerr << "[WS] socket() failed: " << strerror(errno) << "\n";
        running_ = false; return;
    }

    int yes = 1;
    ::setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    ::fcntl(listenFd_, F_SETFL, O_NONBLOCK);

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port_);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (::bind(listenFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::cerr << "[WS] bind() on port " << port_
                  << " failed: " << strerror(errno) << "\n";
        ::close(listenFd_); listenFd_ = -1; running_ = false; return;
    }
    ::listen(listenFd_, 16);

    acceptThread_ = std::jthread([this](std::stop_token st){ acceptLoop(st); });
    std::cout << "[WS] WebSocket server → ws://0.0.0.0:" << port_ << "\n";
}

void WebSocketServer::stop() {
    if (!running_.exchange(false)) return;
    acceptThread_.request_stop();
    if (listenFd_ >= 0) { ::close(listenFd_); listenFd_ = -1; }
    acceptThread_ = std::jthread{};

    // Close all connected clients
    std::lock_guard<std::mutex> lk(clientsMutex_);
    for (int fd : clients_) ::close(fd);
    clients_.clear();
    std::cout << "[WS] Stopped.\n";
}

int WebSocketServer::clientCount() const {
    std::lock_guard<std::mutex> lk(clientsMutex_);
    return static_cast<int>(clients_.size());
}

// ─────────────────────────────────────────────────────────────────────────
//  Accept loop
// ─────────────────────────────────────────────────────────────────────────
void WebSocketServer::acceptLoop(std::stop_token st) {
    while (!st.stop_requested()) {
        // Use poll() so we can wake on stop_request
        pollfd pfd{ listenFd_, POLLIN, 0 };
        int rc = ::poll(&pfd, 1, 200); // 200 ms timeout
        if (rc <= 0) continue;

        sockaddr_in clientAddr{};
        socklen_t addrLen = sizeof(clientAddr);
        int clientFd = ::accept(listenFd_,
                                reinterpret_cast<sockaddr*>(&clientAddr),
                                &addrLen);
        if (clientFd < 0) continue;

        // Set socket to blocking mode for the client handler
        int flags = ::fcntl(clientFd, F_GETFL, 0);
        ::fcntl(clientFd, F_SETFL, flags & ~O_NONBLOCK);

        // Spin up a dedicated jthread per connection
        std::jthread([this, clientFd]() mutable {
            clientLoop(clientFd);
        }).detach();
    }
}

// ─────────────────────────────────────────────────────────────────────────
//  Per-client handler
// ─────────────────────────────────────────────────────────────────────────
void WebSocketServer::clientLoop(int fd) {
    // ── Read HTTP Upgrade request ─────────────────────────────────────────
    std::string reqBuf;
    reqBuf.reserve(1024);
    char tmp[512];
    while (true) {
        ssize_t n = ::recv(fd, tmp, sizeof(tmp) - 1, 0);
        if (n <= 0) { ::close(fd); return; }
        tmp[n] = '\0';
        reqBuf += tmp;
        if (reqBuf.find("\r\n\r\n") != std::string::npos) break;
        if (reqBuf.size() > 8192) { ::close(fd); return; }
    }

    if (!performHandshake(fd, reqBuf)) { ::close(fd); return; }

    // Register client
    {
        std::lock_guard<std::mutex> lk(clientsMutex_);
        clients_.push_back(fd);
    }

    // ── Frame read loop ───────────────────────────────────────────────────
    while (true) {
        // Read 2-byte frame header
        uint8_t header[2];
        ssize_t n = ::recv(fd, header, 2, MSG_WAITALL);
        if (n != 2) break;

        bool fin     = (header[0] & 0x80) != 0;
        uint8_t opcode = header[0] & 0x0F;
        bool masked  = (header[1] & 0x80) != 0;
        uint64_t payLen = header[1] & 0x7F;

        if (payLen == 126) {
            uint8_t ext[2];
            if (::recv(fd, ext, 2, MSG_WAITALL) != 2) break;
            payLen = (uint64_t(ext[0]) << 8) | ext[1];
        } else if (payLen == 127) {
            uint8_t ext[8];
            if (::recv(fd, ext, 8, MSG_WAITALL) != 8) break;
            payLen = 0;
            for (int i = 0; i < 8; ++i)
                payLen = (payLen << 8) | ext[i];
        }

        // Limit: 128 KB per frame
        if (payLen > 131072) break;

        uint8_t mask[4] = {};
        if (masked) {
            if (::recv(fd, mask, 4, MSG_WAITALL) != 4) break;
        }

        std::vector<uint8_t> payload(payLen);
        if (payLen > 0) {
            ssize_t got = ::recv(fd, payload.data(),
                                 static_cast<size_t>(payLen), MSG_WAITALL);
            if (got != static_cast<ssize_t>(payLen)) break;
            if (masked) {
                for (size_t i = 0; i < payLen; ++i)
                    payload[i] ^= mask[i % 4];
            }
        }
        (void)fin; // single-frame messages only for dashboard

        if (opcode == 0x8) {
            // CLOSE — echo back a close frame then disconnect
            uint8_t closeFrame[2] = { 0x88, 0x00 };
            ::send(fd, closeFrame, 2, MSG_NOSIGNAL);
            break;
        } else if (opcode == 0x9) {
            // PING — reply with PONG
            std::string pingData(payload.begin(), payload.end());
            auto pong = makePongFrame(pingData);
            sendAll(fd, pong.data(), pong.size());
        }
        // TEXT/BINARY frames from client are ignored (dashboard is read-only)
    }

    removeClient(fd);
    ::close(fd);
}

// ─────────────────────────────────────────────────────────────────────────
//  WebSocket handshake
// ─────────────────────────────────────────────────────────────────────────
bool WebSocketServer::performHandshake(int fd, const std::string& req) {
    // Extract Sec-WebSocket-Key header
    const std::string keyHeader = "Sec-WebSocket-Key:";
    auto pos = req.find(keyHeader);
    if (pos == std::string::npos) {
        // Maybe it's a plain HTTP request for the /health endpoint
        if (req.find("GET /health") == 0 || req.find("GET /ws/health") != std::string::npos) {
            const std::string resp =
                "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 2\r\n\r\nOK";
            ::send(fd, resp.c_str(), resp.size(), MSG_NOSIGNAL);
        }
        return false;
    }

    size_t start = pos + keyHeader.size();
    while (start < req.size() && req[start] == ' ') ++start;
    size_t end = req.find("\r\n", start);
    if (end == std::string::npos) return false;

    std::string clientKey = req.substr(start, end - start);
    // Trim trailing whitespace
    while (!clientKey.empty() && (clientKey.back() == ' ' || clientKey.back() == '\r'))
        clientKey.pop_back();

    std::string acceptKey = makeAcceptKey(clientKey);

    std::string response =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: " + acceptKey + "\r\n"
        "\r\n";

    return ::send(fd, response.c_str(), response.size(), MSG_NOSIGNAL)
           == static_cast<ssize_t>(response.size());
}

// ─────────────────────────────────────────────────────────────────────────
//  Frame builders
// ─────────────────────────────────────────────────────────────────────────
std::vector<uint8_t> WebSocketServer::makeTextFrame(std::string_view payload) {
    std::vector<uint8_t> frame;
    frame.push_back(0x81); // FIN + TEXT opcode

    size_t len = payload.size();
    if (len < 126) {
        frame.push_back(static_cast<uint8_t>(len));
    } else if (len < 65536) {
        frame.push_back(126);
        frame.push_back((len >> 8) & 0xFF);
        frame.push_back(len & 0xFF);
    } else {
        frame.push_back(127);
        for (int i = 7; i >= 0; --i)
            frame.push_back((len >> (i * 8)) & 0xFF);
    }
    frame.insert(frame.end(), payload.begin(), payload.end());
    return frame;
}

std::vector<uint8_t> WebSocketServer::makePongFrame(const std::string& payload) {
    std::vector<uint8_t> frame;
    frame.push_back(0x8A); // FIN + PONG
    frame.push_back(static_cast<uint8_t>(std::min(payload.size(), size_t(125))));
    frame.insert(frame.end(), payload.begin(), payload.end());
    return frame;
}

bool WebSocketServer::sendAll(int fd, const uint8_t* buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = ::send(fd, buf + sent, len - sent, MSG_NOSIGNAL);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────
//  broadcast — sends JSON to every connected client
// ─────────────────────────────────────────────────────────────────────────
void WebSocketServer::broadcast(std::string_view json) {
    auto frame = makeTextFrame(json);

    std::lock_guard<std::mutex> lk(clientsMutex_);
    std::vector<int> dead;
    for (int fd : clients_) {
        if (!sendAll(fd, frame.data(), frame.size()))
            dead.push_back(fd);
    }
    for (int fd : dead) {
        clients_.erase(std::remove(clients_.begin(), clients_.end(), fd), clients_.end());
        ::close(fd);
    }
}

// ─────────────────────────────────────────────────────────────────────────
//  removeClient
// ─────────────────────────────────────────────────────────────────────────
void WebSocketServer::removeClient(int fd) {
    std::lock_guard<std::mutex> lk(clientsMutex_);
    clients_.erase(std::remove(clients_.begin(), clients_.end(), fd), clients_.end());
}
