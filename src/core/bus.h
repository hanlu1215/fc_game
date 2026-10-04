#pragma once

#include "core/apu.h"
#include "core/controller.h"
#include "core/mapper.h"
#include "core/ppu.h"
#include "core/types.h"

namespace nes {

// The CPU's view of memory.
//
//   $0000-$1FFF  2 KiB work RAM, mirrored four times
//   $2000-$3FFF  PPU registers, mirrored every 8 bytes
//   $4000-$4017  APU, DMA and controller ports
//   $4018-$401F  disabled test registers -> open bus
//   $4020-$FFFF  cartridge (mapper)
class Bus {
public:
    Bus(PPU& p, APU& a, Controller* c) : ppu(p), apu(a), ctrls(c) {}

    void setMapper(Mapper* m) { mapper = m; }

    u8   read(u16 addr);
    void write(u16 addr, u8 value);

    // Unmapped reads return the last value that travelled over the data bus.
    // A few games (Low G Man, among others) depend on it.
    u8 openBusValue() const { return openBus; }

    // Cycles stolen by the most recent OAM DMA; the console adds these to the
    // PPU/APU advance so the whole machine stays in step.
    u32 takeDmaStall() { u32 s = dmaStall; dmaStall = 0; return s; }

    void reset() { dmaStall = 0; openBus = 0; }

    // Direct RAM access for save states.
    u8*       ramData()       { return ram; }
    const u8* ramData() const { return ram; }
    static constexpr size_t ramSize() { return sizeof(ram); }

private:
    PPU& ppu;
    APU& apu;
    Controller* ctrls;
    Mapper* mapper = nullptr;

    u8  ram[2048] = {};
    u8  openBus = 0;
    u32 dmaStall = 0;

    void doOamDma(u8 page);
};

} // namespace nes
