// Low-latency audio playback: Opus packets -> libopus -> jitter buffer -> OpenSL ES.
// (Android 7.1 has no AAudio; OpenSL ES with the API 25 latency performance mode is the
// fastest path available.)
#pragma once

#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <vector>

struct OpusDecoder;

class AudioPlayer {
public:
    // Called on the OpenSL thread when a packet's PCM is handed to the output queue.
    using PlayedCallback = std::function<void(uint64_t pcTimeNs, uint32_t bufferedMs)>;

    ~AudioPlayer() { Stop(); }
    bool Start(int sampleRate, int channels, int frameMs, PlayedCallback cb);
    void Stop();
    bool Running() const { return player_ != nullptr; }
    // Network thread: decode one Opus packet into the jitter buffer.
    void PushOpus(const uint8_t* data, size_t size, uint64_t pcTimeNs);

    std::atomic<uint32_t> underruns{0}, dropped{0}, packets{0};
    std::atomic<uint32_t> peakLevel{0};  // max |sample| since last read (0..32767)

private:
    struct Chunk { std::vector<int16_t> pcm; uint64_t pcTimeNs; };
    static void BufferCallback(SLAndroidSimpleBufferQueueItf bq, void* ctx);
    void FillNext();

    int rate_ = 48000, channels_ = 2, frameSamples_ = 480;
    OpusDecoder* opus_ = nullptr;
    SLObjectItf engineObj_ = nullptr, mixObj_ = nullptr, player_ = nullptr;
    SLEngineItf engine_ = nullptr;
    SLPlayItf play_ = nullptr;
    SLAndroidSimpleBufferQueueItf queue_ = nullptr;
    std::mutex mutex_;
    std::deque<Chunk> chunks_;
    bool priming_ = true;            // wait for the jitter buffer to fill before playing
    std::vector<int16_t> outBuf_[2];
    int outIndex_ = 0;
    PlayedCallback callback_;
    std::vector<int16_t> decodeBuf_;
};
