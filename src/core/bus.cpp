#include "core/bus.h"

namespace nes {

u8 Bus::read(u16 addr) {
    u8 value;
    if (addr < 0x2000) {
        value = ram[addr & 0x07FF];
    } else if (addr < 0x4000) {
        value = ppu.readRegister(static_cast<u16>(addr & 7));
    } else if (addr == 0x4015) {
        value = apu.readRegister(addr);
    } else if (addr == 0x4016) {
        value = static_cast<u8>(0x40 | (ctrls[0].read() & 0x1F) | (openBus & 0xE0));
    } else if (addr == 0x4017) {
        value = static_cast<u8>(0x40 | (ctrls[1].read() & 0x1F) | (openBus & 0xE0));
    } else if (addr < 0x4020) {
        value = openBus;            // disabled test registers
    } else {
        value = mapper ? mapper->cpuRead(addr) : openBus;
    }
    openBus = value;
    return value;
}

void Bus::write(u16 addr, u8 value) {
    openBus = value;

    if (addr < 0x2000) {
        ram[addr & 0x07FF] = value;
        return;
    }
    if (addr < 0x4000) {
        ppu.writeRegister(static_cast<u16>(addr & 7), value);
        return;
    }
    if (addr == 0x4014) {
        doOamDma(value);
        return;
    }
    if (addr < 0x4018) {
        apu.writeRegister(addr, value);
        if (addr == 0x4016) {
            // A write to $4016 strobes both controller ports.
            const bool on = (value & 1) != 0;
            ctrls[0].strobe(on);
            ctrls[1].strobe(on);
        }
        return;
    }
    if (addr < 0x4020) return;

    if (mapper) mapper->cpuWrite(addr, value);
}

// $4014: copy 256 bytes from CPU page `page` into OAM. The transfer runs
// through $2004, so it starts at the current OAMADDR and wraps around.
void Bus::doOamDma(u8 page) {
    const u16 base = static_cast<u16>(page) << 8;
    for (int i = 0; i < 256; i++) {
        const u8 v = read(static_cast<u16>(base + i));
        ppu.writeRegister(0x2004, v);
    }
    // 513 cycles, or 514 when the halt lands on the wrong phase. The extra
    // cycle is not modelled; one cycle per frame is not observable here.
    dmaStall += 513;
}

} // namespace nes
