#include "audio.h"

#include <opus.h>

#include <cstring>

#include "common.h"

namespace {
// Jitter buffer, in 10 ms chunks: start playing at 2, trim back above 5. TCP over USB
// delivers packets evenly, so a small cushion is enough.
constexpr size_t kPrimeChunks = 2;
constexpr size_t kMaxChunks = 5;
}  // namespace

bool AudioPlayer::Start(int sampleRate, int channels, int frameMs, PlayedCallback cb) {
    Stop();
    rate_ = sampleRate;
    channels_ = channels;
    frameSamples_ = sampleRate * frameMs / 1000;
    callback_ = std::move(cb);
    int err = 0;
    opus_ = opus_decoder_create(rate_, channels_, &err);
    if (err != OPUS_OK) { LOGE("audio: opus_decoder_create failed %d", err); return false; }
    decodeBuf_.resize(5760 * channels_);  // 120 ms at 48 kHz, Opus maximum

    SLresult r = slCreateEngine(&engineObj_, 0, nullptr, 0, nullptr, nullptr);
    if (r == SL_RESULT_SUCCESS) r = (*engineObj_)->Realize(engineObj_, SL_BOOLEAN_FALSE);
    if (r == SL_RESULT_SUCCESS) r = (*engineObj_)->GetInterface(engineObj_, SL_IID_ENGINE, &engine_);
    if (r == SL_RESULT_SUCCESS) r = (*engine_)->CreateOutputMix(engine_, &mixObj_, 0, nullptr, nullptr);
    if (r == SL_RESULT_SUCCESS) r = (*mixObj_)->Realize(mixObj_, SL_BOOLEAN_FALSE);
    if (r != SL_RESULT_SUCCESS) { LOGE("audio: OpenSL engine setup failed %u", r); Stop(); return false; }

    SLDataLocator_AndroidSimpleBufferQueue locBq = {SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, 2};
    SLDataFormat_PCM fmt = {SL_DATAFORMAT_PCM, (SLuint32)channels_, (SLuint32)rate_ * 1000,
                            SL_PCMSAMPLEFORMAT_FIXED_16, SL_PCMSAMPLEFORMAT_FIXED_16,
                            channels_ == 2 ? (SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT)
                                           : SL_SPEAKER_FRONT_CENTER,
                            SL_BYTEORDER_LITTLEENDIAN};
    SLDataSource src = {&locBq, &fmt};
    SLDataLocator_OutputMix locMix = {SL_DATALOCATOR_OUTPUTMIX, mixObj_};
    SLDataSink sink = {&locMix, nullptr};
    const SLInterfaceID ids[] = {SL_IID_ANDROIDSIMPLEBUFFERQUEUE, SL_IID_ANDROIDCONFIGURATION};
    const SLboolean req[] = {SL_BOOLEAN_TRUE, SL_BOOLEAN_FALSE};
    r = (*engine_)->CreateAudioPlayer(engine_, &player_, &src, &sink, 2, ids, req);
    if (r != SL_RESULT_SUCCESS) { LOGE("audio: CreateAudioPlayer failed %u", r); Stop(); return false; }
    SLAndroidConfigurationItf config;
    if ((*player_)->GetInterface(player_, SL_IID_ANDROIDCONFIGURATION, &config) == SL_RESULT_SUCCESS) {
        SLint32 stream = SL_ANDROID_STREAM_MEDIA;
        (*config)->SetConfiguration(config, SL_ANDROID_KEY_STREAM_TYPE, &stream, sizeof(stream));
        SLuint32 perf = SL_ANDROID_PERFORMANCE_LATENCY;  // API 25: request the fast track
        (*config)->SetConfiguration(config, SL_ANDROID_KEY_PERFORMANCE_MODE, &perf, sizeof(perf));
    }
    r = (*player_)->Realize(player_, SL_BOOLEAN_FALSE);
    if (r == SL_RESULT_SUCCESS) r = (*player_)->GetInterface(player_, SL_IID_PLAY, &play_);
    if (r == SL_RESULT_SUCCESS)
        r = (*player_)->GetInterface(player_, SL_IID_ANDROIDSIMPLEBUFFERQUEUE, &queue_);
    if (r == SL_RESULT_SUCCESS) r = (*queue_)->RegisterCallback(queue_, BufferCallback, this);
    if (r != SL_RESULT_SUCCESS) { LOGE("audio: player setup failed %u", r); Stop(); return false; }

    for (auto& b : outBuf_) b.assign(frameSamples_ * channels_, 0);
    priming_ = true;
    underruns = dropped = packets = 0;
    // Keep two buffers in flight; the callback refills each one as it completes.
    for (int i = 0; i < 2; i++)
        (*queue_)->Enqueue(queue_, outBuf_[i].data(), outBuf_[i].size() * sizeof(int16_t));
    (*play_)->SetPlayState(play_, SL_PLAYSTATE_PLAYING);
    LOGI("audio: OpenSL ES player started %d Hz x%d, %d-sample buffers", rate_, channels_, frameSamples_);
    return true;
}

void AudioPlayer::Stop() {
    if (play_) (*play_)->SetPlayState(play_, SL_PLAYSTATE_STOPPED);
    if (player_) (*player_)->Destroy(player_);
    if (mixObj_) (*mixObj_)->Destroy(mixObj_);
    if (engineObj_) (*engineObj_)->Destroy(engineObj_);
    player_ = mixObj_ = engineObj_ = nullptr;
    engine_ = nullptr;
    play_ = nullptr;
    queue_ = nullptr;
    if (opus_) opus_decoder_destroy(opus_);
    opus_ = nullptr;
    std::lock_guard<std::mutex> lock(mutex_);
    chunks_.clear();
}

void AudioPlayer::PushOpus(const uint8_t* data, size_t size, uint64_t pcTimeNs) {
    if (!opus_) return;
    const int n = opus_decode(opus_, data, (opus_int32)size, decodeBuf_.data(),
                              (int)decodeBuf_.size() / channels_, 0);
    if (n <= 0) { LOGW("audio: opus_decode error %d", n); return; }
    packets++;
    int peak = 0;
    for (int i = 0; i < n * channels_; i++) {
        const int v = decodeBuf_[i] < 0 ? -decodeBuf_[i] : decodeBuf_[i];
        if (v > peak) peak = v;
    }
    if (peak > (int)peakLevel) peakLevel = peak;
    std::lock_guard<std::mutex> lock(mutex_);
    // Split into output-buffer-sized chunks (packets are normally exactly one chunk).
    for (int off = 0; off + frameSamples_ <= n; off += frameSamples_) {
        Chunk c;
        c.pcm.assign(decodeBuf_.begin() + off * channels_,
                     decodeBuf_.begin() + (off + frameSamples_) * channels_);
        c.pcTimeNs = pcTimeNs + (uint64_t)off * 1000000000ULL / rate_;
        chunks_.push_back(std::move(c));
    }
    while (chunks_.size() > kMaxChunks) {  // clock drift or a burst: trim latency back
        chunks_.pop_front();
        dropped++;
    }
}

void AudioPlayer::BufferCallback(SLAndroidSimpleBufferQueueItf, void* ctx) {
    ((AudioPlayer*)ctx)->FillNext();
}

void AudioPlayer::FillNext() {
    std::vector<int16_t>& out = outBuf_[outIndex_];
    outIndex_ ^= 1;
    uint64_t pcTime = 0;
    uint32_t bufferedMs = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (priming_ && chunks_.size() >= kPrimeChunks) priming_ = false;
        if (!priming_ && !chunks_.empty()) {
            memcpy(out.data(), chunks_.front().pcm.data(), out.size() * sizeof(int16_t));
            pcTime = chunks_.front().pcTimeNs;
            chunks_.pop_front();
        } else {
            memset(out.data(), 0, out.size() * sizeof(int16_t));
            if (!priming_) {  // ran dry: count it and rebuild a cushion
                underruns++;
                priming_ = true;
            }
        }
        bufferedMs = (uint32_t)(chunks_.size() * frameSamples_ * 1000 / rate_);
    }
    (*queue_)->Enqueue(queue_, out.data(), out.size() * sizeof(int16_t));
    if (pcTime && callback_) callback_(pcTime, bufferedMs);
}
