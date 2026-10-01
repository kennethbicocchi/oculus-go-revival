// SteamVR mode: ALVR v20.14.1 client core (Rust, patched for API 25) driven through its C API.
// The core does networking, protocol, statistics and pose bookkeeping; this side provides
// head tracking from VrApi, a MediaCodec decoder and presentation through the VrApi compositor.
#pragma once

#include <android/native_window.h>
#include <jni.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "VrApi_Types.h"
#include "decoder.h"

struct AlvrApi;

class AlvrSession {
public:
    struct FrameToShow {
        uint64_t timestampNs = 0;
        ovrQuatf orientation = {0, 0, 0, 1};  // head orientation the server rendered with
        float fov[2][4] = {};                   // per eye: left, right, up, down (radians)
    };

    // Loads libalvr_client_core.so and initializes it. Safe to call when the library is
    // missing: returns false and the app keeps working in desktop mode.
    bool Init(JavaVM* vm, jobject activity, int eyeWidth, int eyeHeight);
    void Shutdown();
    void Resume();
    void Pause();

    // Main thread, once per frame. Polls events and (re)creates the decoder + surface
    // swapchain when the stream (re)starts. env is the main thread's JNIEnv.
    void Update(JNIEnv* env);
    // Main thread, once per frame: send head tracking for the pose predicted at display time.
    void SendTracking(const ovrTracking2& tracking, const ovrMatrix4f proj[2]);
    // Returns true and fills `out` if a decoded frame is available to present.
    bool BeginFrame(FrameToShow* out);
    void EndFrame(uint64_t timestampNs, double displayTimeS);

    bool Streaming() const { return streaming_; }
    ovrTextureSwapChain* Swapchain() const { return chain_; }
    const std::string& HudMessage() const { return hud_; }
    uint32_t FramesDecoded() const { return decoder_.framesOut; }
    uint32_t InputDrops() const { return decoder_.inputDrops; }
    uint32_t LastDecodeUs() const { return lastDecodeUs_; }

private:
    void DestroyVideo(JNIEnv* env);
    static bool DecoderInput(struct AlvrVideoFrameData frame);

    AlvrApi* api_ = nullptr;
    bool initialized_ = false, viewParamsSent_ = false;
    std::atomic<bool> streaming_{false};
    int viewW_ = 0, viewH_ = 0;
    int codec_ = -1;
    bool needDecoder_ = false;
    std::vector<uint8_t> configNal_;
    std::mutex decoderMutex_;
    VideoDecoder decoder_;
    std::atomic<bool> decoderReady_{false};
    std::atomic<bool> configSent_{false};
    std::atomic<uint64_t> lastDecodedTs_{0};
    uint64_t lastShownTs_ = 0;
    ovrTextureSwapChain* chain_ = nullptr;
    jobject surface_ = nullptr;
    ANativeWindow* window_ = nullptr;
    uint64_t headId_ = 0;
    std::string hud_;
    FrameToShow current_;
    // Our own record of the head orientations sent to the PC. ALVR's Linux server matches each
    // frame to its render pose by rotation similarity, and on this setup the match is often
    // wrong (seconds old), so we use the orientation sent ~kPipelineLatencyNs before the frame
    // was decoded instead (DECISIONS.md D-018).
    struct SentPose { int64_t timeNs; ovrQuatf q; };
    SentPose sent_[512] = {};
    int sentHead_ = 0;
    std::atomic<int64_t> lastDecodedAtNs_{0};
    std::mutex sentMutex_;
    ovrQuatf OrientationSentAt(int64_t timeNs);
    std::atomic<uint32_t> lastDecodeUs_{0};
};
