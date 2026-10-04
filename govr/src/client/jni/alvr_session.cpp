#include "alvr_session.h"

#include <android/native_window_jni.h>
#include <dlfcn.h>

#include <cmath>
#include <climits>
#include <cstring>

#include "VrApi.h"
#include "VrApi_Helpers.h"
#include "alvr_client_core.h"
#include "common.h"

// Function table resolved with dlopen/dlsym, so a missing or broken ALVR library only
// disables SteamVR mode.
struct AlvrApi {
    void* lib = nullptr;
#define ALVR_FN(name) decltype(&::name) name = nullptr;
    ALVR_FN(alvr_initialize_logging)
    ALVR_FN(alvr_initialize_android_context)
    ALVR_FN(alvr_initialize)
    ALVR_FN(alvr_destroy)
    ALVR_FN(alvr_resume)
    ALVR_FN(alvr_pause)
    ALVR_FN(alvr_poll_event)
    ALVR_FN(alvr_hud_message)
    ALVR_FN(alvr_get_decoder_config)
    ALVR_FN(alvr_path_string_to_id)
    ALVR_FN(alvr_send_view_params)
    ALVR_FN(alvr_send_tracking)
    ALVR_FN(alvr_send_button)
    ALVR_FN(alvr_send_battery)
    ALVR_FN(alvr_set_decoder_input_callback)
    ALVR_FN(alvr_report_frame_decoded)
    ALVR_FN(alvr_report_fatal_decoder_error)
    ALVR_FN(alvr_report_compositor_start)
    ALVR_FN(alvr_report_submit)
    ALVR_FN(alvr_protocol_id)
    ALVR_FN(alvr_hostname)
#undef ALVR_FN

    bool Load() {
        lib = dlopen("libalvr_client_core.so", RTLD_NOW | RTLD_LOCAL);
        if (!lib) {
            LOGW("alvr: library not loaded (%s): SteamVR mode disabled", dlerror());
            return false;
        }
#define ALVR_FN(name)                                                   \
    name = (decltype(name))dlsym(lib, #name);                           \
    if (!name) { LOGE("alvr: missing symbol " #name); return false; }
        ALVR_FN(alvr_initialize_logging)
        ALVR_FN(alvr_initialize_android_context)
        ALVR_FN(alvr_initialize)
        ALVR_FN(alvr_destroy)
        ALVR_FN(alvr_resume)
        ALVR_FN(alvr_pause)
        ALVR_FN(alvr_poll_event)
        ALVR_FN(alvr_hud_message)
        ALVR_FN(alvr_get_decoder_config)
        ALVR_FN(alvr_path_string_to_id)
        ALVR_FN(alvr_send_view_params)
        ALVR_FN(alvr_send_tracking)
        ALVR_FN(alvr_send_button)
        ALVR_FN(alvr_send_battery)
        ALVR_FN(alvr_set_decoder_input_callback)
        ALVR_FN(alvr_report_frame_decoded)
        ALVR_FN(alvr_report_fatal_decoder_error)
        ALVR_FN(alvr_report_compositor_start)
        ALVR_FN(alvr_report_submit)
        ALVR_FN(alvr_protocol_id)
        ALVR_FN(alvr_hostname)
#undef ALVR_FN
        return true;
    }
};

namespace {
AlvrSession* gSession = nullptr;  // decoder input callback context (one session per process)
}

bool AlvrSession::Init(JavaVM* vm, jobject activity, int eyeWidth, int eyeHeight) {
    api_ = new AlvrApi();
    if (!api_->Load()) {
        delete api_;
        api_ = nullptr;
        return false;
    }
    gSession = this;
    api_->alvr_initialize_logging();
    api_->alvr_initialize_android_context(vm, activity);
    // The Go decodes ~4K30-class streams; keep the side-by-side frame within budget.
    static const float kRefreshRates[] = {72.0f, 60.0f};
    AlvrClientCapabilities caps = {};
    caps.default_view_width = (uint32_t)eyeWidth;
    caps.default_view_height = (uint32_t)eyeHeight;
    caps.refresh_rates = kRefreshRates;
    caps.refresh_rates_count = 2;
    caps.foveated_encoding = false;  // we present the decoded frame as-is
    caps.encoder_high_profile = true;
    caps.encoder_10_bits = false;
    caps.encoder_av1 = false;        // no AV1 decoder on the Snapdragon 821
    caps.prefer_10bit = false;
    caps.prefer_full_range = true;
    caps.preferred_encoding_gamma = 1.0f;
    caps.prefer_hdr = false;
    api_->alvr_initialize(caps);
    headId_ = api_->alvr_path_string_to_id("/user/head");
    // Inputs of the Quest Touch profile, ALVR's default source profile (server: Quest2Touch).
    rightHandId_ = api_->alvr_path_string_to_id("/user/hand/right");
    leftHandId_ = api_->alvr_path_string_to_id("/user/hand/left");
    using C = proto::VrControllers;
    struct Def { const char* path; bool scalar, left; float (*value)(const C&); };
    static const Def kInputs[] = {
        {"/user/hand/right/input/trigger/value", true, false, [](const C& c) { return c.rtrigger; }},
        {"/user/hand/right/input/trigger/touch", false, false, [](const C& c) { return c.rtrigger > 0.05f ? 1.f : 0.f; }},
        {"/user/hand/right/input/squeeze/value", true, false, [](const C& c) { return c.rgrip; }},
        {"/user/hand/right/input/a/click", false, false, [](const C& c) { return c.buttons & proto::VR_BTN_A ? 1.f : 0.f; }},
        {"/user/hand/right/input/a/touch", false, false, [](const C& c) { return c.buttons & proto::VR_BTN_A ? 1.f : 0.f; }},
        {"/user/hand/right/input/b/click", false, false, [](const C& c) { return c.buttons & proto::VR_BTN_B ? 1.f : 0.f; }},
        {"/user/hand/right/input/b/touch", false, false, [](const C& c) { return c.buttons & proto::VR_BTN_B ? 1.f : 0.f; }},
        {"/user/hand/right/input/system/click", false, false, [](const C& c) { return c.buttons & proto::VR_BTN_SYSTEM ? 1.f : 0.f; }},
        {"/user/hand/right/input/thumbstick/x", true, false, [](const C& c) { return c.rx; }},
        {"/user/hand/right/input/thumbstick/y", true, false, [](const C& c) { return c.ry; }},
        {"/user/hand/right/input/thumbstick/click", false, false, [](const C& c) { return c.buttons & proto::VR_BTN_RSTICK ? 1.f : 0.f; }},
        {"/user/hand/right/input/thumbstick/touch", false, false, [](const C& c) { return c.rx || c.ry || (c.buttons & proto::VR_BTN_RSTICK) ? 1.f : 0.f; }},
        {"/user/hand/left/input/trigger/value", true, true, [](const C& c) { return c.ltrigger; }},
        {"/user/hand/left/input/trigger/touch", false, true, [](const C& c) { return c.ltrigger > 0.05f ? 1.f : 0.f; }},
        {"/user/hand/left/input/squeeze/value", true, true, [](const C& c) { return c.lgrip; }},
        {"/user/hand/left/input/x/click", false, true, [](const C& c) { return c.buttons & proto::VR_BTN_X ? 1.f : 0.f; }},
        {"/user/hand/left/input/x/touch", false, true, [](const C& c) { return c.buttons & proto::VR_BTN_X ? 1.f : 0.f; }},
        {"/user/hand/left/input/y/click", false, true, [](const C& c) { return c.buttons & proto::VR_BTN_Y ? 1.f : 0.f; }},
        {"/user/hand/left/input/y/touch", false, true, [](const C& c) { return c.buttons & proto::VR_BTN_Y ? 1.f : 0.f; }},
        {"/user/hand/left/input/menu/click", false, true, [](const C& c) { return c.buttons & proto::VR_BTN_MENU ? 1.f : 0.f; }},
        {"/user/hand/left/input/thumbstick/x", true, true, [](const C& c) { return c.lx; }},
        {"/user/hand/left/input/thumbstick/y", true, true, [](const C& c) { return c.ly; }},
        {"/user/hand/left/input/thumbstick/click", false, true, [](const C& c) { return c.buttons & proto::VR_BTN_LSTICK ? 1.f : 0.f; }},
        {"/user/hand/left/input/thumbstick/touch", false, true, [](const C& c) { return c.lx || c.ly || (c.buttons & proto::VR_BTN_LSTICK) ? 1.f : 0.f; }},
    };
    for (const Def& d : kInputs)
        inputs_.push_back({api_->alvr_path_string_to_id(d.path), d.scalar, d.left, d.value, 0.0f});
    char buf[256] = {};
    api_->alvr_protocol_id(buf);
    char host[256] = {};
    api_->alvr_hostname(host);
    LOGI("alvr: core initialized, protocol %s, hostname %s, view %dx%d", buf, host, eyeWidth, eyeHeight);
    initialized_ = true;
    return true;
}

void AlvrSession::Shutdown() {
    if (!initialized_) return;
    api_->alvr_destroy();
    initialized_ = false;
}

void AlvrSession::Resume() {
    if (initialized_) api_->alvr_resume();
}

void AlvrSession::Pause() {
    if (initialized_) api_->alvr_pause();
}

bool AlvrSession::DecoderInput(AlvrVideoFrameData frame) {
    AlvrSession* s = gSession;
    if (!s || !s->decoderReady_) return false;
    std::lock_guard<std::mutex> lock(s->decoderMutex_);
    if (!s->configSent_) {
        // The codec config NALs (VPS/SPS/PPS) arrive out of band: prepend them once.
        std::vector<uint8_t> first(s->configNal_);
        first.insert(first.end(), frame.buffer_ptr, frame.buffer_ptr + frame.buffer_size);
        if (!s->decoder_.Submit(first.data(), first.size(), frame.timestamp_ns, NowNanos())) return false;
        s->configSent_ = true;
        return true;
    }
    return s->decoder_.Submit(frame.buffer_ptr, frame.buffer_size, frame.timestamp_ns, NowNanos());
}

void AlvrSession::DestroyVideo(JNIEnv* env) {
    decoderReady_ = false;
    {
        std::lock_guard<std::mutex> lock(decoderMutex_);
        decoder_.Stop();
    }
    if (window_) ANativeWindow_release(window_);
    if (surface_) env->DeleteGlobalRef(surface_);
    if (chain_) vrapi_DestroyTextureSwapChain(chain_);
    window_ = nullptr;
    surface_ = nullptr;
    chain_ = nullptr;
    lastDecodedTs_ = lastShownTs_ = 0;
    current_ = FrameToShow();
}

void AlvrSession::Update(JNIEnv* env) {
    if (!initialized_) return;
    AlvrEvent ev;
    while (api_->alvr_poll_event(&ev)) {
        switch (ev.tag) {
            case ALVR_EVENT_HUD_MESSAGE_UPDATED: {
                const uint64_t n = api_->alvr_hud_message(nullptr);
                std::string msg(n, '\0');
                api_->alvr_hud_message(&msg[0]);
                msg.resize(strlen(msg.c_str()));
                if (msg != hud_) LOGI("alvr: hud: %s", msg.c_str());
                hud_ = msg;
                break;
            }
            case ALVR_EVENT_STREAMING_STARTED: {
                const auto& b = ev.STREAMING_STARTED;
                viewW_ = (int)b.view_width;
                viewH_ = (int)b.view_height;
                LOGI("alvr: streaming started: view %dx%d refresh %.0f gamma %.2f foveated %d hdr %d",
                     viewW_, viewH_, b.refresh_rate_hint, b.encoding_gamma,
                     b.enable_foveated_encoding, b.enable_hdr);
                if (b.enable_foveated_encoding)
                    LOGW("alvr: foveated encoding is ON on the server: disable it, the Go client "
                         "does not de-foveate");
                streaming_ = true;
                viewParamsSent_ = false;
                codec_ = -1;
                break;
            }
            case ALVR_EVENT_STREAMING_STOPPED:
                LOGI("alvr: streaming stopped");
                streaming_ = false;
                DestroyVideo(env);
                break;
            case ALVR_EVENT_DECODER_CONFIG: {
                if (codec_ >= 0) break;  // ignore repeats until reconnection (see C API)
                codec_ = ev.DECODER_CONFIG.codec;
                const uint64_t n = api_->alvr_get_decoder_config(nullptr);
                configNal_.resize(n);
                api_->alvr_get_decoder_config((char*)configNal_.data());
                LOGI("alvr: decoder config codec=%d (%llu bytes)", codec_, (unsigned long long)n);
                needDecoder_ = true;
                break;
            }
            default:
                break;
        }
    }
    if (needDecoder_ && viewW_ > 0) {
        needDecoder_ = false;
        DestroyVideo(env);
        const int w = viewW_ * 2, h = viewH_;  // both eyes side by side
        chain_ = vrapi_CreateAndroidSurfaceSwapChain(w, h);
        surface_ = env->NewGlobalRef(vrapi_GetTextureSwapChainAndroidSurface(chain_));
        window_ = ANativeWindow_fromSurface(env, surface_);
        configSent_ = false;
        bool ok;
        {
            std::lock_guard<std::mutex> lock(decoderMutex_);
            ok = decoder_.Start(window_, codec_ == ALVR_CODEC_HEVC ? 1 : 0, w, h,
                                [this](uint64_t ts, uint32_t decodeUs, uint32_t) {
                                    lastDecodedTs_ = ts;
                                    lastDecodeUs_ = decodeUs;
                                    lastDecodedAtNs_ = NowNanos();
                                    api_->alvr_report_frame_decoded(ts);
                                });
        }
        if (!ok) {
            api_->alvr_report_fatal_decoder_error("MediaCodec start failed");
            return;
        }
        decoderReady_ = true;
        api_->alvr_set_decoder_input_callback(this, &AlvrSession::DecoderInput);
        LOGI("alvr: decoder ready %dx%d", w, h);
    }
}

void AlvrSession::SendTracking(const ovrTracking2& tracking, const ovrMatrix4f proj[2]) {
    if (!initialized_ || !streaming_) return;
    if (!viewParamsSent_) {
        // Eye poses relative to the head (3DoF: only the IPD offset) and FoVs from VrApi.
        AlvrViewParams views[2] = {};
        const float ipd = 0.063f;
        for (int eye = 0; eye < 2; eye++) {
            const ovrMatrix4f& p = proj[eye];
            const float l = (p.M[0][2] - 1.0f) / p.M[0][0], r = (p.M[0][2] + 1.0f) / p.M[0][0];
            const float d = (p.M[1][2] - 1.0f) / p.M[1][1], u = (p.M[1][2] + 1.0f) / p.M[1][1];
            views[eye].pose.orientation = {0, 0, 0, 1};
            views[eye].pose.position[0] = eye == 0 ? -ipd / 2 : ipd / 2;
            views[eye].fov = {atanf(l), atanf(r), atanf(u), atanf(d)};
            LOGI("alvr: view %d fov L%.1f R%.1f U%.1f D%.1f deg", eye, atanf(l) * 57.2958f,
                 atanf(r) * 57.2958f, atanf(u) * 57.2958f, atanf(d) * 57.2958f);
        }
        api_->alvr_send_view_params(views);
        viewParamsSent_ = true;
    }
    AlvrDeviceMotion motions[3] = {};
    AlvrDeviceMotion& head = motions[0];
    head.device_id = headId_;
    const ovrRigidBodyPosef& hp = tracking.HeadPose;
    head.pose.orientation = {hp.Pose.Orientation.x, hp.Pose.Orientation.y, hp.Pose.Orientation.z,
                             hp.Pose.Orientation.w};
    // 3DoF: VrApi's head model gives a small neck offset; the seated eye height is applied
    // by ALVR's "Local" position recentering (pc/alvr/make_session.py).
    head.pose.position[0] = hp.Pose.Position.x;
    head.pose.position[1] = hp.Pose.Position.y;
    head.pose.position[2] = hp.Pose.Position.z;
    head.angular_velocity[0] = hp.AngularVelocity.x;
    head.angular_velocity[1] = hp.AngularVelocity.y;
    head.angular_velocity[2] = hp.AngularVelocity.z;
    const int controllers = SendControllers(hp.Pose, &motions[1]);
    const int64_t nowNs = NowNanos();
    api_->alvr_send_tracking((uint64_t)nowNs, motions, 1 + controllers, nullptr, nullptr);
    std::lock_guard<std::mutex> lock(sentMutex_);
    sent_[sentHead_] = {nowNs, hp.Pose.Orientation};
    sentHead_ = (sentHead_ + 1) % 512;
}

ovrQuatf AlvrSession::OrientationSentAt(int64_t timeNs) {
    std::lock_guard<std::mutex> lock(sentMutex_);
    ovrQuatf best = sent_[(sentHead_ + 511) % 512].q;
    int64_t bestDiff = INT64_MAX;
    for (const auto& e : sent_) {
        if (!e.timeNs) continue;
        const int64_t d = e.timeNs > timeNs ? e.timeNs - timeNs : timeNs - e.timeNs;
        if (d < bestDiff) { bestDiff = d; best = e.q; }
    }
    return best;
}

bool AlvrSession::BeginFrame(FrameToShow* out) {
    if (!initialized_ || !streaming_ || !chain_) return false;
    const uint64_t ts = lastDecodedTs_;
    if (!ts) return false;
    if (ts != current_.timestampNs) {
        // Once per new frame: get the pose and FoV the server rendered this frame with.
        AlvrViewParams vp[2];
        api_->alvr_report_compositor_start(ts, vp);
        current_.timestampNs = ts;
        current_.orientation = {vp[0].pose.orientation.x, vp[0].pose.orientation.y,
                                vp[0].pose.orientation.z, vp[0].pose.orientation.w};
        for (int eye = 0; eye < 2; eye++) {
            current_.fov[eye][0] = vp[eye].fov.left;
            current_.fov[eye][1] = vp[eye].fov.right;
            current_.fov[eye][2] = vp[eye].fov.up;
            current_.fov[eye][3] = vp[eye].fov.down;
        }
    }
    *out = current_;
    // Render orientation: what we sent one pipeline latency before this frame was decoded.
    static const int64_t kPipelineLatencyNs = 60 * 1000000LL;
    const int64_t decodedAt = lastDecodedAtNs_;
    if (decodedAt) out->orientation = OrientationSentAt(decodedAt - kPipelineLatencyNs);
    return true;
}

void AlvrSession::EndFrame(uint64_t timestampNs, double displayTimeS) {
    if (!initialized_ || timestampNs == lastShownTs_) return;
    lastShownTs_ = timestampNs;
    const int64_t queue = (int64_t)(displayTimeS * 1e9) - NowNanos();
    api_->alvr_report_submit(timestampNs, queue > 0 ? (uint64_t)queue : 0);
}

// ---- Virtual controllers (gamepad on the PC)
// The SteamVR dashboard, and the menus of games made for motion controllers, only accept
// laser-pointer input from VR controllers, and the Go has none in SteamVR mode. While the PC
// says so, a virtual right controller is sent: it sits low-right of the head and aims at the
// point 2 m along the gaze (plus the stick offset), so the laser dot lands where the user looks.
// In "VR controllers" mode a left controller joins (low-left, pointing forward) and every
// gamepad input is forwarded to the two controllers (pc/vr_pointer.py has the mapping).

namespace {
struct V3 { float x, y, z; };
V3 Rotate(const ovrQuatf& q, V3 v) {
    // v' = v + 2w(u x v) + 2 u x (u x v)
    const V3 u = {q.x, q.y, q.z};
    const V3 t = {2 * (u.y * v.z - u.z * v.y), 2 * (u.z * v.x - u.x * v.z), 2 * (u.x * v.y - u.y * v.x)};
    return {v.x + q.w * t.x + (u.y * t.z - u.z * t.y), v.y + q.w * t.y + (u.z * t.x - u.x * t.z),
            v.z + q.w * t.z + (u.x * t.y - u.y * t.x)};
}
ovrQuatf Mul(const ovrQuatf& a, const ovrQuatf& b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
ovrQuatf AxisAngle(V3 axis, float a) {
    const float s = sinf(a / 2);
    return {axis.x * s, axis.y * s, axis.z * s, cosf(a / 2)};
}
V3 Norm(V3 v) {
    const float l = sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
    return l > 1e-6f ? V3{v.x / l, v.y / l, v.z / l} : V3{0, 0, -1};
}
V3 Cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
// Orientation whose -Z axis points along `fwd`, with +Y as close to world up as possible.
ovrQuatf LookRotation(V3 fwd) {
    const V3 z = {-fwd.x, -fwd.y, -fwd.z};
    const V3 x = Norm(Cross({0, 1, 0}, z));
    const V3 y = Cross(z, x);
    const float m00 = x.x, m11 = y.y, m22 = z.z, tr = m00 + m11 + m22;
    ovrQuatf q;
    if (tr > 0) {
        const float s = sqrtf(tr + 1) * 2;
        q = {(y.z - z.y) / s, (z.x - x.z) / s, (x.y - y.x) / s, 0.25f * s};
    } else if (m00 > m11 && m00 > m22) {
        const float s = sqrtf(1 + m00 - m11 - m22) * 2;
        q = {0.25f * s, (y.x + x.y) / s, (z.x + x.z) / s, (y.z - z.y) / s};
    } else if (m11 > m22) {
        const float s = sqrtf(1 + m11 - m00 - m22) * 2;
        q = {(y.x + x.y) / s, 0.25f * s, (z.y + y.z) / s, (z.x - x.z) / s};
    } else {
        const float s = sqrtf(1 + m22 - m00 - m11) * 2;
        q = {(z.x + x.z) / s, (z.y + y.z) / s, 0.25f * s, (x.y - y.x) / s};
    }
    return q;
}
// SteamVR draws the laser from the controller model's "tip" component, which the Quest 2 model
// (oculus_quest2_controller_right.json, ALVR's Quest2Touch emulation) tilts by -37.4 deg about X
// relative to the pose ALVR reports: pitch the pose up by as much so the laser runs along the aim.
constexpr float kAimPitchFix = 37.4f * 3.14159265f / 180.0f;
constexpr float kAimDistance = 2.0f;           // m, about where SteamVR places the dashboard
constexpr int64_t kPointerTimeoutNs = 500 * 1000000LL;  // PC silent -> controller off
}  // namespace

void AlvrSession::SetControllers(const proto::VrControllers& c) {
    std::lock_guard<std::mutex> lock(controllersMutex_);
    controllers_ = c;
    controllersAtNs_ = NowNanos();
}

int AlvrSession::SendControllers(const ovrPosef& head, AlvrDeviceMotion* out) {
    proto::VrControllers c;
    {
        std::lock_guard<std::mutex> lock(controllersMutex_);
        c = controllers_;
        if (NowNanos() - controllersAtNs_ > kPointerTimeoutNs) c = {};
    }
    const bool right = c.flags & 1, left = right && (c.flags & 2);
    if (leftWasActive_ && !left) SendControllerInputs({}, true);  // release before it disappears
    leftWasActive_ = left;
    if (!right) {
        if (controllerFrames_) {
            SendControllerInputs({}, false);
            controllerFrames_ = 0;
            LOGI("alvr: virtual controllers off");
        }
        return 0;
    }
    if (!controllerFrames_) LOGI("alvr: virtual controllers on (left %d)", left);
    const ovrQuatf& hq = head.Orientation;
    const V3 hp = {head.Position.x, head.Position.y, head.Position.z};
    // Aim: gaze rotated by the stick offset (yaw about the head's up, then pitch).
    const ovrQuatf off = Mul(AxisAngle({0, 1, 0}, -c.yaw), AxisAngle({1, 0, 0}, c.pitch));
    const V3 dir = Rotate(Mul(hq, off), {0, 0, -1});
    const V3 target = {hp.x + dir.x * kAimDistance, hp.y + dir.y * kAimDistance, hp.z + dir.z * kAimDistance};
    // Hands: low in front of the body, following only the head's yaw. Looking almost straight
    // down (or up) the forward vector has no usable yaw: take it from the head's up vector.
    V3 f = Rotate(hq, {0, 0, -1});
    if (f.x * f.x + f.z * f.z < 0.1f) {
        const V3 up = Rotate(hq, {0, 1, 0});
        f = f.y < 0 ? up : V3{-up.x, -up.y, -up.z};
    }
    const float yaw = atan2f(-f.x, -f.z);
    const ovrQuatf yawOnly = AxisAngle({0, 1, 0}, yaw);
    auto motion = [&](uint64_t id, V3 offset, ovrQuatf q) {
        const V3 o = Rotate(yawOnly, offset);
        AlvrDeviceMotion m = {};
        m.device_id = id;
        m.pose.orientation = {q.x, q.y, q.z, q.w};
        m.pose.position[0] = hp.x + o.x;
        m.pose.position[1] = hp.y + o.y;
        m.pose.position[2] = hp.z + o.z;
        return m;
    };
    const V3 rightAt = Rotate(yawOnly, {0.18f, -0.35f, -0.25f});
    const V3 hand = {hp.x + rightAt.x, hp.y + rightAt.y, hp.z + rightAt.z};
    out[0] = motion(rightHandId_, {0.18f, -0.35f, -0.25f},
                    Mul(LookRotation(Norm({target.x - hand.x, target.y - hand.y, target.z - hand.z})),
                        AxisAngle({1, 0, 0}, kAimPitchFix)));
    int n = 1;
    if (left)  // pointing forward, slightly down
        out[n++] = motion(leftHandId_, {-0.18f, -0.35f, -0.25f},
                          Mul(yawOnly, AxisAngle({1, 0, 0}, kAimPitchFix - 0.35f)));
    // The server ignores buttons until the controller pose is valid: give it a few frames.
    if (++controllerFrames_ > 5) {
        SendControllerInputs(c, false);
        if (left) SendControllerInputs(c, true);
    }
    return n;
}

void AlvrSession::SendControllerInputs(const proto::VrControllers& c, bool left) {
    for (Input& in : inputs_) {
        if (in.left != left) continue;
        const float v = in.value(c);
        if (v == in.sent) continue;
        in.sent = v;
        AlvrButtonValue b = {};
        if (in.scalar) {
            b.tag = ALVR_BUTTON_VALUE_SCALAR;
            b.scalar = v;
        } else {
            b.tag = ALVR_BUTTON_VALUE_BINARY;
            b.binary = v != 0;
        }
        api_->alvr_send_button(in.id, b);
    }
}
