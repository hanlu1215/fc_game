#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

using u8  = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using s8  = int8_t;
using s16 = int16_t;
using s32 = int32_t;

// ---------------------------------------------------------------------------
// NES NTSC timing constants
// ---------------------------------------------------------------------------
namespace nes {

constexpr u32 CPU_CLOCK   = 1789773;   // 2A03 CPU clock (NTSC)
constexpr double FPS      = 60.0988;   // frame rate (NTSC)
constexpr int  SCANLINES  = 262;       // PPU scanlines per frame
constexpr int  DOTS       = 341;       // PPU dots per scanline
constexpr int  VISIBLE_H  = 240;       // visible scanlines
constexpr int  WIDTH      = 256;
constexpr int  HEIGHT     = 240;
constexpr int  AUDIO_RATE = 44100;

// 6502 status register flags
enum Flag : u8 {
    C = 0x01,   // carry
    Z = 0x02,   // zero
    I = 0x04,   // interrupt disable
    D = 0x08,   // decimal (unused on NES)
    B = 0x10,   // break
    U = 0x20,   // unused, always 1
    V = 0x40,   // overflow
    N = 0x80,   // negative
};

// Nametable mirroring modes
enum class Mirroring { Horizontal, Vertical, FourScreen, SingleLower, SingleUpper };

// Bit helpers
template <typename T> constexpr bool getBit(T v, int b) { return (v >> b) & 1; }
template <typename T> constexpr T setBit(T v, int b, bool on) {
    return on ? static_cast<T>(v | (static_cast<T>(1) << b))
              : static_cast<T>(v & ~(static_cast<T>(1) << b));
}

// ---------------------------------------------------------------------------
// Save-state serialisation helpers.
// A StateWriter/StateReader pair keeps every subsystem's save/load code short
// and guarantees the same byte layout is used in both directions.
// ---------------------------------------------------------------------------
class StateWriter {
public:
    std::vector<u8> data;

    void raw(const void* p, size_t n) {
        const u8* b = static_cast<const u8*>(p);
        data.insert(data.end(), b, b + n);
    }
    template <typename T> void pod(const T& v) { raw(&v, sizeof(T)); }
    void boolean(bool v) { u8 b = v ? 1 : 0; raw(&b, 1); }
    void str(const std::string& s) {
        u32 n = static_cast<u32>(s.size());
        pod(n);
        raw(s.data(), n);
    }
    // Blob: length-prefixed byte buffer (used for RAM/VRAM arrays).
    void blob(const u8* p, size_t n) {
        u32 len = static_cast<u32>(n);
        pod(len);
        raw(p, n);
    }
};

class StateReader {
public:
    const u8* ptr = nullptr;
    size_t    remaining = 0;
    bool      failed = false;

    StateReader(const u8* p, size_t n) : ptr(p), remaining(n) {}

    bool ok() const { return !failed; }
    void raw(void* out, size_t n) {
        if (remaining < n) { failed = true; memset(out, 0, n); return; }
        memcpy(out, ptr, n);
        ptr += n;
        remaining -= n;
    }
    template <typename T> void pod(T& v) { raw(&v, sizeof(T)); }
    void boolean(bool& v) { u8 b = 0; raw(&b, 1); v = (b != 0); }
    void str(std::string& s) {
        u32 n = 0;
        pod(n);
        if (failed || n > remaining) { failed = true; s.clear(); return; }
        s.assign(reinterpret_cast<const char*>(ptr), n);
        ptr += n;
        remaining -= n;
    }
    void blob(u8* out, size_t n) {
        u32 len = 0;
        pod(len);
        if (failed) { memset(out, 0, n); return; }
        if (len != n) { failed = true; memset(out, 0, n); return; }
        raw(out, n);
    }
};

} // namespace nes
