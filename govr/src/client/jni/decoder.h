// Hardware video decoder (NDK AMediaCodec, API 21+) rendering into an ANativeWindow.
#pragma once

#include <android/native_window.h>
#include <media/NdkMediaCodec.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_map>

class VideoDecoder {
public:
    // Called from the output thread each time a frame is released to the surface.
    using FrameCallback = std::function<void(uint64_t pcTimeNs, uint32_t decodeUs, uint32_t onGoUs)>;

    ~VideoDecoder() { Stop(); }

    // codec: 0 = H.264, 1 = HEVC. The window is not owned.
    bool Start(ANativeWindow* window, int codec, int width, int height, FrameCallback cb);
    void Stop();
    bool Running() const { return codec_ != nullptr; }

    // Queue one Annex-B access unit. Returns false if no input buffer became available.
    bool Submit(const uint8_t* data, size_t size, uint64_t pcTimeNs, int64_t recvNs);

    std::atomic<uint32_t> framesOut{0}, framesIn{0}, inputDrops{0};

private:
    void OutputLoop();

    AMediaCodec* codec_ = nullptr;
    std::thread outputThread_;
    std::atomic<bool> running_{false};
    FrameCallback callback_;
    std::mutex timesMutex_;
    struct Pending { uint64_t pcTimeNs; int64_t recvNs, submitNs; };
    std::unordered_map<int64_t, Pending> submitTimes_;  // keyed by ptsUs
};
