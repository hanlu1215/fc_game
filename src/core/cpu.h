#pragma once

#include "core/types.h"

namespace nes {

class Bus;

// Ricoh 2A03 CPU core (NMOS 6502 without decimal mode).
//
// Instructions are executed one at a time; `step()` returns the number of CPU
// cycles the instruction took so the caller can advance the PPU and APU by a
// matching amount. Official opcodes plus the undocumented ones real games use
// (LAX/SAX/DCP/ISB/SLO/RLA/SRE/RRA and friends) are implemented.
class CPU {
public:
    explicit CPU(Bus& b) : bus(b) {}

    void reset();
    u32  step();

    void nmi() { nmiPending = true; }
    void setIrqLine(bool level) { irqLine = level; }

    // Registers (public so the save-state writer can reach them directly).
    u8  a = 0, x = 0, y = 0, sp = 0xFD, status = 0x24;
    u16 pc = 0;

    // Counts executions of opcodes the core does not implement. A non-zero
    // value means the running game is doing something we are silently getting
    // wrong, so the frontend/test harness can flag it.
    u32 unknownOpcodeCount = 0;
    u8  lastUnknownOpcode = 0;

    // Debug hook, used by the tracing tool. It is called with the address of
    // the opcode and the opcode byte just before the instruction executes.
    // A raw function pointer keeps this to one predictable branch per
    // instruction when no tracer is attached.
    void (*debugHook)(void* ctx, u16 pc, u8 op) = nullptr;
    void* debugHookCtx = nullptr;

    void serialize(StateWriter& w) const;
    void deserialize(StateReader& r);

private:
    Bus& bus;

    bool nmiPending = false;
    bool irqLine = false;
    bool pageCrossed = false;

    // ---- memory (defined in cpu.cpp, where Bus is a complete type) --------
    u8  read(u16 addr);
    void write(u16 addr, u8 v);
    u8  fetch() { return read(pc++); }
    u16 fetch16() { u16 lo = fetch(); return static_cast<u16>(lo | (fetch() << 8)); }

    void push(u8 v) { write(0x0100 + sp--, v); }
    u8   pull() { return read(0x0100 + ++sp); }
    void push16(u16 v) { push(static_cast<u8>(v >> 8)); push(static_cast<u8>(v & 0xFF)); }
    u16  pull16() { u16 lo = pull(); return static_cast<u16>(lo | (pull() << 8)); }

    // ---- flags ------------------------------------------------------------
    bool flag(u8 f) const { return (status & f) != 0; }
    void setFlag(u8 f, bool on) { status = on ? (status | f) : (status & ~f); }
    void setZN(u8 v) {
        setFlag(Z, v == 0);
        setFlag(N, (v & 0x80) != 0);
    }

    void interrupt(u16 vector, bool brk);

    // ---- addressing modes -------------------------------------------------
    u16 aImm() { return pc++; }
    u16 aZp()  { return fetch(); }
    u16 aZpx() { return static_cast<u8>(fetch() + x); }
    u16 aZpy() { return static_cast<u8>(fetch() + y); }
    u16 aAbs() { return fetch16(); }
    u16 aAbx() {
        u16 base = fetch16();
        u16 addr = static_cast<u16>(base + x);
        pageCrossed = (base & 0xFF00) != (addr & 0xFF00);
        return addr;
    }
    u16 aAby() {
        u16 base = fetch16();
        u16 addr = static_cast<u16>(base + y);
        pageCrossed = (base & 0xFF00) != (addr & 0xFF00);
        return addr;
    }
    u16 aIzx() {
        u8 zp = static_cast<u8>(fetch() + x);
        return static_cast<u16>(read(zp) | (read(static_cast<u8>(zp + 1)) << 8));
    }
    u16 aIzy() {
        u8 zp = fetch();
        u16 base = static_cast<u16>(read(zp) | (read(static_cast<u8>(zp + 1)) << 8));
        u16 addr = static_cast<u16>(base + y);
        pageCrossed = (base & 0xFF00) != (addr & 0xFF00);
        return addr;
    }

    // ---- ALU helpers ------------------------------------------------------
    void adc(u8 v);
    void sbc(u8 v);
    void cmp(u8 reg, u8 v);
    u8   asl(u8 v);
    u8   lsr(u8 v);
    u8   rol(u8 v);
    u8   ror(u8 v);

    // Read-modify-write instructions share this shape: read, dummy write,
    // modify, real write. The caller supplies the cycle count.
    //
    // Flags are deliberately NOT set here: `fn` does it. The combined
    // read-modify-write ops (SLO, RLA, SRE, RRA, DCP, ISB) finish with an
    // ALU operation whose flags must survive, so setting them from the
    // modified memory byte afterwards would clobber the real result.
    template <typename F> void rmw(u16 addr, F fn) {
        u8 v = read(addr);
        write(addr, v);              // dummy write, as the real 6502 does
        v = fn(v);
        write(addr, v);
    }
};

} // namespace nes
