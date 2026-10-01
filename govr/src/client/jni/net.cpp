#include "net.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "common.h"
#include "protocol.h"

void NetClient::Start(uint16_t port, MessageHandler onMessage, EventHandler onConnection) {
    port_ = port;
    onMessage_ = std::move(onMessage);
    onConnection_ = std::move(onConnection);
    running_ = true;
    thread_ = std::thread(&NetClient::Loop, this);
}

void NetClient::Stop() {
    running_ = false;
    int fd = fd_.exchange(-1);
    if (fd >= 0) {
        shutdown(fd, SHUT_RDWR);
        close(fd);
    }
    if (thread_.joinable()) thread_.join();
}

bool NetClient::ReadExact(int fd, void* dst, size_t n) {
    uint8_t* p = (uint8_t*)dst;
    while (n > 0) {
        ssize_t r = recv(fd, p, n, 0);
        if (r <= 0) {
            if (r < 0 && errno == EINTR) continue;
            return false;
        }
        p += r;
        n -= r;
        bytesIn += r;
    }
    return true;
}

bool NetClient::Send(uint8_t type, const void* payload, uint32_t len) {
    std::lock_guard<std::mutex> lock(sendMutex_);
    const int fd = fd_;
    if (fd < 0) return false;
    proto::Header h = {type, {0, 0, 0}, len};
    uint8_t buf[256];
    if (len + sizeof(h) <= sizeof(buf)) {  // small messages: one syscall
        memcpy(buf, &h, sizeof(h));
        if (len) memcpy(buf + sizeof(h), payload, len);
        return send(fd, buf, sizeof(h) + len, MSG_NOSIGNAL) == (ssize_t)(sizeof(h) + len);
    }
    if (send(fd, &h, sizeof(h), MSG_NOSIGNAL) != sizeof(h)) return false;
    return send(fd, payload, len, MSG_NOSIGNAL) == (ssize_t)len;
}

void NetClient::Loop() {
    bool loggedWaiting = false;
    useconds_t backoffUs = 300000;
    while (running_) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port_);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (connect(fd, (sockaddr*)&addr, sizeof(addr)) != 0) {
            close(fd);
            if (!loggedWaiting) {
                LOGI("net: waiting for PC server on 127.0.0.1:%u (adb reverse)", port_);
                loggedWaiting = true;
            }
            usleep(500000);
            continue;
        }
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        int rcvbuf = 4 << 20;
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
        fd_ = fd;
        // adbd accepts on behalf of `adb reverse` even when no PC server listens, then
        // closes: only a received message proves the server is there.
        if (onConnection_) onConnection_(true);  // lets the app send its hello
        bool announced = false;
        proto::Header h;
        while (running_ && ReadExact(fd, &h, sizeof(h))) {
            if (h.length > (64u << 20)) {
                LOGE("net: bogus message length %u", h.length);
                break;
            }
            buffer_.resize(h.length);
            if (h.length && !ReadExact(fd, buffer_.data(), h.length)) break;
            lastMessageNs = NowNanos();
            if (!announced) {
                LOGI("net: connected to PC server");
                announced = true;
                loggedWaiting = false;
                backoffUs = 300000;
            }
            onMessage_(h.type, buffer_.data(), h.length);
        }
        {
            std::lock_guard<std::mutex> lock(sendMutex_);
            if (fd_.exchange(-1) >= 0) close(fd);
        }
        if (announced) LOGI("net: disconnected");
        if (onConnection_) onConnection_(false);
        if (!announced && !loggedWaiting) {
            LOGI("net: waiting for PC server on 127.0.0.1:%u (adb reverse)", port_);
            loggedWaiting = true;
        }
        if (!announced && backoffUs < 2000000) backoffUs *= 2;
        usleep(announced ? 300000 : backoffUs);
    }
}

