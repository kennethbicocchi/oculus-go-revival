// TCP link to the PC server (through `adb reverse`), with automatic reconnection.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

class NetClient {
public:
    // Called on the network thread for every received message.
    using MessageHandler = std::function<void(uint8_t type, const uint8_t* data, uint32_t len)>;
    using EventHandler = std::function<void(bool connected)>;

    void Start(uint16_t port, MessageHandler onMessage, EventHandler onConnection);
    void Stop();
    // Thread-safe. Returns false if not connected.
    bool Send(uint8_t type, const void* payload, uint32_t len);
    bool Connected() const { return fd_ >= 0; }

    std::atomic<uint64_t> bytesIn{0};
    int64_t lastMessageNs = 0;  // when the current message was fully received (net thread)

private:
    void Loop();
    bool ReadExact(int fd, void* dst, size_t n);

    uint16_t port_ = 0;
    std::atomic<int> fd_{-1};
    std::atomic<bool> running_{false};
    std::thread thread_;
    std::mutex sendMutex_;
    MessageHandler onMessage_;
    EventHandler onConnection_;
    std::vector<uint8_t> buffer_;
};
