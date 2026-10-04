#pragma once

#include <functional>
#include <vector>

#include "core/types.h"

namespace nes {

// Ricoh 2A03 audio processing unit.
//
// Two pulse channels, a triangle, a noise channel and the DMC sample player,
// plus the frame counter that can raise an IRQ. The frame counter and the
// $4015 status byte are not cosmetic: several games use them as their only
// timebase and will hang if they are missing.
class APU {
public:
    APU();

    void reset();
    void setSampleRate(int hz);

    // The DMC fetches its sample bytes through the CPU bus.
    void setMemoryReader(std::function<u8(u16)> fn) { memRead = std::move(fn); }

    void step(u32 cpuCycles);

    u8   readRegister(u16 addr);
    void writeRegister(u16 addr, u8 value);

    // Wired-OR into the CPU's IRQ line alongside the mapper's.
    bool irqActive() const { return frameIrqFlag || dmcIrqFlag; }

    // Moves whatever has been generated since the last call into `out`.
    void drainSamples(std::vector<float>& out);

    void serialize(StateWriter& w) const;
    void deserialize(StateReader& r);

private:
    // ---------------------------------------------------------------- pulse
    struct Pulse {
        bool enabled = false;
        u8 duty = 0, dutyPos = 0;
        u8 lengthHalt = 0, constVolume = 0, volume = 0;
        u8 envDivider = 0, envDecay = 0;
        bool envStart = false;
        u16 timerPeriod = 0, timerCounter = 0;
        u8 lengthCounter = 0;
        bool sweepEnabled = false, sweepNegate = false, sweepReload = false;
        u8 sweepPeriod = 0, sweepShift = 0, sweepDivider = 0;
        bool isPulse2 = false;
    };

    // ------------------------------------------------------------- triangle
    struct Triangle {
        bool enabled = false;
        u8 lengthCounter = 0;
        bool controlFlag = false, linearReload = false;
        u8 linearCounter = 0, linearReloadValue = 0;
        u16 timerPeriod = 0, timerCounter = 0;
        u8 sequencePos = 0;
    };

    struct Noise {
        bool enabled = false;
        u8 lengthHalt = 0, constVolume = 0, volume = 0;
        u8 envDivider = 0, envDecay = 0;
        bool envStart = false;
        u8 lengthCounter = 0;
        bool mode = false;
        u16 timerPeriod = 0, timerCounter = 0;
        u16 shiftRegister = 1;
    };

    struct Dmc {
        bool irqEnable = false, loop = false;
        u8 rateIndex = 0;
        u16 timerPeriod = 0, timerCounter = 0;
        u16 sampleAddr = 0xC000, currentAddr = 0xC000;
        u16 sampleLength = 0, bytesRemaining = 0;
        u8 sampleBuffer = 0;
        bool bufferEmpty = true;
        u8 shiftRegister = 0;
        u8 bitsRemaining = 8;
        bool silence = true;
        u8 outputLevel = 0;
        bool dmcIrq = false;
        bool restart = false;
    };

    Pulse pulse1, pulse2;
    Triangle triangle;
    Noise noise;
    Dmc dmc;

    // ------------------------------------------------------- frame counter
    bool mode5 = false;
    bool irqInhibit = false;
    bool frameIrqFlag = false;
    bool dmcIrqFlag = false;
    u32 frameCycle = 0;

    // ------------------------------------------------------------ sampling
    int sampleRate = 44100;
    double cyclesPerSample = 0;
    double sampleAccum = 0;
    double mixAccum = 0;
    int mixCount = 0;
    std::vector<float> samples;

    std::function<u8(u16)> memRead;

    // ------------------------------------------------------------ internals
    bool apuPhase = false;      // APU cycles are half the CPU rate

    void stepOneCycle();
    void clockFrameCounter();
    void clockPulse(Pulse& p);
    void clockTriangle();
    void clockNoise();
    void clockDmc();
    // Quarter-frame (240 Hz) and half-frame (120 Hz) work. Envelopes and the
    // triangle's linear counter belong here, NOT on the APU clock -- clocking
    // them per APU cycle drains them in microseconds and silences the channel.
    void clockQuarterFrame();
    void clockHalfFrame();
    void clockEnvelope(Pulse& p);
    void clockEnvelopeNoise();
    void clockSweep(Pulse& p);
    void clockLengthCounters();

    int  pulseOutput(const Pulse& p) const;
    int  triangleOutput() const;
    int  noiseOutput() const;
    int  dmcOutput() const;
    double mix() const;

    friend struct ApuStateAccess;
};

} // namespace nes
