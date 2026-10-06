#pragma once

#include <string>
#include <vector>

#include "core/types.h"

namespace nes {

// Plays the APU's samples through ALSA.
//
// Only libasound.so.2 (the runtime library) is guaranteed to be present on
// this system, with no development headers, so the handful of PCM entry points
// we need are resolved with dlopen() at run time. If anything fails, the
// object reports !ok() and the frontend simply falls back to a clock-based
// frame timer -- the emulator keeps running silently.
class AudioOut {
public:
    ~AudioOut() { shutdown(); }

    bool init(int sampleRate, int latencyMs = 70);
    void shutdown();

    // `samples` are mono floats in roughly 0..1. This call blocks once the
    // device buffer is full, which is what paces the whole emulator.
    void write(const float* samples, size_t n);

    // Linear gain on the output, clamped to 0..1. Survives shutdown()/init().
    void  setVolume(float v);
    float volume() const { return volume_; }

    bool ok() const { return pcm_ != nullptr; }
    const std::string& error() const { return err_; }

private:
    void* lib_ = nullptr;      // handle for libasound.so.2
    void* pcm_ = nullptr;      // snd_pcm_t*

    // Resolved entry points.
    int  (*pOpen_)(void** pcm, const char* name, int stream, int mode) = nullptr;
    int  (*pSetParams_)(void* pcm, int format, int access, unsigned channels,
                        unsigned rate, int softResample, unsigned latency) = nullptr;
    long (*pWriteI_)(void* pcm, const void* buffer, unsigned long frames) = nullptr;
    int  (*pRecover_)(void* pcm, int err, int silent) = nullptr;
    int  (*pDrain_)(void* pcm) = nullptr;
    int  (*pClose_)(void* pcm) = nullptr;

    int sampleRate_ = 44100;
    float volume_ = 1.0f;
    std::vector<s16> buf_;     // interleaved stereo scratch
    float dcPrevIn_ = 0.0f, dcPrevOut_ = 0.0f;
    std::string err_;
};

} // namespace nes
