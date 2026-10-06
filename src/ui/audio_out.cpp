#include "ui/audio_out.h"

#include <dlfcn.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace nes {

// Constants from <alsa/pcm.h>. They are part of ALSA's ABI so hard-coding the
// values is safe here -- we cannot include the header, it is not installed.
static constexpr int kStreamPlayback   = 0;
static constexpr int kFormatS16LE      = 2;
static constexpr int kAccessRwInterlv  = 3;

bool AudioOut::init(int sampleRate, int latencyMs) {
    sampleRate_ = sampleRate > 0 ? sampleRate : 44100;

    // Try a few plausible sonames; distributions differ.
    const char* candidates[] = { "libasound.so.2", "libasound.so", "libasound.so.1" };
    for (const char* name : candidates) {
        lib_ = dlopen(name, RTLD_NOW | RTLD_LOCAL);
        if (lib_) break;
    }
    if (!lib_) { err_ = "找不到 ALSA 运行库（libasound.so.2）"; return false; }

    pOpen_      = reinterpret_cast<decltype(pOpen_)>(dlsym(lib_, "snd_pcm_open"));
    pSetParams_ = reinterpret_cast<decltype(pSetParams_)>(dlsym(lib_, "snd_pcm_set_params"));
    pWriteI_    = reinterpret_cast<decltype(pWriteI_)>(dlsym(lib_, "snd_pcm_writei"));
    pRecover_   = reinterpret_cast<decltype(pRecover_)>(dlsym(lib_, "snd_pcm_recover"));
    pDrain_     = reinterpret_cast<decltype(pDrain_)>(dlsym(lib_, "snd_pcm_drain"));
    pClose_     = reinterpret_cast<decltype(pClose_)>(dlsym(lib_, "snd_pcm_close"));

    if (!pOpen_ || !pSetParams_ || !pWriteI_ || !pRecover_) {
        err_ = "ALSA 运行库缺少所需的函数";
        shutdown();
        return false;
    }

    void* pcm = nullptr;
    // "default" lets ALSA route through PulseAudio/PipeWire if it is running.
    int rc = pOpen_(&pcm, "default", kStreamPlayback, 0 /* blocking */);
    if (rc < 0) {
        err_ = "无法打开默认音频设备（可能被占用或没有声卡）";
        shutdown();
        return false;
    }
    pcm_ = pcm;

    const unsigned latencyUs = static_cast<unsigned>(latencyMs) * 1000u;
    rc = pSetParams_(pcm_, kFormatS16LE, kAccessRwInterlv, 2,
                     static_cast<unsigned>(sampleRate_), 1 /* soft resample */, latencyUs);
    if (rc < 0) {
        err_ = "无法配置音频参数（44100Hz / 16bit / 立体声）";
        shutdown();
        return false;
    }

    buf_.reserve(8192);
    return true;
}

void AudioOut::shutdown() {
    if (pcm_ && pDrain_) pDrain_(pcm_);
    if (pcm_ && pClose_) pClose_(pcm_);
    pcm_ = nullptr;
    if (lib_) dlclose(lib_);
    lib_ = nullptr;
}

void AudioOut::setVolume(float v) {
    volume_ = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

void AudioOut::write(const float* samples, size_t n) {
    if (!pcm_ || !pWriteI_ || n == 0) return;

    // The NES output sits at a DC offset; a one-pole high-pass removes it so
    // the speaker is not fed a constant bias.
    static constexpr float kR = 0.9995f;
    buf_.resize(n * 2);
    for (size_t i = 0; i < n; i++) {
        const float in = samples[i];
        const float out = in - dcPrevIn_ + kR * dcPrevOut_;
        dcPrevIn_ = in;
        dcPrevOut_ = out;

        float v = out * 3.0f * volume_;         // bring it up, then the user's level
        v = std::max(-1.0f, std::min(1.0f, v));
        const s16 s = static_cast<s16>(v * 32767.0f);
        buf_[i * 2 + 0] = s;
        buf_[i * 2 + 1] = s;                    // mono duplicated to both channels
    }

    size_t written = 0;
    while (written < n) {
        // snd_pcm_writei counts frames; one frame here is one stereo sample.
        long rc = pWriteI_(pcm_, buf_.data() + written * 2,
                           static_cast<unsigned long>(n - written));
        if (rc < 0) {
            rc = pRecover_(pcm_, static_cast<int>(rc), 1);
            if (rc < 0) return;                 // device went away; stay silent
            continue;
        }
        written += static_cast<size_t>(rc);
    }
}

} // namespace nes
