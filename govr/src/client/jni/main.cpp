// GoVR client: native VrApi app for the Oculus Go (Android 7.1, Mobile SDK 1.50).
//
// Draws a head-tracked environment (test pattern) and, when the PC streams, a curved
// virtual screen: MediaCodec decodes straight into a VrApi Android-surface swapchain that the
// compositor samples as a cylinder layer (zero copy, a single resampling step).

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <android/native_window_jni.h>
#include <android_native_app_glue.h>
#include <sys/prctl.h>
#include <unistd.h>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "VrApi.h"
#include "VrApi_Helpers.h"
#include "VrApi_SystemUtils.h"
#include "alvr_session.h"
#include "audio.h"
#include "common.h"
#include "decoder.h"
#include "net.h"
#include "protocol.h"

namespace {

constexpr int kCpuLevel = 2;
constexpr int kGpuLevel = 3;

// ---------------------------------------------------------------- EGL

struct Egl {
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLConfig config = nullptr;
    EGLContext context = EGL_NO_CONTEXT;
    EGLSurface tinySurface = EGL_NO_SURFACE;

    bool Create() {
        display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        eglInitialize(display, nullptr, nullptr);
        // Avoid eglChooseConfig: it may force MSAA on the warp target (see SDK samples).
        EGLConfig configs[1024];
        EGLint n = 0;
        eglGetConfigs(display, configs, 1024, &n);
        const EGLint want[] = {EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
                               EGL_ALPHA_SIZE, 8, EGL_DEPTH_SIZE, 0, EGL_STENCIL_SIZE, 0,
                               EGL_SAMPLES, 0, EGL_NONE};
        for (int i = 0; i < n && !config; i++) {
            EGLint v = 0;
            eglGetConfigAttrib(display, configs[i], EGL_RENDERABLE_TYPE, &v);
            if (!(v & EGL_OPENGL_ES3_BIT_KHR)) continue;
            eglGetConfigAttrib(display, configs[i], EGL_SURFACE_TYPE, &v);
            if ((v & (EGL_WINDOW_BIT | EGL_PBUFFER_BIT)) != (EGL_WINDOW_BIT | EGL_PBUFFER_BIT))
                continue;
            int j = 0;
            for (; want[j] != EGL_NONE; j += 2) {
                eglGetConfigAttrib(display, configs[i], want[j], &v);
                if (v != want[j + 1]) break;
            }
            if (want[j] == EGL_NONE) config = configs[i];
        }
        if (!config) { LOGE("no suitable EGL config"); return false; }
        const EGLint ctxAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
        context = eglCreateContext(display, config, EGL_NO_CONTEXT, ctxAttribs);
        if (context == EGL_NO_CONTEXT) { LOGE("eglCreateContext failed"); return false; }
        const EGLint pbAttribs[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
        tinySurface = eglCreatePbufferSurface(display, config, pbAttribs);
        if (!eglMakeCurrent(display, tinySurface, tinySurface, context)) {
            LOGE("eglMakeCurrent failed");
            return false;
        }
        return true;
    }

    void Destroy() {
        if (display == EGL_NO_DISPLAY) return;
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (context != EGL_NO_CONTEXT) eglDestroyContext(display, context);
        if (tinySurface != EGL_NO_SURFACE) eglDestroySurface(display, tinySurface);
        eglTerminate(display);
        display = EGL_NO_DISPLAY;
    }
};

// ---------------------------------------------------------------- GL helpers

GLuint CompileShader(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        LOGE("shader compile failed: %s", log);
    }
    return s;
}

GLuint LinkProgram(const char* vs, const char* fs) {
    GLuint p = glCreateProgram();
    GLuint v = CompileShader(GL_VERTEX_SHADER, vs), f = CompileShader(GL_FRAGMENT_SHADER, fs);
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetProgramInfoLog(p, sizeof(log), nullptr, log);
        LOGE("program link failed: %s", log);
    }
    glDeleteShader(v);
    glDeleteShader(f);
    return p;
}

// ---------------------------------------------------------------- eye swapchains

struct EyeTarget {
    ovrTextureSwapChain* chain = nullptr;
    int length = 0, index = 0, width = 0, height = 0;
    GLuint fbos[8] = {};

    bool Create(int w, int h) {
        width = w;
        height = h;
        chain = vrapi_CreateTextureSwapChain3(VRAPI_TEXTURE_TYPE_2D, GL_RGBA8, w, h, 1, 3);
        if (!chain) return false;
        length = vrapi_GetTextureSwapChainLength(chain);
        if (length > 8) length = 8;
        glGenFramebuffers(length, fbos);
        for (int i = 0; i < length; i++) {
            GLuint tex = vrapi_GetTextureSwapChainHandle(chain, i);
            glBindTexture(GL_TEXTURE_2D, tex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbos[i]);
            glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
            GLenum st = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
            if (st != GL_FRAMEBUFFER_COMPLETE) { LOGE("FBO incomplete 0x%x", st); return false; }
        }
        glBindTexture(GL_TEXTURE_2D, 0);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        return true;
    }

    void Bind() {
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbos[index]);
        glViewport(0, 0, width, height);
    }

    void Advance() { index = (index + 1) % length; }

    void Destroy() {
        if (!chain) return;
        glDeleteFramebuffers(length, fbos);
        vrapi_DestroyTextureSwapChain(chain);
        chain = nullptr;
    }
};

// ---------------------------------------------------------------- test pattern

const char* kFullscreenVS = R"(#version 300 es
out vec2 vNdc;
void main() {
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    vNdc = p * 2.0 - 1.0;
    gl_Position = vec4(vNdc, 0.0, 1.0);
}
)";

// Draws a world-fixed sphere around the viewer: 15-degree lat/long grid, coloured
// cardinal directions, a red ring straight ahead. Any head rotation moves the pattern.
const char* kEnvironmentFS = R"(#version 300 es
precision highp float;
in vec2 vNdc;
uniform mat3 uWorldFromView;
uniform vec4 uProj;   // P00, P11, P02, P12 of the eye projection
uniform int uEye;
out vec4 outColor;
const float PI = 3.14159265;
void main() {
    vec3 v = vec3((vNdc.x + uProj.z) / uProj.x, (vNdc.y + uProj.w) / uProj.y, -1.0);
    vec3 d = normalize(uWorldFromView * v);
    float yaw = atan(d.x, -d.z);
    float pitch = asin(clamp(d.y, -1.0, 1.0));
    vec3 col = d.y < 0.0 ? vec3(0.08, 0.10, 0.08) : vec3(0.05, 0.06, 0.14);
    // cardinal tints: front red, right green, back blue, left yellow
    float a = mod(yaw + PI * 2.25, PI * 2.0);
    int sector = int(a / (PI * 0.5));
    vec3 tint = sector == 0 ? vec3(0.5, 0.1, 0.1) : sector == 1 ? vec3(0.1, 0.5, 0.1)
              : sector == 2 ? vec3(0.1, 0.1, 0.5) : vec3(0.5, 0.5, 0.1);
    col += tint * 0.35;
    vec2 g = vec2(yaw, pitch) * (180.0 / PI / 15.0);
    vec2 f = abs(fract(g - 0.5) - 0.5) / fwidth(g);
    float line = 1.0 - min(min(f.x, f.y), 1.0);
    col = mix(col, vec3(0.85), line * 0.8);
    float horizon = 1.0 - min(abs(pitch) / fwidth(pitch) / 2.0, 1.0);
    col = mix(col, vec3(1.0, 1.0, 1.0), horizon);
    float fwd = length(vec2(yaw, pitch));
    col = mix(col, vec3(1.0, 0.1, 0.1), smoothstep(0.012, 0.0, abs(fwd - 0.08)));
    // eye marker: small square in the lower-left of each eye (L = cyan, R = magenta)
    if (vNdc.x < -0.55 && vNdc.x > -0.65 && vNdc.y < -0.55 && vNdc.y > -0.65)
        col = uEye == 0 ? vec3(0.0, 1.0, 1.0) : vec3(1.0, 0.0, 1.0);
    outColor = vec4(col, 1.0);
}
)";

struct TestPattern {
    GLuint program = 0, vao = 0;
    GLint uWorldFromView = -1, uProj = -1, uEye = -1;

    void Create() {
        program = LinkProgram(kFullscreenVS, kEnvironmentFS);
        uWorldFromView = glGetUniformLocation(program, "uWorldFromView");
        uProj = glGetUniformLocation(program, "uProj");
        uEye = glGetUniformLocation(program, "uEye");
        glGenVertexArrays(1, &vao);
    }

    void Draw(const ovrMatrix4f& view, const ovrMatrix4f& proj, int eye) {
        // Column j of world-from-view (= R^T) is row j of the view rotation R.
        float m[9];
        for (int j = 0; j < 3; j++)
            for (int i = 0; i < 3; i++) m[j * 3 + i] = view.M[j][i];
        glUseProgram(program);
        glUniformMatrix3fv(uWorldFromView, 1, GL_FALSE, m);
        glUniform4f(uProj, proj.M[0][0], proj.M[1][1], proj.M[0][2], proj.M[1][2]);
        glUniform1i(uEye, eye);
        glBindVertexArray(vao);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glBindVertexArray(0);
    }

    void Destroy() {
        if (program) glDeleteProgram(program);
        if (vao) glDeleteVertexArrays(1, &vao);
        program = vao = 0;
    }
};

void QuatToEuler(const ovrQuatf& q, float* yaw, float* pitch, float* roll) {
    const float r2d = 180.0f / (float)M_PI;
    *yaw = atan2f(2.f * (q.w * q.y + q.x * q.z), 1.f - 2.f * (q.x * q.x + q.y * q.y)) * r2d;
    float s = 2.f * (q.w * q.x - q.y * q.z);
    *pitch = asinf(s > 1.f ? 1.f : (s < -1.f ? -1.f : s)) * r2d;
    *roll = atan2f(2.f * (q.w * q.z + q.x * q.y), 1.f - 2.f * (q.x * q.x + q.z * q.z)) * r2d;
}

// ---------------------------------------------------------------- video screen

// Curved screen placement. Defaults: 2 m radius, 100 degrees wide.
struct ScreenParams {
    float radius = 2.0f;
    float arcDeg = 100.0f;
    float pitchDeg = 0.0f;
    uint32_t recenterSeq = 0;
    bool showEnvironment = true;
    bool desktopOverVr = false;   // show the desktop screen on top of the SteamVR stream
    bool debugProjection = false; // show the desktop stream through the SteamVR layer path
    int equirect = 0;             // 0 flat screen, 1 360, 2 360 3D top/bottom, 3 180, 4 180 3D SBS
};

// Build a cylinder layer showing a texW x texH texture over arcDeg degrees.
ovrLayerCylinder2 BuildScreenLayer(ovrTextureSwapChain* chain, int texW, int texH,
                                   const ScreenParams& sp, float yawRad,
                                   const ovrTracking2& tracking) {
    ovrLayerCylinder2 layer = vrapi_DefaultLayerCylinder2();
    layer.Header.SrcBlend = VRAPI_FRAME_LAYER_BLEND_SRC_ALPHA;
    layer.Header.DstBlend = VRAPI_FRAME_LAYER_BLEND_ONE_MINUS_SRC_ALPHA;
    layer.Header.Flags |= VRAPI_FRAME_LAYER_FLAG_CLIP_TO_TEXTURE_RECT;
    layer.HeadPose = tracking.HeadPose;

    // density = texture pixels per full 360 degrees (see VrCompositor sample).
    const float density = texW * 360.0f / sp.arcDeg;
    const float r = sp.radius;
    const ovrMatrix4f scale = ovrMatrix4f_CreateScale(r, r * (float)texH * VRAPI_PI / density, r);
    const ovrMatrix4f rotX = ovrMatrix4f_CreateRotation(sp.pitchDeg * VRAPI_PI / 180.0f, 0, 0);
    const ovrMatrix4f rotY = ovrMatrix4f_CreateRotation(0, yawRad, 0);
    const ovrMatrix4f m1 = ovrMatrix4f_Multiply(&rotX, &scale);
    const ovrMatrix4f model = ovrMatrix4f_Multiply(&rotY, &m1);

    const float circScale = density * 0.5f / texW;
    const float circBias = -circScale * (0.5f * (1.0f - 1.0f / circScale));
    const float texScaleY = 0.5f;
    const float texBiasY = -texScaleY * (0.5f * (1.0f - (1.0f / texScaleY)));
    for (int eye = 0; eye < 2; eye++) {
        const ovrMatrix4f mv = ovrMatrix4f_Multiply(&tracking.Eye[eye].ViewMatrix, &model);
        layer.Textures[eye].TexCoordsFromTanAngles = ovrMatrix4f_Inverse(&mv);
        layer.Textures[eye].ColorSwapChain = chain;
        layer.Textures[eye].SwapChainIndex = 0;
        layer.Textures[eye].TextureMatrix.M[0][0] = circScale;
        layer.Textures[eye].TextureMatrix.M[0][2] = circBias;
        layer.Textures[eye].TextureMatrix.M[1][1] = texScaleY;
        layer.Textures[eye].TextureMatrix.M[1][2] = texBiasY;
        layer.Textures[eye].TextureRect = {0, 0, 1, 1};
    }
    return layer;
}

// 360/180 video on a world-fixed sphere. yawRad turns the sphere's front to where the user
// looked when recentering. mode: 1 360, 2 360 3D top/bottom, 3 180, 4 180 3D side-by-side.
// In this layer's texture space u spans 360 degrees and v runs top-down on decoder surfaces.
ovrLayerEquirect2 BuildEquirectLayer(ovrTextureSwapChain* chain, float yawRad, int mode) {
    ovrLayerEquirect2 layer = vrapi_DefaultLayerEquirect2();
    layer.HeadPose.Pose.Orientation = {0, 0, 0, 1};
    layer.TexCoordsFromTanAngles = ovrMatrix4f_CreateRotation(0.0f, -yawRad, 0.0f);
    if (mode >= 3) layer.Header.Flags |= VRAPI_FRAME_LAYER_FLAG_CLIP_TO_TEXTURE_RECT;
    for (int eye = 0; eye < 2; eye++) {
        auto& t = layer.Textures[eye];
        t.ColorSwapChain = chain;
        t.SwapChainIndex = 0;
        switch (mode) {
            case 2:  // top/bottom: left eye = v 0..0.5
                t.TextureMatrix.M[1][1] = 0.5f;
                t.TextureMatrix.M[1][2] = eye == 0 ? 0.0f : 0.5f;
                t.TextureRect = {0.0f, eye == 0 ? 0.0f : 0.5f, 1.0f, 0.5f};
                break;
            case 3:  // 180 mono: the image covers the front half, u 0.25..0.75
                t.TextureMatrix.M[0][0] = 2.0f;
                t.TextureMatrix.M[0][2] = -0.5f;
                t.TextureRect = {0.0f, 0.0f, 1.0f, 1.0f};
                break;
            case 4:  // 180 side-by-side: each eye half covers the front half of the sphere
                t.TextureMatrix.M[0][0] = 1.0f;
                t.TextureMatrix.M[0][2] = eye == 0 ? -0.25f : 0.25f;
                t.TextureRect = {eye * 0.5f, 0.0f, 0.5f, 1.0f};
                break;
            default:
                break;
        }
    }
    return layer;
}

// Full-view projection layer from a side-by-side video surface (SteamVR mode).
// fov: per eye left, right, up, down in radians (left/down negative). The layer's head pose
// is the pose the frame was rendered with, so the compositor's timewarp corrects the rest.
ovrLayerProjection2 BuildVideoProjectionLayer(ovrTextureSwapChain* chain, const float fov[2][4],
                                              const ovrQuatf& renderOrientation,
                                              const ovrTracking2& tracking) {
    ovrLayerProjection2 layer = vrapi_DefaultLayerProjection2();
    layer.HeadPose = tracking.HeadPose;
    layer.HeadPose.Pose.Orientation = renderOrientation;
    const float n = 0.1f;
    for (int eye = 0; eye < 2; eye++) {
        const ovrMatrix4f proj = ovrMatrix4f_CreateProjection(
            tanf(fov[eye][0]) * n, tanf(fov[eye][1]) * n, tanf(fov[eye][3]) * n,
            tanf(fov[eye][2]) * n, n, 0.0f);
        ovrMatrix4f m = ovrMatrix4f_TanAngleMatrixFromProjection(&proj);
        // Squeeze into this eye's half: u' = 0.5 u + 0.5 eye (row 2 is the projective w).
        for (int c = 0; c < 4; c++) m.M[0][c] = 0.5f * m.M[0][c] + 0.5f * eye * m.M[2][c];
        // Decoder surfaces are top-down, eye buffers bottom-up: v' = 1 - v.
        for (int c = 0; c < 4; c++) m.M[1][c] = m.M[2][c] - m.M[1][c];
        layer.Textures[eye].ColorSwapChain = chain;
        layer.Textures[eye].SwapChainIndex = 0;
        layer.Textures[eye].TexCoordsFromTanAngles = m;
        layer.Textures[eye].TextureRect = {eye * 0.5f, 0.0f, 0.5f, 1.0f};
    }
    return layer;
}

void FovFromProjection(const ovrMatrix4f& p, float out[4]) {
    out[0] = atanf((p.M[0][2] - 1.0f) / p.M[0][0]);
    out[1] = atanf((p.M[0][2] + 1.0f) / p.M[0][0]);
    out[2] = atanf((p.M[1][2] + 1.0f) / p.M[1][1]);
    out[3] = atanf((p.M[1][2] - 1.0f) / p.M[1][1]);
}

// ---------------------------------------------------------------- 2D OSD panel

// Head-locked flat panel for menus and player status. The PC sends plain text (OSD message);
// Android's Canvas renders it (MainActivity.renderText) into a texture shown as a cylinder layer
// fixed to the view, so it stays flat and readable whatever the video projection is.
struct OsdPanel {
    static constexpr int kW = 1024, kH = 640;
    ovrTextureSwapChain* chain = nullptr;
    std::vector<uint8_t> pixels;
    jobject buffer = nullptr;
    jclass activityClass = nullptr;
    jmethodID render = nullptr;
    std::mutex mutex;
    std::string pending;
    bool dirty = false, visible = false;

    void Create(JNIEnv* env, jobject activity) {
        chain = vrapi_CreateTextureSwapChain3(VRAPI_TEXTURE_TYPE_2D, GL_RGBA8, kW, kH, 1, 1);
        const GLuint tex = vrapi_GetTextureSwapChainHandle(chain, 0);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);
        pixels.assign(kW * kH * 4, 0);
        buffer = env->NewGlobalRef(env->NewDirectByteBuffer(pixels.data(), pixels.size()));
        jclass cls = env->GetObjectClass(activity);
        activityClass = (jclass)env->NewGlobalRef(cls);
        render = env->GetStaticMethodID(activityClass, "renderText",
                                        "(Ljava/lang/String;IILjava/nio/ByteBuffer;)V");
        if (!render) { LOGE("osd: renderText not found"); env->ExceptionClear(); }
    }

    void SetText(const std::string& text) {  // any thread
        std::lock_guard<std::mutex> lock(mutex);
        if (text == pending) return;
        pending = text;
        dirty = true;
    }

    static jstring ToJString(JNIEnv* env, const std::string& s) {
        // UTF-8 -> UTF-16 (NewStringUTF expects modified UTF-8, which breaks on emoji).
        std::u16string out;
        for (size_t i = 0; i < s.size();) {
            uint32_t c = (uint8_t)s[i], n = 0;
            if (c < 0x80) n = 0; else if ((c >> 5) == 6) { c &= 0x1f; n = 1; }
            else if ((c >> 4) == 14) { c &= 0x0f; n = 2; } else { c &= 0x07; n = 3; }
            for (uint32_t k = 1; k <= n && i + k < s.size(); k++) c = (c << 6) | ((uint8_t)s[i + k] & 0x3f);
            i += n + 1;
            if (c >= 0x10000) {
                c -= 0x10000;
                out.push_back((char16_t)(0xD800 + (c >> 10)));
                out.push_back((char16_t)(0xDC00 + (c & 0x3ff)));
            } else {
                out.push_back((char16_t)c);
            }
        }
        return env->NewString((const jchar*)out.data(), (jsize)out.size());
    }

    void Update(JNIEnv* env) {  // main (GL) thread
        std::string text;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!dirty) return;
            dirty = false;
            text = pending;
        }
        visible = !text.empty();
        if (!visible || !render) return;
        jstring js = ToJString(env, text);
        env->CallStaticVoidMethod(activityClass, render, js, kW, kH, buffer);
        env->DeleteLocalRef(js);
        if (env->ExceptionCheck()) { env->ExceptionDescribe(); env->ExceptionClear(); return; }
        glBindTexture(GL_TEXTURE_2D, vrapi_GetTextureSwapChainHandle(chain, 0));
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kW, kH, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    // Flat-ish panel ~55 degrees wide, 1.5 m away, slightly below eye level, fixed to the view.
    ovrLayerCylinder2 BuildLayer() const {
        ScreenParams sp;
        sp.radius = 1.5f;
        sp.arcDeg = 55.0f;
        sp.pitchDeg = -6.0f;
        ovrTracking2 headSpace = {};
        headSpace.HeadPose.Pose.Orientation.w = 1.0f;
        headSpace.Eye[0].ViewMatrix = ovrMatrix4f_CreateIdentity();
        headSpace.Eye[1].ViewMatrix = ovrMatrix4f_CreateIdentity();
        ovrLayerCylinder2 layer = BuildScreenLayer(chain, kW, kH, sp, 0.0f, headSpace);
        layer.Header.Flags |= VRAPI_FRAME_LAYER_FLAG_FIXED_TO_VIEW;
        layer.Header.SrcBlend = VRAPI_FRAME_LAYER_BLEND_ONE;  // Android bitmaps are premultiplied
        layer.Header.DstBlend = VRAPI_FRAME_LAYER_BLEND_ONE_MINUS_SRC_ALPHA;
        return layer;
    }
};

// ---------------------------------------------------------------- app

struct App {
    android_app* android = nullptr;
    ovrJava java = {};
    Egl egl;
    ovrMobile* ovr = nullptr;
    ANativeWindow* window = nullptr;
    bool resumed = false;
    EyeTarget eyes[2];
    TestPattern pattern;
    long long frameIndex = 1;
    double lastStats = 0;
    int framesSinceStats = 0;

    // streaming
    NetClient net;
    VideoDecoder decoder;
    std::mutex decoderMutex;           // net thread submits, main thread (re)configures
    std::atomic<bool> decoderReady{false};
    std::atomic<bool> waitKeyframe{true};
    std::mutex configMutex;
    bool configPending = false, stopPending = false;
    proto::VideoConfig pendingConfig = {};
    ScreenParams screen;               // guarded by configMutex
    uint32_t appliedRecenterSeq = 0;
    bool needAutoRecenter = true;      // place the screen in view when a stream starts
    float screenYaw = 0.0f, screenPitch = 0.0f;
    ovrTextureSwapChain* videoChain = nullptr;
    jobject videoSurface = nullptr;
    ANativeWindow* videoWindow = nullptr;
    int videoW = 0, videoH = 0;
    std::atomic<uint64_t> lastLatencyUs{0};
    AlvrSession alvr;
    OsdPanel osd;
    AudioPlayer audio;
    std::mutex audioMutex;             // net thread starts/feeds, connection loss stops
    uint32_t audioAckCounter = 0;
    std::atomic<uint32_t> lastDecodeUs{0};

    void StartNetwork() {
        net.Start(proto::kPort,
                  [this](uint8_t type, const uint8_t* d, uint32_t n) { OnMessage(type, d, n); },
                  [this](bool connected) {
                      if (connected) {
                          proto::HelloClient h = {proto::kVersion, (uint32_t)eyes[0].width,
                                                  (uint32_t)eyes[0].height, 72.0f};
                          net.Send(proto::HELLO_CLIENT, &h, sizeof(h));
                      } else {
                          {
                              std::lock_guard<std::mutex> lock(audioMutex);
                              audio.Stop();
                          }
                          osd.SetText("");
                          std::lock_guard<std::mutex> lock(configMutex);
                          stopPending = true;
                          decoderReady = false;
                      }
                  });
    }

    // Network thread.
    void OnMessage(uint8_t type, const uint8_t* d, uint32_t n) {
        switch (type) {
            case proto::VIDEO_CONFIG: {
                if (n < sizeof(proto::VideoConfig)) break;
                std::lock_guard<std::mutex> lock(configMutex);
                memcpy(&pendingConfig, d, sizeof(pendingConfig));
                configPending = true;
                decoderReady = false;
                LOGI("net: video config codec=%u %ux%u", pendingConfig.codec, pendingConfig.width,
                     pendingConfig.height);
                break;
            }
            case proto::VIDEO_FRAME: {
                if (n < sizeof(proto::VideoFrameHeader) || !decoderReady) break;
                proto::VideoFrameHeader fh;
                memcpy(&fh, d, sizeof(fh));
                if (waitKeyframe && !(fh.flags & 1)) break;
                waitKeyframe = false;
                std::lock_guard<std::mutex> lock(decoderMutex);
                if (!decoder.Submit(d + sizeof(fh), n - sizeof(fh), fh.pcTimeNs, net.lastMessageNs)) {
                    // Decoder congested: drop until the next keyframe to avoid corruption.
                    waitKeyframe = true;
                    net.Send(proto::REQUEST_IDR, nullptr, 0);
                }
                break;
            }
            case proto::SCREEN: {
                if (n < sizeof(proto::Screen)) break;
                proto::Screen s;
                memcpy(&s, d, sizeof(s));
                std::lock_guard<std::mutex> lock(configMutex);
                screen.radius = s.radius;
                screen.arcDeg = s.arcDeg;
                screen.pitchDeg = s.pitchDeg;
                screen.recenterSeq = s.recenterSeq;
                screen.showEnvironment = s.flags & 1;
                screen.desktopOverVr = s.flags & 2;
                screen.debugProjection = s.flags & 4;
                screen.equirect = (s.flags & 8) ? 1 : (s.flags & 16) ? 2 : (s.flags & 32) ? 3
                                : (s.flags & 64) ? 4 : 0;
                break;
            }
            case proto::AUDIO_CONFIG: {
                if (n < sizeof(proto::AudioConfig)) break;
                proto::AudioConfig ac;
                memcpy(&ac, d, sizeof(ac));
                std::lock_guard<std::mutex> lock(audioMutex);
                audio.Start((int)ac.sampleRate, (int)ac.channels, (int)ac.frameMs,
                            [this](uint64_t pcTimeNs, uint32_t bufferedMs) {
                                // ~10 acks/s are plenty for latency statistics.
                                if (++audioAckCounter % 10) return;
                                proto::AudioAck a = {pcTimeNs, bufferedMs, audio.underruns.load()};
                                net.Send(proto::AUDIO_ACK, &a, sizeof(a));
                            });
                break;
            }
            case proto::AUDIO_FRAME: {
                if (n <= sizeof(proto::AudioFrameHeader)) break;
                proto::AudioFrameHeader ah;
                memcpy(&ah, d, sizeof(ah));
                audio.PushOpus(d + sizeof(ah), n - sizeof(ah), ah.pcTimeNs);
                break;
            }
            case proto::OSD:
                osd.SetText(std::string((const char*)d, n));
                break;
            case proto::PING:
                net.Send(proto::PONG, d, n);
                break;
            default:
                break;
        }
    }

    void DestroyVideoSurface() {
        {
            std::lock_guard<std::mutex> lock(decoderMutex);
            decoder.Stop();
        }
        if (videoWindow) ANativeWindow_release(videoWindow);
        if (videoSurface) java.Env->DeleteGlobalRef(videoSurface);
        if (videoChain) vrapi_DestroyTextureSwapChain(videoChain);
        videoWindow = nullptr;
        videoSurface = nullptr;
        videoChain = nullptr;
    }

    // Main thread: apply decoder (re)configuration requested by the network thread.
    void HandleStreamChanges() {
        proto::VideoConfig cfg;
        bool start = false, stop = false;
        {
            std::lock_guard<std::mutex> lock(configMutex);
            start = configPending;
            stop = stopPending;
            cfg = pendingConfig;
            configPending = stopPending = false;
        }
        if (stop && !start) {
            DestroyVideoSurface();
            return;
        }
        if (!start) return;
        DestroyVideoSurface();
        videoW = (int)cfg.width;
        videoH = (int)cfg.height;
        videoChain = vrapi_CreateAndroidSurfaceSwapChain(videoW, videoH);
        if (!videoChain) { LOGE("vrapi_CreateAndroidSurfaceSwapChain failed"); return; }
        jobject surface = vrapi_GetTextureSwapChainAndroidSurface(videoChain);
        videoSurface = java.Env->NewGlobalRef(surface);
        videoWindow = ANativeWindow_fromSurface(java.Env, videoSurface);
        bool ok;
        {
            std::lock_guard<std::mutex> lock(decoderMutex);
            ok = decoder.Start(videoWindow, (int)cfg.codec, videoW, videoH,
                               [this](uint64_t pcTimeNs, uint32_t decodeUs, uint32_t onGoUs) {
                                   proto::FrameAck a = {pcTimeNs, decodeUs, onGoUs};
                                   lastDecodeUs = decodeUs;
                                   net.Send(proto::FRAME_ACK, &a, sizeof(a));
                               });
        }
        if (!ok) return;
        waitKeyframe = true;
        decoderReady = true;
        net.Send(proto::REQUEST_IDR, nullptr, 0);
    }

    void HandleVrModeChanges() {
        if (resumed && window) {
            if (ovr) return;
            ovrModeParms parms = vrapi_DefaultModeParms(&java);
            parms.Flags &= ~VRAPI_MODE_FLAG_RESET_WINDOW_FULLSCREEN;
            parms.Flags |= VRAPI_MODE_FLAG_NATIVE_WINDOW;
            parms.Display = (size_t)egl.display;
            parms.WindowSurface = (size_t)window;
            parms.ShareContext = (size_t)egl.context;
            ovr = vrapi_EnterVrMode(&parms);
            if (!ovr) {
                LOGE("vrapi_EnterVrMode failed (invalid window?)");
                window = nullptr;
                return;
            }
            LOGI("entered VR mode");
            vrapi_SetClockLevels(ovr, kCpuLevel, kGpuLevel);
            vrapi_SetPerfThread(ovr, VRAPI_PERF_THREAD_TYPE_MAIN, gettid());
            vrapi_SetDisplayRefreshRate(ovr, 72.0f);
            alvr.Resume();
        } else if (ovr) {
            alvr.Pause();
            vrapi_LeaveVrMode(ovr);
            ovr = nullptr;
            LOGI("left VR mode");
        }
    }

    void HandleVrApiEvents() {
        ovrEventDataBuffer buf = {};
        for (;;) {
            ovrEventHeader* h = (ovrEventHeader*)&buf;
            if (vrapi_PollEvent(h) != ovrSuccess) break;
            switch (h->EventType) {
                case VRAPI_EVENT_FOCUS_GAINED: LOGI("event: focus gained"); break;
                case VRAPI_EVENT_FOCUS_LOST: LOGI("event: focus lost"); break;
                case VRAPI_EVENT_VISIBILITY_GAINED: LOGI("event: visibility gained"); break;
                case VRAPI_EVENT_VISIBILITY_LOST: LOGI("event: visibility lost"); break;
                default: break;
            }
        }
    }

    void RenderFrame() {
        frameIndex++;
        const double displayTime = vrapi_GetPredictedDisplayTime(ovr, frameIndex);
        const ovrTracking2 tracking = vrapi_GetPredictedTracking2(ovr, displayTime);

        ScreenParams sp;
        {
            std::lock_guard<std::mutex> lock(configMutex);
            sp = screen;
        }
        const bool streaming = videoChain && decoder.framesOut > 0;
        if (!streaming) needAutoRecenter = true;
        if (sp.recenterSeq != appliedRecenterSeq || (streaming && needAutoRecenter)) {
            // Recenter = put the screen where the user is looking (yaw and pitch), so it
            // also works reclined or lying down.
            appliedRecenterSeq = sp.recenterSeq;
            needAutoRecenter = false;
            float y, p, r;
            QuatToEuler(tracking.HeadPose.Pose.Orientation, &y, &p, &r);
            screenYaw = y * VRAPI_PI / 180.0f;
            screenPitch = p;
            LOGI("recenter: screen yaw %.1f pitch %.1f deg", y, p);
        }
        sp.pitchDeg += screenPitch;

        ovrLayerProjection2 envLayer = vrapi_DefaultLayerProjection2();
        envLayer.HeadPose = tracking.HeadPose;
        const float envScale = streaming ? (sp.showEnvironment ? 0.25f : 0.0f) : 1.0f;
        envLayer.Header.ColorScale = {envScale, envScale, envScale, 1.0f};
        for (int eye = 0; eye < 2; eye++) {
            EyeTarget& t = eyes[eye];
            t.Bind();
            glDisable(GL_DEPTH_TEST);
            glDisable(GL_CULL_FACE);
            glDisable(GL_SCISSOR_TEST);
            pattern.Draw(tracking.Eye[eye].ViewMatrix, tracking.Eye[eye].ProjectionMatrix, eye);
            // Clear a 1-pixel border so the timewarp's clamp-to-edge doesn't smear colour.
            glEnable(GL_SCISSOR_TEST);
            glClearColor(0, 0, 0, 1);
            const int w = t.width, h = t.height;
            const int rects[4][4] = {{0, 0, w, 1}, {0, h - 1, w, 1}, {0, 0, 1, h}, {w - 1, 0, 1, h}};
            for (auto& r : rects) {
                glScissor(r[0], r[1], r[2], r[3]);
                glClear(GL_COLOR_BUFFER_BIT);
            }
            glDisable(GL_SCISSOR_TEST);
            envLayer.Textures[eye].ColorSwapChain = t.chain;
            envLayer.Textures[eye].SwapChainIndex = t.index;
            envLayer.Textures[eye].TexCoordsFromTanAngles =
                ovrMatrix4f_TanAngleMatrixFromProjection(&tracking.Eye[eye].ProjectionMatrix);
            t.Advance();
        }
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);

        ovrMatrix4f projs[2] = {tracking.Eye[0].ProjectionMatrix, tracking.Eye[1].ProjectionMatrix};
        alvr.SendTracking(tracking, projs);

        // Layer stack: SteamVR stream (if any) replaces the environment; the desktop screen
        // is drawn on top unless SteamVR is streaming and the overlay is off.
        ovrLayerProjection2 vrLayer;
        ovrLayerCylinder2 screenLayer;
        ovrLayerEquirect2 sphereLayer;
        ovrLayerCylinder2 osdLayer;
        const ovrLayerHeader2* layers[4];
        int layerCount = 0;
        AlvrSession::FrameToShow vrFrame;
        const bool vrStreaming = alvr.BeginFrame(&vrFrame);
        if (vrStreaming) {
            vrLayer = BuildVideoProjectionLayer(alvr.Swapchain(), vrFrame.fov, vrFrame.orientation, tracking);
            layers[layerCount++] = &vrLayer.Header;
        } else if (streaming && sp.equirect) {
            sphereLayer = BuildEquirectLayer(videoChain, screenYaw, sp.equirect);
            layers[layerCount++] = &sphereLayer.Header;
        } else if (streaming && sp.debugProjection) {
            float fov[2][4];
            FovFromProjection(tracking.Eye[0].ProjectionMatrix, fov[0]);
            FovFromProjection(tracking.Eye[1].ProjectionMatrix, fov[1]);
            vrLayer = BuildVideoProjectionLayer(videoChain, fov, tracking.HeadPose.Pose.Orientation, tracking);
            layers[layerCount++] = &vrLayer.Header;
        } else {
            layers[layerCount++] = &envLayer.Header;
        }
        if (streaming && !sp.debugProjection && !sp.equirect && (!vrStreaming || sp.desktopOverVr)) {
            screenLayer = BuildScreenLayer(videoChain, videoW, videoH, sp, screenYaw, tracking);
            layers[layerCount++] = &screenLayer.Header;
        }
        osd.Update(java.Env);
        if (osd.visible) {
            osdLayer = osd.BuildLayer();
            layers[layerCount++] = &osdLayer.Header;
        }
        ovrSubmitFrameDescription2 desc = {};
        desc.SwapInterval = 1;
        desc.FrameIndex = frameIndex;
        desc.DisplayTime = displayTime;
        desc.LayerCount = layerCount;
        desc.Layers = layers;
        vrapi_SubmitFrame2(ovr, &desc);
        if (vrStreaming) alvr.EndFrame(vrFrame.timestampNs, displayTime);

        const ovrQuatf& q = tracking.HeadPose.Pose.Orientation;
        if (net.Connected()) {
            proto::HeadPose hp = {(uint64_t)NowNanos(), q.x, q.y, q.z, q.w};
            net.Send(proto::HEAD_POSE, &hp, sizeof(hp));
        }

        framesSinceStats++;
        const double now = NowSeconds();
        static double lastPoseLog = 0;
        if (vrStreaming && now - lastPoseLog >= 0.25) {
            float hy, hp, hr, fy, fp, fr;
            QuatToEuler(tracking.HeadPose.Pose.Orientation, &hy, &hp, &hr);
            QuatToEuler(vrFrame.orientation, &fy, &fp, &fr);
            LOGI("alvr pose: head yaw %.1f pitch %.1f | frame yaw %.1f pitch %.1f | age %.0f ms",
                 hy, hp, fy, fp, (NowNanos() - (int64_t)vrFrame.timestampNs) / 1e6);
            lastPoseLog = now;
        }
        if (now - lastStats >= 2.0 && alvr.Streaming()) {
            static uint32_t lastAlvrFrames = 0;
            const uint32_t f = alvr.FramesDecoded();
            if (f < lastAlvrFrames) lastAlvrFrames = 0;  // decoder was restarted
            // Age of the frame on screen = now - the tracking timestamp it was rendered for
            // (both on this headset's clock): motion-to-display latency of the PC pipeline.
            const double ageMs = vrStreaming ? (NowNanos() - (int64_t)vrFrame.timestampNs) / 1e6 : -1;
            LOGI("stats alvr: decoded %.1f fps, decode %u us, input drops %u, frame age %.0f ms",
                 (f - lastAlvrFrames) / (now - lastStats), alvr.LastDecodeUs(), alvr.InputDrops(), ageMs);
            lastAlvrFrames = f;
        }
        if (now - lastStats >= 2.0) {
            float yaw, pitch, roll;
            QuatToEuler(q, &yaw, &pitch, &roll);
            LOGI("stats fps=%.1f head yaw=%.1f pitch=%.1f roll=%.1f net=%s rx=%.2fMB "
                 "dec in/out/drop=%u/%u/%u decode=%uus audio pkts=%u underruns=%u dropped=%u peak=%u",
                 framesSinceStats / (now - lastStats), yaw, pitch, roll,
                 net.Connected() ? "up" : "down", net.bytesIn / 1e6, decoder.framesIn.load(),
                 decoder.framesOut.load(), decoder.inputDrops.load(), lastDecodeUs.load(),
                 audio.packets.load(), audio.underruns.load(), audio.dropped.load(),
                 audio.peakLevel.exchange(0));
            lastStats = now;
            framesSinceStats = 0;
        }
    }
};

// Xbox gamepad (paired to the headset): screen controls, forwarded to the PC which owns
// the screen state. B is swallowed so it never acts as BACK and closes the app.
// SteamVR mode: every pad event becomes PC gamepad input (virtual Xbox 360 pad on the PC).
bool ForwardGamepad(App* app, AInputEvent* event) {
    static proto::Gamepad pad = {};
    static const struct { int32_t key; uint32_t bit; } kButtons[] = {
        {AKEYCODE_BUTTON_A, 0}, {AKEYCODE_BUTTON_B, 1}, {AKEYCODE_BUTTON_X, 2},
        {AKEYCODE_BUTTON_Y, 3}, {AKEYCODE_BUTTON_L1, 4}, {AKEYCODE_BUTTON_R1, 5},
        {AKEYCODE_BUTTON_SELECT, 6}, {AKEYCODE_BACK, 6}, {AKEYCODE_BUTTON_START, 7},
        {AKEYCODE_BUTTON_MODE, 8}, {AKEYCODE_BUTTON_THUMBL, 9}, {AKEYCODE_BUTTON_THUMBR, 10}};
    const proto::Gamepad before = pad;
    if (AInputEvent_getType(event) == AINPUT_EVENT_TYPE_KEY) {
        const int32_t code = AKeyEvent_getKeyCode(event);
        const bool down = AKeyEvent_getAction(event) == AKEY_EVENT_ACTION_DOWN;
        bool known = false;
        for (auto& b : kButtons) {
            if (b.key != code) continue;
            known = true;
            pad.buttons = down ? (pad.buttons | (1u << b.bit)) : (pad.buttons & ~(1u << b.bit));
        }
        // Some pads send the D-pad as keys.
        if (code == AKEYCODE_DPAD_LEFT || code == AKEYCODE_DPAD_RIGHT) {
            pad.hatX = down ? (code == AKEYCODE_DPAD_LEFT ? -1 : 1) : 0;
            known = true;
        } else if (code == AKEYCODE_DPAD_UP || code == AKEYCODE_DPAD_DOWN) {
            pad.hatY = down ? (code == AKEYCODE_DPAD_UP ? -1 : 1) : 0;
            known = true;
        }
        if (!known) return false;
    } else if (AInputEvent_getType(event) == AINPUT_EVENT_TYPE_MOTION &&
               (AInputEvent_getSource(event) & AINPUT_SOURCE_JOYSTICK) == AINPUT_SOURCE_JOYSTICK) {
        auto axis = [&](int32_t a) { return AMotionEvent_getAxisValue(event, a, 0); };
        auto s16 = [](float v) { return (int16_t)(v < -1 ? -32768 : v > 1 ? 32767 : v * 32767.0f); };
        auto u8 = [](float v) { return (uint8_t)(v < 0 ? 0 : v > 1 ? 255 : v * 255.0f); };
        pad.lx = s16(axis(AMOTION_EVENT_AXIS_X));
        pad.ly = s16(axis(AMOTION_EVENT_AXIS_Y));
        pad.rx = s16(axis(AMOTION_EVENT_AXIS_Z));
        pad.ry = s16(axis(AMOTION_EVENT_AXIS_RZ));
        const float lt = fmaxf(axis(AMOTION_EVENT_AXIS_LTRIGGER), axis(AMOTION_EVENT_AXIS_BRAKE));
        const float rt = fmaxf(axis(AMOTION_EVENT_AXIS_RTRIGGER), axis(AMOTION_EVENT_AXIS_GAS));
        pad.lt = u8(lt);
        pad.rt = u8(rt);
        const float hx = axis(AMOTION_EVENT_AXIS_HAT_X), hy = axis(AMOTION_EVENT_AXIS_HAT_Y);
        pad.hatX = hx > 0.5f ? 1 : hx < -0.5f ? -1 : 0;
        pad.hatY = hy > 0.5f ? 1 : hy < -0.5f ? -1 : 0;
    } else {
        return false;
    }
    if (memcmp(&before, &pad, sizeof(pad)) != 0) app->net.Send(proto::GAMEPAD, &pad, sizeof(pad));
    return true;
}

int32_t OnInputEvent(android_app* android, AInputEvent* event) {
    App* app = (App*)android->userData;
    if (app->alvr.Streaming() && ForwardGamepad(app, event)) return 1;
    static int hatX = 0, hatY = 0;
    uint32_t cmd = 0;
    if (AInputEvent_getType(event) == AINPUT_EVENT_TYPE_KEY) {
        const int32_t code = AKeyEvent_getKeyCode(event);
        if (AKeyEvent_getAction(event) == AKEY_EVENT_ACTION_DOWN && AKeyEvent_getRepeatCount(event) == 0) {
            switch (code) {
                case AKEYCODE_BUTTON_A: cmd = proto::CTL_RECENTER; break;
                case AKEYCODE_DPAD_RIGHT: cmd = proto::CTL_BIGGER; break;
                case AKEYCODE_DPAD_LEFT: cmd = proto::CTL_SMALLER; break;
                case AKEYCODE_DPAD_UP: cmd = proto::CTL_UP; break;
                case AKEYCODE_DPAD_DOWN: cmd = proto::CTL_DOWN; break;
                case AKEYCODE_BUTTON_L1: cmd = proto::CTL_CLOSER; break;
                case AKEYCODE_BUTTON_R1: cmd = proto::CTL_FARTHER; break;
                case AKEYCODE_BUTTON_Y: cmd = proto::CTL_ENV; break;
                case AKEYCODE_BUTTON_X: cmd = proto::CTL_RESET; break;
                case AKEYCODE_BUTTON_SELECT: cmd = proto::CTL_OVERLAY; break;  // "View"/Back button
                default: break;
            }
            LOGI("input: key %d -> cmd %u", code, cmd);
        }
        if (code == AKEYCODE_BACK || code == AKEYCODE_BUTTON_B || cmd) {
            if (cmd) app->net.Send(proto::CONTROL, &cmd, sizeof(cmd));
            return 1;
        }
        return 0;
    }
    if (AInputEvent_getType(event) == AINPUT_EVENT_TYPE_MOTION &&
        (AInputEvent_getSource(event) & AINPUT_SOURCE_JOYSTICK) == AINPUT_SOURCE_JOYSTICK) {
        // Many pads report the D-pad as a hat axis instead of key events.
        const float hx = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_HAT_X, 0);
        const float hy = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_HAT_Y, 0);
        const int nx = hx > 0.5f ? 1 : (hx < -0.5f ? -1 : 0);
        const int ny = hy > 0.5f ? 1 : (hy < -0.5f ? -1 : 0);
        if (nx != hatX && nx) cmd = nx > 0 ? proto::CTL_BIGGER : proto::CTL_SMALLER;
        if (ny != hatY && ny) cmd = ny > 0 ? proto::CTL_DOWN : proto::CTL_UP;
        hatX = nx;
        hatY = ny;
        if (cmd) {
            LOGI("input: hat -> cmd %u", cmd);
            app->net.Send(proto::CONTROL, &cmd, sizeof(cmd));
        }
        return 1;
    }
    return 0;
}

void OnAppCmd(android_app* android, int32_t cmd) {
    App* app = (App*)android->userData;
    switch (cmd) {
        case APP_CMD_RESUME: app->resumed = true; LOGI("onResume"); break;
        case APP_CMD_PAUSE: app->resumed = false; LOGI("onPause"); break;
        case APP_CMD_INIT_WINDOW: app->window = android->window; LOGI("window created"); break;
        case APP_CMD_TERM_WINDOW: app->window = nullptr; LOGI("window destroyed"); break;
        case APP_CMD_DESTROY: app->window = nullptr; break;
        default: break;
    }
}

}  // namespace

void android_main(android_app* android) {
    LOGI("android_main start");
    App* appPtr = new App();  // large and shared with worker threads: keep off the stack
    App& app = *appPtr;
    app.android = android;
    app.java.Vm = android->activity->vm;
    app.java.Vm->AttachCurrentThread(&app.java.Env, nullptr);
    app.java.ActivityObject = android->activity->clazz;
    prctl(PR_SET_NAME, (long)"GoVR::Main", 0, 0, 0);

    const ovrInitParms initParms = vrapi_DefaultInitParms(&app.java);
    const int32_t initResult = vrapi_Initialize(&initParms);
    if (initResult != VRAPI_INITIALIZE_SUCCESS) {
        LOGE("vrapi_Initialize failed: %d", initResult);
        exit(0);
    }
    LOGI("vrapi initialized, device type %d",
         vrapi_GetSystemPropertyInt(&app.java, VRAPI_SYS_PROP_DEVICE_TYPE));

    if (!app.egl.Create()) exit(0);
    const int ew = vrapi_GetSystemPropertyInt(&app.java, VRAPI_SYS_PROP_SUGGESTED_EYE_TEXTURE_WIDTH);
    const int eh = vrapi_GetSystemPropertyInt(&app.java, VRAPI_SYS_PROP_SUGGESTED_EYE_TEXTURE_HEIGHT);
    LOGI("suggested eye texture %dx%d", ew, eh);
    for (auto& e : app.eyes)
        if (!e.Create(ew, eh)) { LOGE("swapchain creation failed"); exit(0); }
    app.pattern.Create();
    app.osd.Create(app.java.Env, android->activity->clazz);
    app.StartNetwork();
    // SteamVR mode. ~2304x1296 side by side at 72 Hz stays within the Go's decoder budget.
    app.alvr.Init(app.java.Vm, app.java.Env->NewGlobalRef(android->activity->clazz), 1152, 1296);

    android->userData = &app;
    android->onAppCmd = OnAppCmd;
    android->onInputEvent = OnInputEvent;

    while (!android->destroyRequested) {
        for (;;) {
            int events;
            android_poll_source* source;
            const int timeout = (!app.ovr && !android->destroyRequested) ? 100 : 0;
            if (ALooper_pollOnce(timeout, nullptr, &events, (void**)&source) < 0) break;
            if (source) source->process(android, source);
            app.HandleVrModeChanges();
        }
        app.HandleVrApiEvents();
        app.HandleStreamChanges();
        app.alvr.Update(app.java.Env);
        if (!app.ovr) continue;
        app.RenderFrame();
    }

    app.net.Stop();
    app.alvr.Shutdown();
    app.audio.Stop();
    app.DestroyVideoSurface();
    app.pattern.Destroy();
    for (auto& e : app.eyes) e.Destroy();
    app.egl.Destroy();
    vrapi_Shutdown();
    app.java.Vm->DetachCurrentThread();
    delete appPtr;
    LOGI("android_main exit");
}
