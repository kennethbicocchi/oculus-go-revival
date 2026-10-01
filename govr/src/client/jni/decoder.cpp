#include "decoder.h"

#include <media/NdkMediaFormat.h>

#include <cstring>

#include "common.h"

bool VideoDecoder::Start(ANativeWindow* window, int codec, int width, int height, FrameCallback cb) {
    Stop();
    const char* mime = codec == 1 ? "video/hevc" : "video/avc";
    codec_ = AMediaCodec_createDecoderByType(mime);
    if (!codec_) {
        LOGE("decoder: no decoder for %s", mime);
        return false;
    }
    AMediaFormat* fmt = AMediaFormat_new();
    AMediaFormat_setString(fmt, AMEDIAFORMAT_KEY_MIME, mime);
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_WIDTH, width);
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_HEIGHT, height);
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_MAX_INPUT_SIZE, width * height);
    // Low-latency hints. Qualcomm (MSM8996) honours "picture order" = decode order output,
    // which removes the decoder's reorder buffering. Unknown keys are ignored.
    AMediaFormat_setInt32(fmt, "vendor.qti-ext-dec-picture-order.enable", 1);
    AMediaFormat_setInt32(fmt, "vendor.qti-ext-dec-low-latency.enable", 1);
    AMediaFormat_setInt32(fmt, "low-latency", 1);
    AMediaFormat_setInt32(fmt, "priority", 0);
    AMediaFormat_setInt32(fmt, "operating-rate", 120);
    media_status_t st = AMediaCodec_configure(codec_, fmt, window, nullptr, 0);
    AMediaFormat_delete(fmt);
    if (st != AMEDIA_OK) {
        LOGE("decoder: configure failed %d", st);
        AMediaCodec_delete(codec_);
        codec_ = nullptr;
        return false;
    }
    if ((st = AMediaCodec_start(codec_)) != AMEDIA_OK) {
        LOGE("decoder: start failed %d", st);
        AMediaCodec_delete(codec_);
        codec_ = nullptr;
        return false;
    }
    callback_ = std::move(cb);
    framesIn = framesOut = inputDrops = 0;
    running_ = true;
    outputThread_ = std::thread(&VideoDecoder::OutputLoop, this);
    LOGI("decoder: started %s %dx%d", mime, width, height);
    return true;
}

void VideoDecoder::Stop() {
    if (!codec_) return;
    running_ = false;
    if (outputThread_.joinable()) outputThread_.join();
    AMediaCodec_stop(codec_);
    AMediaCodec_delete(codec_);
    codec_ = nullptr;
    std::lock_guard<std::mutex> lock(timesMutex_);
    submitTimes_.clear();
    LOGI("decoder: stopped");
}

bool VideoDecoder::Submit(const uint8_t* data, size_t size, uint64_t pcTimeNs, int64_t recvNs) {
    if (!codec_) return false;
    const ssize_t idx = AMediaCodec_dequeueInputBuffer(codec_, 20000);
    if (idx < 0) {
        inputDrops++;
        return false;
    }
    size_t cap = 0;
    uint8_t* buf = AMediaCodec_getInputBuffer(codec_, idx, &cap);
    if (!buf || cap < size) {
        LOGE("decoder: input buffer too small (%zu < %zu)", cap, size);
        AMediaCodec_queueInputBuffer(codec_, idx, 0, 0, 0, 0);
        inputDrops++;
        return false;
    }
    memcpy(buf, data, size);
    // Unique, monotonic presentation time: the PC timestamp in microseconds.
    const int64_t ptsUs = (int64_t)(pcTimeNs / 1000);
    {
        std::lock_guard<std::mutex> lock(timesMutex_);
        submitTimes_[ptsUs] = {pcTimeNs, recvNs, NowNanos()};
    }
    AMediaCodec_queueInputBuffer(codec_, idx, 0, size, ptsUs, 0);
    framesIn++;
    return true;
}

void VideoDecoder::OutputLoop() {
    while (running_) {
        AMediaCodecBufferInfo info;
        const ssize_t idx = AMediaCodec_dequeueOutputBuffer(codec_, &info, 10000);
        if (idx >= 0) {
            AMediaCodec_releaseOutputBuffer(codec_, idx, info.size != 0);
            framesOut++;
            uint64_t pcTimeNs = 0;
            uint32_t decodeUs = 0, onGoUs = 0;
            {
                std::lock_guard<std::mutex> lock(timesMutex_);
                auto it = submitTimes_.find(info.presentationTimeUs);
                if (it != submitTimes_.end()) {
                    const int64_t now = NowNanos();
                    pcTimeNs = it->second.pcTimeNs;
                    decodeUs = (uint32_t)((now - it->second.submitNs) / 1000);
                    onGoUs = (uint32_t)((now - it->second.recvNs) / 1000);
                    submitTimes_.erase(it);
                }
                if (submitTimes_.size() > 256) submitTimes_.clear();  // lost frames
            }
            if (callback_ && pcTimeNs) callback_(pcTimeNs, decodeUs, onGoUs);
        } else if (idx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            AMediaFormat* f = AMediaCodec_getOutputFormat(codec_);
            LOGI("decoder: output format %s", AMediaFormat_toString(f));
            AMediaFormat_delete(f);
        }
    }
}
