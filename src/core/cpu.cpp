#include "core/cpu.h"
#include "core/bus.h"

namespace nes {

// ------------------------------------------------------------------ memory
u8   CPU::read(u16 addr)  { return bus.read(addr); }
void CPU::write(u16 addr, u8 v) { bus.write(addr, v); }

// ---------------------------------------------------------------- helpers
void CPU::adc(u8 v) {
    const u16 sum = static_cast<u16>(a) + v + (flag(C) ? 1 : 0);
    setFlag(C, sum > 0xFF);
    // Overflow is set when both operands share a sign that differs from the result.
    setFlag(V, (~(a ^ v) & (a ^ static_cast<u8>(sum)) & 0x80) != 0);
    a = static_cast<u8>(sum);
    setZN(a);
}

void CPU::sbc(u8 v) {
    adc(static_cast<u8>(~v));   // A - v == A + ~v with carry as the borrow
}

void CPU::cmp(u8 reg, u8 v) {
    setFlag(C, reg >= v);
    setZN(static_cast<u8>(reg - v));
}

u8 CPU::asl(u8 v) { setFlag(C, (v & 0x80) != 0); v = static_cast<u8>(v << 1); setZN(v); return v; }
u8 CPU::lsr(u8 v) { setFlag(C, (v & 0x01) != 0); v = static_cast<u8>(v >> 1); setZN(v); return v; }
u8 CPU::rol(u8 v) {
    const bool c = flag(C);
    setFlag(C, (v & 0x80) != 0);
    v = static_cast<u8>((v << 1) | (c ? 1 : 0));
    setZN(v);
    return v;
}
u8 CPU::ror(u8 v) {
    const bool c = flag(C);
    setFlag(C, (v & 0x01) != 0);
    v = static_cast<u8>((v >> 1) | (c ? 0x80 : 0));
    setZN(v);
    return v;
}

void CPU::interrupt(u16 vector, bool brk) {
    push16(pc);
    u8 p = status | U;
    p = brk ? static_cast<u8>(p | B) : static_cast<u8>(p & ~B);
    push(p);
    setFlag(I, true);
    pc = static_cast<u16>(read(vector) | (read(vector + 1) << 8));
}

// ------------------------------------------------------------------ reset
void CPU::reset() {
    a = x = y = 0;
    sp = 0xFD;
    status = 0x24;          // U set, I set
    pc = static_cast<u16>(read(0xFFFC) | (read(0xFFFD) << 8));
    nmiPending = false;
    irqLine = false;
    pageCrossed = false;
}

// ------------------------------------------------------------------- step
u32 CPU::step() {
    if (nmiPending) {
        nmiPending = false;
        interrupt(0xFFFA, false);
        return 7;
    }
    // IRQ is level-triggered and maskable.
    if (irqLine && !flag(I)) {
        interrupt(0xFFFE, false);
        return 7;
    }

    pageCrossed = false;
    const u16 opPc = pc;
    const u8 op = fetch();
    // Trace hooks see the opcode before it runs, which is what a call tracer
    // needs to follow JSR/RTS and build an accurate call tree.
    if (debugHook) debugHook(debugHookCtx, opPc, op);

    // Relative branch helper: the offset byte is always consumed.
    auto branch = [&](bool take) -> u32 {
        const s16 off = static_cast<s8>(fetch());
        if (!take) return 2;
        const u16 from = pc;
        pc = static_cast<u16>(pc + off);
        return ((from & 0xFF00) != (pc & 0xFF00)) ? 4 : 3;
    };

    switch (op) {
        // ---------------------------------------------------------- 0x00-0x0F
        case 0x00: pc++; interrupt(0xFFFE, true); return 7;              // BRK
        case 0x01: { u16 ad = aIzx(); a |= read(ad); setZN(a); return 6; } // ORA (zp,X)
        case 0x05: { u16 ad = aZp();  a |= read(ad); setZN(a); return 3; } // ORA zp
        case 0x06: { u16 ad = aZp();  rmw(ad, [this](u8 v){ return asl(v); }); return 5; }
        case 0x08: push(status | B | U); return 3;                        // PHP
        case 0x09: { u8 v = fetch(); a |= v; setZN(a); return 2; }        // ORA #
        case 0x0A: a = asl(a); return 2;                                  // ASL A
        case 0x0B: { u8 v = fetch(); a &= v; setFlag(C, (a & 0x80) != 0); setZN(a); return 2; } // ANC
        case 0x0C: { (void)aAbs(); return 4; }                            // NOP abs
        case 0x0D: { u16 ad = aAbs(); a |= read(ad); setZN(a); return 4; }
        case 0x0E: { u16 ad = aAbs(); rmw(ad, [this](u8 v){ return asl(v); }); return 6; }
        case 0x0F: { u16 ad = aAbs(); rmw(ad, [this](u8 v){ setFlag(C,(v&0x80)!=0); v<<=1; a|=v; setZN(a); return v; }); return 6; } // SLO

        // ---------------------------------------------------------- 0x10-0x1F
        case 0x10: return branch(!flag(N));                               // BPL
        case 0x11: { u16 ad = aIzy(); a |= read(ad); setZN(a); return 5 + (pageCrossed?1:0); }
        case 0x15: { u16 ad = aZpx(); a |= read(ad); setZN(a); return 4; }
        case 0x16: { u16 ad = aZpx(); rmw(ad, [this](u8 v){ return asl(v); }); return 6; }
        case 0x18: setFlag(C, false); return 2;                           // CLC
        case 0x19: { u16 ad = aAby(); a |= read(ad); setZN(a); return 4 + (pageCrossed?1:0); }
        case 0x1A: return 2;                                              // NOP
        case 0x1B: { u16 ad = aAby(); rmw(ad, [this](u8 v){ setFlag(C,(v&0x80)!=0); v<<=1; a|=v; setZN(a); return v; }); return 7; }
        case 0x1C: { u16 ad = aAbx(); (void)read(ad); return 4 + (pageCrossed?1:0); }
        case 0x1D: { u16 ad = aAbx(); a |= read(ad); setZN(a); return 4 + (pageCrossed?1:0); }
        case 0x1E: { u16 ad = aAbx(); rmw(ad, [this](u8 v){ return asl(v); }); return 7; }
        case 0x1F: { u16 ad = aAbx(); rmw(ad, [this](u8 v){ setFlag(C,(v&0x80)!=0); v<<=1; a|=v; setZN(a); return v; }); return 7; }

        // ---------------------------------------------------------- 0x20-0x2F
        case 0x20: { u16 target = fetch16(); push16(static_cast<u16>(pc - 1)); pc = target; return 6; } // JSR
        case 0x21: { u16 ad = aIzx(); a &= read(ad); setZN(a); return 6; }
        case 0x24: { u16 ad = aZp();  u8 v = read(ad); setFlag(Z, (a & v) == 0);
                     setFlag(N, (v & 0x80) != 0); setFlag(V, (v & 0x40) != 0); return 3; } // BIT zp
        case 0x25: { u16 ad = aZp();  a &= read(ad); setZN(a); return 3; }
        case 0x26: { u16 ad = aZp();  rmw(ad, [this](u8 v){ return rol(v); }); return 5; }
        case 0x28: { u8 p = pull(); status = (p & ~B) | U; return 4; }    // PLP
        case 0x29: { u8 v = fetch(); a &= v; setZN(a); return 2; }
        case 0x2A: a = rol(a); return 2;
        case 0x2B: { u8 v = fetch(); a &= v; setFlag(C, (a & 0x80) != 0); setZN(a); return 2; } // ANC
        case 0x2C: { u16 ad = aAbs(); u8 v = read(ad); setFlag(Z, (a & v) == 0);
                     setFlag(N, (v & 0x80) != 0); setFlag(V, (v & 0x40) != 0); return 4; }
        case 0x2D: { u16 ad = aAbs(); a &= read(ad); setZN(a); return 4; }
        case 0x2E: { u16 ad = aAbs(); rmw(ad, [this](u8 v){ return rol(v); }); return 6; }
        case 0x2F: { u16 ad = aAbs(); rmw(ad, [this](u8 v){ v = rol(v); a &= v; setZN(a); return v; }); return 6; } // RLA

        // ---------------------------------------------------------- 0x30-0x3F
        case 0x30: return branch(flag(N));                                // BMI
        case 0x31: { u16 ad = aIzy(); a &= read(ad); setZN(a); return 5 + (pageCrossed?1:0); }
        case 0x35: { u16 ad = aZpx(); a &= read(ad); setZN(a); return 4; }
        case 0x36: { u16 ad = aZpx(); rmw(ad, [this](u8 v){ return rol(v); }); return 6; }
        case 0x38: setFlag(C, true); return 2;                            // SEC
        case 0x39: { u16 ad = aAby(); a &= read(ad); setZN(a); return 4 + (pageCrossed?1:0); }
        case 0x3A: return 2;
        case 0x3B: { u16 ad = aAby(); rmw(ad, [this](u8 v){ v = rol(v); a &= v; setZN(a); return v; }); return 7; }
        case 0x3C: { u16 ad = aAbx(); (void)read(ad); return 4 + (pageCrossed?1:0); }
        case 0x3D: { u16 ad = aAbx(); a &= read(ad); setZN(a); return 4 + (pageCrossed?1:0); }
        case 0x3E: { u16 ad = aAbx(); rmw(ad, [this](u8 v){ return rol(v); }); return 7; }
        case 0x3F: { u16 ad = aAbx(); rmw(ad, [this](u8 v){ v = rol(v); a &= v; setZN(a); return v; }); return 7; }

        // ---------------------------------------------------------- 0x40-0x4F
        case 0x40: { u8 p = pull(); status = (p & ~B) | U; pc = pull16(); return 6; } // RTI
        case 0x41: { u16 ad = aIzx(); a ^= read(ad); setZN(a); return 6; }
        case 0x45: { u16 ad = aZp();  a ^= read(ad); setZN(a); return 3; }
        case 0x46: { u16 ad = aZp();  rmw(ad, [this](u8 v){ return lsr(v); }); return 5; }
        case 0x48: push(a); return 3;                                     // PHA
        case 0x49: { u8 v = fetch(); a ^= v; setZN(a); return 2; }
        case 0x4A: a = lsr(a); return 2;
        case 0x4B: { u8 v = fetch(); a &= v; setFlag(C, (a & 0x80) != 0); a = lsr(a); return 2; } // ALR
        case 0x4C: pc = fetch16(); return 3;                              // JMP abs
        case 0x4D: { u16 ad = aAbs(); a ^= read(ad); setZN(a); return 4; }
        case 0x4E: { u16 ad = aAbs(); rmw(ad, [this](u8 v){ return lsr(v); }); return 6; }
        case 0x4F: { u16 ad = aAbs(); rmw(ad, [this](u8 v){ setFlag(C,(v&1)!=0); v>>=1; a^=v; setZN(a); return v; }); return 6; } // SRE

        // ---------------------------------------------------------- 0x50-0x5F
        case 0x50: return branch(!flag(V));                               // BVC
        case 0x51: { u16 ad = aIzy(); a ^= read(ad); setZN(a); return 5 + (pageCrossed?1:0); }
        case 0x55: { u16 ad = aZpx(); a ^= read(ad); setZN(a); return 4; }
        case 0x56: { u16 ad = aZpx(); rmw(ad, [this](u8 v){ return lsr(v); }); return 6; }
        case 0x58: setFlag(I, false); return 2;                           // CLI
        case 0x59: { u16 ad = aAby(); a ^= read(ad); setZN(a); return 4 + (pageCrossed?1:0); }
        case 0x5A: return 2;
        case 0x5B: { u16 ad = aAby(); rmw(ad, [this](u8 v){ setFlag(C,(v&1)!=0); v>>=1; a^=v; setZN(a); return v; }); return 7; }
        case 0x5C: { u16 ad = aAbx(); (void)read(ad); return 4 + (pageCrossed?1:0); }
        case 0x5D: { u16 ad = aAbx(); a ^= read(ad); setZN(a); return 4 + (pageCrossed?1:0); }
        case 0x5E: { u16 ad = aAbx(); rmw(ad, [this](u8 v){ return lsr(v); }); return 7; }
        case 0x5F: { u16 ad = aAbx(); rmw(ad, [this](u8 v){ setFlag(C,(v&1)!=0); v>>=1; a^=v; setZN(a); return v; }); return 7; }

        // ---------------------------------------------------------- 0x60-0x6F
        case 0x60: pc = static_cast<u16>(pull16() + 1); return 6;         // RTS
        case 0x61: { u16 ad = aIzx(); adc(read(ad)); return 6; }
        case 0x65: { u16 ad = aZp();  adc(read(ad)); return 3; }
        case 0x66: { u16 ad = aZp();  rmw(ad, [this](u8 v){ return ror(v); }); return 5; }
        case 0x68: a = pull(); setZN(a); return 4;                        // PLA
        case 0x69: { u8 v = fetch(); adc(v); return 2; }
        case 0x6A: a = ror(a); return 2;
        case 0x6B: { // ARR
            u8 v = fetch();
            a &= v;
            const bool c = flag(C);
            a = static_cast<u8>((a >> 1) | (c ? 0x80 : 0));
            setZN(a);
            setFlag(C, (a & 0x40) != 0);
            setFlag(V, ((a >> 6) ^ (a >> 5)) & 1);
            return 2;
        }
        case 0x6C: { // JMP (ind) - the page-boundary bug is emulated
            u16 ptr = fetch16();
            u16 lo = read(ptr);
            u16 hi = read(static_cast<u16>((ptr & 0xFF00) | ((ptr + 1) & 0x00FF)));
            pc = static_cast<u16>(lo | (hi << 8));
            return 5;
        }
        case 0x6D: { u16 ad = aAbs(); adc(read(ad)); return 4; }
        case 0x6E: { u16 ad = aAbs(); rmw(ad, [this](u8 v){ return ror(v); }); return 6; }
        case 0x6F: { u16 ad = aAbs(); rmw(ad, [this](u8 v){ v = ror(v); adc(v); return v; }); return 6; } // RRA

        // ---------------------------------------------------------- 0x70-0x7F
        case 0x70: return branch(flag(V));                                // BVS
        case 0x71: { u16 ad = aIzy(); adc(read(ad)); return 5 + (pageCrossed?1:0); }
        case 0x75: { u16 ad = aZpx(); adc(read(ad)); return 4; }
        case 0x76: { u16 ad = aZpx(); rmw(ad, [this](u8 v){ return ror(v); }); return 6; }
        case 0x78: setFlag(I, true); return 2;                            // SEI
        case 0x79: { u16 ad = aAby(); adc(read(ad)); return 4 + (pageCrossed?1:0); }
        case 0x7A: return 2;
        case 0x7B: { u16 ad = aAby(); rmw(ad, [this](u8 v){ v = ror(v); adc(v); return v; }); return 7; }
        case 0x7C: { u16 ad = aAbx(); (void)read(ad); return 4 + (pageCrossed?1:0); }
        case 0x7D: { u16 ad = aAbx(); adc(read(ad)); return 4 + (pageCrossed?1:0); }
        case 0x7E: { u16 ad = aAbx(); rmw(ad, [this](u8 v){ return ror(v); }); return 7; }
        case 0x7F: { u16 ad = aAbx(); rmw(ad, [this](u8 v){ v = ror(v); adc(v); return v; }); return 7; }

        // ---------------------------------------------------------- 0x80-0x8F
        case 0x80: case 0x82: case 0x89: case 0xC2: case 0xE2: { (void)fetch(); return 2; } // NOP #
        case 0x81: { u16 ad = aIzx(); write(ad, a); return 6; }           // STA (zp,X)
        case 0x83: { u16 ad = aIzx(); write(ad, static_cast<u8>(a & x)); return 6; } // SAX
        case 0x84: { u16 ad = aZp();  write(ad, y); return 3; }           // STY zp
        case 0x85: { u16 ad = aZp();  write(ad, a); return 3; }           // STA zp
        case 0x86: { u16 ad = aZp();  write(ad, x); return 3; }           // STX zp
        case 0x87: { u16 ad = aZp();  write(ad, static_cast<u8>(a & x)); return 3; }
        case 0x88: y--; setZN(y); return 2;                               // DEY
        case 0x8A: a = x; setZN(a); return 2;                             // TXA
        case 0x8B: { u8 v = fetch(); a = static_cast<u8>(x & v); setZN(a); return 2; } // XAA
        case 0x8C: { u16 ad = aAbs(); write(ad, y); return 4; }
        case 0x8D: { u16 ad = aAbs(); write(ad, a); return 4; }
        case 0x8E: { u16 ad = aAbs(); write(ad, x); return 4; }
        case 0x8F: { u16 ad = aAbs(); write(ad, static_cast<u8>(a & x)); return 4; }

        // ---------------------------------------------------------- 0x90-0x9F
        case 0x90: return branch(!flag(C));                               // BCC
        case 0x91: { u16 ad = aIzy(); write(ad, a); return 6; }
        case 0x93: { u16 ad = aIzy(); write(ad, static_cast<u8>(a & x)); return 6; } // AHX
        case 0x94: { u16 ad = aZpy(); write(ad, y); return 4; }
        case 0x95: { u16 ad = aZpx(); write(ad, a); return 4; }
        case 0x96: { u16 ad = aZpy(); write(ad, x); return 4; }
        case 0x97: { u16 ad = aZpy(); write(ad, static_cast<u8>(a & x)); return 4; }
        case 0x98: a = y; setZN(a); return 2;                             // TYA
        case 0x99: { u16 ad = aAby(); write(ad, a); return 5; }
        case 0x9A: sp = x; return 2;                                      // TXS
        case 0x9B: { u16 ad = aAby(); sp = static_cast<u8>(a & x);
                     write(ad, static_cast<u8>(sp & ((ad >> 8) + 1))); return 5; } // TAS
        case 0x9C: { u16 ad = aAbx(); write(ad, static_cast<u8>(y & ((ad >> 8) + 1))); return 5; } // SHY
        case 0x9D: { u16 ad = aAbx(); write(ad, a); return 5; }
        case 0x9E: { u16 ad = aAby(); write(ad, static_cast<u8>(x & ((ad >> 8) + 1))); return 5; } // SHX
        case 0x9F: { u16 ad = aAby(); write(ad, static_cast<u8>(a & x & ((ad >> 8) + 1))); return 5; }

        // ---------------------------------------------------------- 0xA0-0xAF
        case 0xA0: y = fetch(); setZN(y); return 2;                       // LDY #
        case 0xA1: { u16 ad = aIzx(); a = read(ad); setZN(a); return 6; }
        case 0xA2: x = fetch(); setZN(x); return 2;                       // LDX #
        case 0xA3: { u16 ad = aIzx(); a = x = read(ad); setZN(a); return 6; } // LAX
        case 0xA4: { u16 ad = aZp();  y = read(ad); setZN(y); return 3; }
        case 0xA5: { u16 ad = aZp();  a = read(ad); setZN(a); return 3; }
        case 0xA6: { u16 ad = aZp();  x = read(ad); setZN(x); return 3; }
        case 0xA7: { u16 ad = aZp();  a = x = read(ad); setZN(a); return 3; }
        case 0xA8: y = a; setZN(y); return 2;                             // TAY
        case 0xA9: a = fetch(); setZN(a); return 2;                       // LDA #
        case 0xAA: x = a; setZN(x); return 2;                             // TAX
        case 0xAB: { u8 v = fetch(); a = x = static_cast<u8>(a & v); setZN(a); return 2; } // LAX #
        case 0xAC: { u16 ad = aAbs(); y = read(ad); setZN(y); return 4; }
        case 0xAD: { u16 ad = aAbs(); a = read(ad); setZN(a); return 4; }
        case 0xAE: { u16 ad = aAbs(); x = read(ad); setZN(x); return 4; }
        case 0xAF: { u16 ad = aAbs(); a = x = read(ad); setZN(a); return 4; }

        // ---------------------------------------------------------- 0xB0-0xBF
        case 0xB0: return branch(flag(C));                                // BCS
        case 0xB1: { u16 ad = aIzy(); a = read(ad); setZN(a); return 5 + (pageCrossed?1:0); }
        case 0xB3: { u16 ad = aIzy(); a = x = read(ad); setZN(a); return 5 + (pageCrossed?1:0); } // LAX
        case 0xB4: { u16 ad = aZpx(); y = read(ad); setZN(y); return 4; }
        case 0xB5: { u16 ad = aZpx(); a = read(ad); setZN(a); return 4; }
        case 0xB6: { u16 ad = aZpy(); x = read(ad); setZN(x); return 4; }
        case 0xB7: { u16 ad = aZpy(); a = x = read(ad); setZN(a); return 4; }
        case 0xB8: setFlag(V, false); return 2;                           // CLV
        case 0xB9: { u16 ad = aAby(); a = read(ad); setZN(a); return 4 + (pageCrossed?1:0); }
        case 0xBA: x = sp; setZN(x); return 2;                            // TSX
        case 0xBB: { u16 ad = aAby(); x = sp = static_cast<u8>(read(ad) & sp); setZN(x); return 4 + (pageCrossed?1:0); } // LAS
        case 0xBC: { u16 ad = aAbx(); y = read(ad); setZN(y); return 4 + (pageCrossed?1:0); }
        case 0xBD: { u16 ad = aAbx(); a = read(ad); setZN(a); return 4 + (pageCrossed?1:0); }
        case 0xBE: { u16 ad = aAby(); x = read(ad); setZN(x); return 4 + (pageCrossed?1:0); }
        case 0xBF: { u16 ad = aAby(); a = x = read(ad); setZN(a); return 4 + (pageCrossed?1:0); }

        // ---------------------------------------------------------- 0xC0-0xCF
        case 0xC0: { u8 v = fetch(); cmp(y, v); return 2; }               // CPY #
        case 0xC1: { u16 ad = aIzx(); cmp(a, read(ad)); return 6; }
        case 0xC3: { u16 ad = aIzx(); rmw(ad, [this](u8 v){ u8 r = static_cast<u8>(v - 1); setFlag(C, v >= 1); setZN(r); return r; }); cmp(a, read(ad)); return 8; } // DCP
        case 0xC4: { u16 ad = aZp();  cmp(y, read(ad)); return 3; }
        case 0xC5: { u16 ad = aZp();  cmp(a, read(ad)); return 3; }
        case 0xC6: { u16 ad = aZp();  rmw(ad, [this](u8 v){ u8 r = static_cast<u8>(v - 1); setZN(r); return r; }); return 5; } // DEC zp
        case 0xC7: { u16 ad = aZp();  rmw(ad, [this](u8 v){ u8 r = static_cast<u8>(v - 1); setZN(r); return r; }); cmp(a, read(ad)); return 5; }
        case 0xC8: y++; setZN(y); return 2;                               // INY
        case 0xC9: { u8 v = fetch(); cmp(a, v); return 2; }               // CMP #
        case 0xCA: x--; setZN(x); return 2;                               // DEX
        case 0xCB: { u8 v = fetch(); u8 t = static_cast<u8>(a & x);
                     setFlag(C, t >= v); t = static_cast<u8>(t - v); x = t; setZN(t); return 2; } // AXS
        case 0xCC: { u16 ad = aAbs(); cmp(y, read(ad)); return 4; }
        case 0xCD: { u16 ad = aAbs(); cmp(a, read(ad)); return 4; }
        case 0xCE: { u16 ad = aAbs(); rmw(ad, [this](u8 v){ u8 r = static_cast<u8>(v - 1); setZN(r); return r; }); return 6; }
        case 0xCF: { u16 ad = aAbs(); rmw(ad, [this](u8 v){ u8 r = static_cast<u8>(v - 1); setZN(r); return r; }); cmp(a, read(ad)); return 6; }

        // ---------------------------------------------------------- 0xD0-0xDF
        case 0xD0: return branch(!flag(Z));                               // BNE
        case 0xD1: { u16 ad = aIzy(); cmp(a, read(ad)); return 5 + (pageCrossed?1:0); }
        case 0xD3: { u16 ad = aIzy(); rmw(ad, [this](u8 v){ u8 r = static_cast<u8>(v - 1); setZN(r); return r; }); cmp(a, read(ad)); return 8; }
        case 0xD5: { u16 ad = aZpx(); cmp(a, read(ad)); return 4; }
        case 0xD6: { u16 ad = aZpx(); rmw(ad, [this](u8 v){ u8 r = static_cast<u8>(v - 1); setZN(r); return r; }); return 6; }
        case 0xD7: { u16 ad = aZpx(); rmw(ad, [this](u8 v){ u8 r = static_cast<u8>(v - 1); setZN(r); return r; }); cmp(a, read(ad)); return 6; }
        case 0xD8: setFlag(D, false); return 2;                           // CLD
        case 0xD9: { u16 ad = aAby(); cmp(a, read(ad)); return 4 + (pageCrossed?1:0); }
        case 0xDA: return 2;
        case 0xDB: { u16 ad = aAby(); rmw(ad, [this](u8 v){ u8 r = static_cast<u8>(v - 1); setZN(r); return r; }); cmp(a, read(ad)); return 7; }
        case 0xDC: { u16 ad = aAbx(); (void)read(ad); return 4 + (pageCrossed?1:0); }
        case 0xDD: { u16 ad = aAbx(); cmp(a, read(ad)); return 4 + (pageCrossed?1:0); }
        case 0xDE: { u16 ad = aAbx(); rmw(ad, [this](u8 v){ u8 r = static_cast<u8>(v - 1); setZN(r); return r; }); return 7; }
        case 0xDF: { u16 ad = aAbx(); rmw(ad, [this](u8 v){ u8 r = static_cast<u8>(v - 1); setZN(r); return r; }); cmp(a, read(ad)); return 7; }

        // ---------------------------------------------------------- 0xE0-0xEF
        case 0xE0: { u8 v = fetch(); cmp(x, v); return 2; }               // CPX #
        case 0xE1: { u16 ad = aIzx(); sbc(read(ad)); return 6; }
        case 0xE3: { u16 ad = aIzx(); rmw(ad, [this](u8 v){ u8 r = static_cast<u8>(v + 1); setZN(r); return r; }); sbc(read(ad)); return 8; } // ISB
        case 0xE4: { u16 ad = aZp();  cmp(x, read(ad)); return 3; }
        case 0xE5: { u16 ad = aZp();  sbc(read(ad)); return 3; }
        case 0xE6: { u16 ad = aZp();  rmw(ad, [this](u8 v){ u8 r = static_cast<u8>(v + 1); setZN(r); return r; }); return 5; } // INC zp
        case 0xE7: { u16 ad = aZp();  rmw(ad, [this](u8 v){ u8 r = static_cast<u8>(v + 1); setZN(r); return r; }); sbc(read(ad)); return 5; }
        case 0xE8: x++; setZN(x); return 2;                               // INX
        case 0xE9: case 0xEB: { u8 v = fetch(); sbc(v); return 2; }       // SBC # (0xEB is unofficial)
        case 0xEA: return 2;                                              // NOP
        case 0xEC: { u16 ad = aAbs(); cmp(x, read(ad)); return 4; }
        case 0xED: { u16 ad = aAbs(); sbc(read(ad)); return 4; }
        case 0xEE: { u16 ad = aAbs(); rmw(ad, [this](u8 v){ u8 r = static_cast<u8>(v + 1); setZN(r); return r; }); return 6; }
        case 0xEF: { u16 ad = aAbs(); rmw(ad, [this](u8 v){ u8 r = static_cast<u8>(v + 1); setZN(r); return r; }); sbc(read(ad)); return 6; }

        // ---------------------------------------------------------- 0xF0-0xFF
        case 0xF0: return branch(flag(Z));                                // BEQ
        case 0xF1: { u16 ad = aIzy(); sbc(read(ad)); return 5 + (pageCrossed?1:0); }
        case 0xF3: { u16 ad = aIzy(); rmw(ad, [this](u8 v){ u8 r = static_cast<u8>(v + 1); setZN(r); return r; }); sbc(read(ad)); return 8; }
        case 0xF5: { u16 ad = aZpx(); sbc(read(ad)); return 4; }
        case 0xF6: { u16 ad = aZpx(); rmw(ad, [this](u8 v){ u8 r = static_cast<u8>(v + 1); setZN(r); return r; }); return 6; }
        case 0xF7: { u16 ad = aZpx(); rmw(ad, [this](u8 v){ u8 r = static_cast<u8>(v + 1); setZN(r); return r; }); sbc(read(ad)); return 6; }
        case 0xF8: setFlag(D, true); return 2;                            // SED
        case 0xF9: { u16 ad = aAby(); sbc(read(ad)); return 4 + (pageCrossed?1:0); }
        case 0xFA: return 2;
        case 0xFB: { u16 ad = aAby(); rmw(ad, [this](u8 v){ u8 r = static_cast<u8>(v + 1); setZN(r); return r; }); sbc(read(ad)); return 7; }
        case 0xFC: { u16 ad = aAbx(); (void)read(ad); return 4 + (pageCrossed?1:0); }
        case 0xFD: { u16 ad = aAbx(); sbc(read(ad)); return 4 + (pageCrossed?1:0); }
        case 0xFE: { u16 ad = aAbx(); rmw(ad, [this](u8 v){ u8 r = static_cast<u8>(v + 1); setZN(r); return r; }); return 7; }
        case 0xFF: { u16 ad = aAbx(); rmw(ad, [this](u8 v){ u8 r = static_cast<u8>(v + 1); setZN(r); return r; }); sbc(read(ad)); return 7; }
    }

    // Any opcode without a case above is unimplemented. Record it rather than
    // failing silently -- a game that lands here is being emulated wrongly.
    unknownOpcodeCount++;
    lastUnknownOpcode = op;
    return 2;
}

// ------------------------------------------------------------ save states
void CPU::serialize(StateWriter& w) const {
    w.pod(a); w.pod(x); w.pod(y); w.pod(sp); w.pod(status); w.pod(pc);
    w.boolean(nmiPending); w.boolean(irqLine);
}
void CPU::deserialize(StateReader& r) {
    r.pod(a); r.pod(x); r.pod(y); r.pod(sp); r.pod(status); r.pod(pc);
    r.boolean(nmiPending); r.boolean(irqLine);
    pageCrossed = false;
}

} // namespace nes
