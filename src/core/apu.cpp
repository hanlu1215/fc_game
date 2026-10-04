#include "core/apu.h"

namespace nes {

// Duty cycles for 12.5%, 25%, 50% and 75% (the last is the 25% sequence
// inverted, which is how the hardware produces it).
static const u8 kDuty[4][8] = {
    {0, 1, 0, 0, 0, 0, 0, 0},
    {0, 1, 1, 0, 0, 0, 0, 0},
    {0, 1, 1, 1, 1, 0, 0, 0},
    {1, 0, 0, 1, 1, 1, 1, 1},
};

// Length counter values. These are the hardware values plus one, so a channel
// is silenced when the counter reaches zero rather than when it is clocked at
// zero.
static const u8 kLengthTable[32] = {
    10, 254, 20,   2, 40,   4, 80,   6, 160,   8, 60,  10, 14,  12, 26, 14,
    12,  16, 24,  18, 48,  20, 96,  22, 192,  24, 72,  26, 16,  28, 32, 30,
};

static const u16 kNoisePeriod[16] = {
    4, 8, 16, 32, 64, 96, 128, 160, 202, 254, 380, 508, 762, 1016, 2034, 4068,
};

static const u16 kDmcRate[16] = {
    428, 380, 340, 320, 286, 254, 226, 214, 190, 160, 142, 128, 106, 84, 72, 54,
};

// The triangle walks 15..0 then 0..15.
static const u8 kTriangleSeq[32] = {
    15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0,
     0,  1,  2,  3,  4,  5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
};

// ---------------------------------------------------------------------------
APU::APU() {
    pulse2.isPulse2 = true;     // pulse 2 negates its sweep differently
    reset();
}

void APU::setSampleRate(int hz) {
    sampleRate = hz > 0 ? hz : 44100;
    const double apuCyclesPerSecond = static_cast<double>(CPU_CLOCK) / 2.0;
    cyclesPerSample = apuCyclesPerSecond / sampleRate;
}

void APU::reset() {
    pulse1 = Pulse();
    pulse2 = Pulse();
    pulse2.isPulse2 = true;
    triangle = Triangle();
    noise = Noise();
    dmc = Dmc();

    mode5 = false;
    irqInhibit = false;
    frameIrqFlag = false;
    dmcIrqFlag = false;
    frameCycle = 0;

    apuPhase = false;
    sampleAccum = 0;
    mixAccum = 0;
    mixCount = 0;
    samples.clear();
    setSampleRate(sampleRate);
}

// ---------------------------------------------------------------------------
// Frame counter. Clocked once per APU cycle (every two CPU cycles).
// ---------------------------------------------------------------------------
void APU::clockFrameCounter() {
    frameCycle++;
    if (mode5) {
        if (frameCycle == 3728)       { /* quarter */ }
        else if (frameCycle == 7456)  { /* quarter + half */ }
        else if (frameCycle == 11185) { /* quarter */ }
        else if (frameCycle == 18640) { /* quarter + half */ }
        else if (frameCycle == 18641) { frameCycle = 0; return; }
        else return;

        if (frameCycle == 3728 || frameCycle == 11185) clockQuarterFrame();
        if (frameCycle == 7456 || frameCycle == 18640) { clockQuarterFrame(); clockHalfFrame(); }
    } else {
        if (frameCycle == 3728)       { /* quarter */ }
        else if (frameCycle == 7456)  { /* quarter + half */ }
        else if (frameCycle == 11185) { /* quarter */ }
        else if (frameCycle == 14914) {
            if (!irqInhibit) frameIrqFlag = true;
        }
        else if (frameCycle == 14915) { frameCycle = 0; return; }
        else return;

        if (frameCycle == 3728 || frameCycle == 11185) clockQuarterFrame();
        if (frameCycle == 7456 || frameCycle == 14914) { clockQuarterFrame(); clockHalfFrame(); }
    }
}

void APU::clockLengthCounters() {
    if (!pulse1.lengthHalt && pulse1.lengthCounter > 0) pulse1.lengthCounter--;
    if (!pulse2.lengthHalt && pulse2.lengthCounter > 0) pulse2.lengthCounter--;
    if (!noise.lengthHalt  && noise.lengthCounter  > 0) noise.lengthCounter--;
    // The triangle's halt is the control flag.
    if (!triangle.controlFlag && triangle.lengthCounter > 0) triangle.lengthCounter--;
}

// ---------------------------------------------------------------------------
void APU::clockEnvelope(Pulse& p) {
    if (p.envStart) {
        p.envStart = false;
        p.envDecay = 15;
        p.envDivider = p.volume;
    } else if (p.envDivider-- == 0) {
        p.envDivider = p.volume;
        if (p.envDecay > 0) p.envDecay--;
        else if (p.lengthHalt) p.envDecay = 15;   // loop
    }
}

void APU::clockSweep(Pulse& p) {
    const int delta = p.timerPeriod >> p.sweepShift;
    const int target = p.sweepNegate ? p.timerPeriod - delta - (p.isPulse2 ? 0 : 1)
                                     : p.timerPeriod + delta;

    if (p.sweepDivider == 0 && p.sweepEnabled && p.sweepShift > 0
        && p.timerPeriod >= 8 && target < 0x800) {
        p.timerPeriod = static_cast<u16>(target);
    }
    if (p.sweepDivider == 0 || p.sweepReload) {
        p.sweepDivider = p.sweepPeriod;
        p.sweepReload = false;
    } else {
        p.sweepDivider--;
    }
}

void APU::clockPulse(Pulse& p) {
    // Timer and duty sequencer only; envelope and sweep run on the frame
    // counter's quarter/half frame clocks.
    if (p.timerCounter == 0) {
        p.timerCounter = p.timerPeriod;
        p.dutyPos = static_cast<u8>((p.dutyPos + 1) & 7);
    } else {
        p.timerCounter--;
    }
}

void APU::clockTriangle() {
    if (triangle.timerCounter == 0) {
        triangle.timerCounter = triangle.timerPeriod;
        if (triangle.lengthCounter > 0 && triangle.linearCounter > 0)
            triangle.sequencePos = static_cast<u8>((triangle.sequencePos + 1) & 31);
    } else {
        triangle.timerCounter--;
    }
    // The linear counter is clocked by the quarter frame clock, see
    // clockQuarterFrame().
}

void APU::clockNoise() {
    if (noise.timerCounter == 0) {
        noise.timerCounter = noise.timerPeriod;
        const u16 bit = noise.mode ? static_cast<u16>((noise.shiftRegister >> 6) & 1)
                                   : static_cast<u16>((noise.shiftRegister >> 1) & 1);
        const u16 fb = static_cast<u16>((noise.shiftRegister ^ bit) & 1);
        noise.shiftRegister = static_cast<u16>((noise.shiftRegister >> 1) | (fb << 14));
    } else {
        noise.timerCounter--;
    }
    // Envelope is clocked by the quarter frame clock, see clockQuarterFrame().
}

void APU::clockEnvelopeNoise() {
    if (noise.envStart) {
        noise.envStart = false;
        noise.envDecay = 15;
        noise.envDivider = noise.volume;
    } else if (noise.envDivider-- == 0) {
        noise.envDivider = noise.volume;
        if (noise.envDecay > 0) noise.envDecay--;
        else if (noise.lengthHalt) noise.envDecay = 15;
    }
}

// 240 Hz: envelopes and the triangle's linear counter.
void APU::clockQuarterFrame() {
    clockEnvelope(pulse1);
    clockEnvelope(pulse2);
    clockEnvelopeNoise();

    if (triangle.linearReload) triangle.linearCounter = triangle.linearReloadValue;
    else if (triangle.linearCounter > 0) triangle.linearCounter--;
    if (!triangle.controlFlag) triangle.linearReload = false;
}

// 120 Hz: length counters and sweep units.
void APU::clockHalfFrame() {
    clockLengthCounters();
    clockSweep(pulse1);
    clockSweep(pulse2);
}

void APU::clockDmc() {
    if (dmc.timerCounter == 0) {
        dmc.timerCounter = dmc.timerPeriod;
        if (!dmc.silence) {
            // Advance through the 8 bits of the buffered sample byte.
            if (dmc.shiftRegister & 1) {
                if (dmc.outputLevel <= 125) dmc.outputLevel += 2;
            } else {
                if (dmc.outputLevel >= 2) dmc.outputLevel -= 2;
            }
            dmc.shiftRegister >>= 1;
            if (--dmc.bitsRemaining == 0) {
                dmc.bitsRemaining = 8;
                if (dmc.bufferEmpty) {
                    dmc.silence = true;
                } else {
                    dmc.silence = false;
                    dmc.shiftRegister = dmc.sampleBuffer;
                    dmc.bufferEmpty = true;
                }
            }
        }
    } else {
        dmc.timerCounter--;
    }

    // Refill the buffer; each fetch steals a CPU cycle on real hardware, which
    // we do not model, but the address/length bookkeeping is what games poll.
    if (dmc.bufferEmpty && dmc.bytesRemaining > 0 && memRead) {
        dmc.sampleBuffer = memRead(dmc.currentAddr);
        dmc.bufferEmpty = false;
        dmc.currentAddr = static_cast<u16>(dmc.currentAddr == 0xFFFF ? 0x8000 : dmc.currentAddr + 1);
        if (--dmc.bytesRemaining == 0) {
            if (dmc.loop) {
                dmc.currentAddr = dmc.sampleAddr;
                dmc.bytesRemaining = dmc.sampleLength;
            } else if (dmc.irqEnable) {
                dmc.dmcIrq = true;
            }
        }
    }
}

void APU::stepOneCycle() {
    apuPhase = !apuPhase;
    if (!apuPhase) return;      // channels advance once per APU cycle

    clockPulse(pulse1);
    clockPulse(pulse2);
    clockTriangle();
    clockNoise();
    clockDmc();
    clockFrameCounter();

    // Box-filter down to the output rate: average the mixer over one output
    // sample period, which suppresses the worst aliasing from noise/triangle.
    sampleAccum += 1.0;
    mixAccum += mix();
    mixCount++;
    if (sampleAccum >= cyclesPerSample) {
        sampleAccum -= cyclesPerSample;
        const float s = static_cast<float>(mixAccum / (mixCount > 0 ? mixCount : 1));
        samples.push_back(s);
        mixAccum = 0;
        mixCount = 0;
    }
}

void APU::step(u32 cpuCycles) {
    for (u32 i = 0; i < cpuCycles; i++) stepOneCycle();
}

void APU::drainSamples(std::vector<float>& out) {
    out.insert(out.end(), samples.begin(), samples.end());
    samples.clear();
}

// ---------------------------------------------------------------------------
// Channel outputs
// ---------------------------------------------------------------------------
int APU::pulseOutput(const Pulse& p) const {
    if (!p.enabled || p.lengthCounter == 0) return 0;
    if (p.timerPeriod < 8) return 0;
    // Sweep can mute by driving the target period out of range.
    const int delta = p.timerPeriod >> p.sweepShift;
    const int target = p.sweepNegate ? p.timerPeriod - delta - (p.isPulse2 ? 0 : 1)
                                     : p.timerPeriod + delta;
    if (p.sweepEnabled && p.sweepShift > 0 && target >= 0 && target > 0x7FF) return 0;
    if (!kDuty[p.duty][p.dutyPos]) return 0;
    return p.constVolume ? p.volume : p.envDecay;
}

int APU::triangleOutput() const {
    if (!triangle.enabled || triangle.lengthCounter == 0) return 0;
    if (triangle.linearCounter == 0) return 0;
    return kTriangleSeq[triangle.sequencePos];
}

int APU::noiseOutput() const {
    if (!noise.enabled || noise.lengthCounter == 0) return 0;
    if (noise.shiftRegister & 1) return 0;      // LFSR bit 0 set = silence
    return noise.constVolume ? noise.volume : noise.envDecay;
}

int APU::dmcOutput() const {
    return dmc.outputLevel;
}

double APU::mix() const {
    const int p = pulseOutput(pulse1) + pulseOutput(pulse2);
    const double pulseOut = (p > 0) ? 95.88 / (8128.0 / p + 100.0) : 0.0;

    const double tndIn = triangleOutput() / 8227.0
                       + noiseOutput() / 12241.0
                       + dmcOutput() / 22638.0;
    const double tndOut = (tndIn > 0.0) ? 159.79 / (1.0 / tndIn + 100.0) : 0.0;

    // The standard non-linear mixer already lands in roughly 0..1. The DC
    // offset is removed later, by the audio output stage.
    return pulseOut + tndOut;
}

// ---------------------------------------------------------------------------
// Registers
// ---------------------------------------------------------------------------
u8 APU::readRegister(u16 addr) {
    if (addr == 0x4015) {
        u8 result = 0;
        if (pulse1.lengthCounter   > 0) result |= 0x01;
        if (pulse2.lengthCounter   > 0) result |= 0x02;
        if (triangle.lengthCounter > 0) result |= 0x04;
        if (noise.lengthCounter    > 0) result |= 0x08;
        if (dmc.bytesRemaining     > 0) result |= 0x10;
        if (frameIrqFlag) result |= 0x40;
        if (dmc.dmcIrq)   result |= 0x80;

        frameIrqFlag = false;       // reading acknowledges the frame IRQ
        return result;
    }
    return 0;
}

void APU::writeRegister(u16 addr, u8 value) {
    switch (addr) {
        // ---------------------------------------------------------- pulse 1
        case 0x4000: case 0x4004: {
            Pulse& p = (addr == 0x4000) ? pulse1 : pulse2;
            p.duty = static_cast<u8>((value >> 6) & 3);
            p.lengthHalt = (value >> 5) & 1;
            p.constVolume = (value >> 4) & 1;
            p.volume = static_cast<u8>(value & 0x0F);
            break;
        }
        case 0x4001: case 0x4005: {
            Pulse& p = (addr == 0x4001) ? pulse1 : pulse2;
            p.sweepEnabled = (value & 0x80) != 0;
            p.sweepPeriod = static_cast<u8>((value >> 4) & 7);
            p.sweepNegate = (value & 0x08) != 0;
            p.sweepShift = static_cast<u8>(value & 7);
            p.sweepReload = true;
            break;
        }
        case 0x4002: case 0x4006: {
            Pulse& p = (addr == 0x4002) ? pulse1 : pulse2;
            p.timerPeriod = static_cast<u16>((p.timerPeriod & 0x700) | value);
            break;
        }
        case 0x4003: case 0x4007: {
            Pulse& p = (addr == 0x4003) ? pulse1 : pulse2;
            p.timerPeriod = static_cast<u16>((p.timerPeriod & 0x0FF) | ((value & 7) << 8));
            if (p.enabled) p.lengthCounter = kLengthTable[value >> 3];
            p.dutyPos = 0;
            p.envStart = true;      // writing the length reloads the envelope
            break;
        }

        // -------------------------------------------------------- triangle
        case 0x4008:
            triangle.controlFlag = (value & 0x80) != 0;
            triangle.linearReloadValue = static_cast<u8>(value & 0x7F);
            break;
        case 0x400A:
            triangle.timerPeriod = static_cast<u16>((triangle.timerPeriod & 0x700) | value);
            break;
        case 0x400B:
            triangle.timerPeriod = static_cast<u16>((triangle.timerPeriod & 0x0FF) | ((value & 7) << 8));
            if (triangle.enabled) triangle.lengthCounter = kLengthTable[value >> 3];
            triangle.linearReload = true;
            break;

        // ----------------------------------------------------------- noise
        case 0x400C:
            noise.lengthHalt = (value >> 5) & 1;
            noise.constVolume = (value >> 4) & 1;
            noise.volume = static_cast<u8>(value & 0x0F);
            break;
        case 0x400E:
            noise.mode = (value & 0x80) != 0;
            noise.timerPeriod = kNoisePeriod[value & 0x0F];
            break;
        case 0x400F:
            if (noise.enabled) noise.lengthCounter = kLengthTable[value >> 3];
            noise.envStart = true;
            break;

        // ------------------------------------------------------------- DMC
        case 0x4010:
            dmc.irqEnable = (value & 0x80) != 0;
            dmc.loop = (value & 0x40) != 0;
            if (!dmc.irqEnable) dmc.dmcIrq = false;
            dmc.rateIndex = static_cast<u8>(value & 0x0F);
            dmc.timerPeriod = kDmcRate[value & 0x0F];
            break;
        case 0x4011:
            dmc.outputLevel = static_cast<u8>(value & 0x7F);
            break;
        case 0x4012:
            dmc.sampleAddr = static_cast<u16>(0xC000 + (value * 64));
            break;
        case 0x4013:
            dmc.sampleLength = static_cast<u16>((value * 64) + 1);
            break;

        // --------------------------------------------------------- control
        case 0x4015: {
            pulse1.enabled = (value & 0x01) != 0;
            pulse2.enabled = (value & 0x02) != 0;
            triangle.enabled = (value & 0x04) != 0;
            noise.enabled = (value & 0x08) != 0;

            // Disabling a channel forces its length counter to zero. Enabling
            // one does NOT reload it; it stays silent until a length write.
            if (!pulse1.enabled)   pulse1.lengthCounter = 0;
            if (!pulse2.enabled)   pulse2.lengthCounter = 0;
            if (!triangle.enabled) triangle.lengthCounter = 0;
            if (!noise.enabled)    noise.lengthCounter = 0;

            if ((value & 0x10) == 0) {
                dmc.bytesRemaining = 0;
            } else if (dmc.bytesRemaining == 0) {
                dmc.currentAddr = dmc.sampleAddr;
                dmc.bytesRemaining = dmc.sampleLength;
            }
            dmc.dmcIrq = false;
            break;
        }

        case 0x4017: {
            mode5 = (value & 0x80) != 0;
            irqInhibit = (value & 0x40) != 0;
            if (irqInhibit) frameIrqFlag = false;   // bit 6 also acknowledges
            frameCycle = 0;
            // In 5-step mode the write immediately clocks a quarter and half
            // frame (i.e. the length counters run straight away).
            if (mode5) { clockQuarterFrame(); clockHalfFrame(); }
            break;
        }

        default:
            break;
    }
}

// ---------------------------------------------------------------------------
// Save states
// ---------------------------------------------------------------------------
void APU::serialize(StateWriter& w) const {
    w.raw(&pulse1, sizeof(pulse1));
    w.raw(&pulse2, sizeof(pulse2));
    w.raw(&triangle, sizeof(triangle));
    w.raw(&noise, sizeof(noise));
    w.raw(&dmc, sizeof(dmc));
    w.pod(mode5); w.pod(irqInhibit); w.pod(frameIrqFlag); w.pod(dmcIrqFlag);
    w.pod(frameCycle);
    w.pod(apuPhase);
    w.pod(sampleAccum); w.pod(mixAccum); w.pod(mixCount);
}

void APU::deserialize(StateReader& r) {
    r.raw(&pulse1, sizeof(pulse1));
    r.raw(&pulse2, sizeof(pulse2));
    r.raw(&triangle, sizeof(triangle));
    r.raw(&noise, sizeof(noise));
    r.raw(&dmc, sizeof(dmc));
    r.pod(mode5); r.pod(irqInhibit); r.pod(frameIrqFlag); r.pod(dmcIrqFlag);
    r.pod(frameCycle);
    r.pod(apuPhase);
    r.pod(sampleAccum); r.pod(mixAccum); r.pod(mixCount);
    samples.clear();
}

} // namespace nes
