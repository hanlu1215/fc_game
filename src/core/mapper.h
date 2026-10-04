#pragma once

#include "core/cartridge.h"
#include "core/types.h"

namespace nes {

// Base class for cartridge mappers.
//
// A mapper translates CPU/PPU addresses into offsets inside the cartridge's
// ROM/RAM arrays. Everything the CPU sees at $4020-$FFFF and everything the
// PPU sees at $0000-$1FFF goes through here.
class Mapper {
public:
    explicit Mapper(Cartridge& c) : cart(c) {}
    virtual ~Mapper() = default;

    virtual u8   cpuRead(u16 addr) = 0;
    virtual void cpuWrite(u16 addr, u8 value) = 0;
    virtual u8   ppuRead(u16 addr) = 0;
    virtual void ppuWrite(u16 addr, u8 value) = 0;

    virtual void reset() {}

    // Called by the PPU once per visible scanline (at dot 260, where the
    // PPU's A12 line rises). MMC3 uses this to clock its IRQ counter.
    virtual void scanline() {}

    virtual bool irqActive() const { return false; }
    virtual void irqClear() {}

    virtual const char* name() const = 0;

    virtual void serialize(StateWriter&) {}
    virtual void deserialize(StateReader&) {}

protected:
    Cartridge& cart;

    // Helpers for the common case of a flat 8 KiB CHR ROM/RAM.
    u8 readChr8k(u16 addr) const { return cart.chr[addr & (cart.chr.size() - 1)]; }
    void writeChr8k(u16 addr, u8 value) {
        if (cart.chrIsRam) cart.chr[addr & (cart.chr.size() - 1)] = value;
    }
    // PRG RAM is always readable/writable at $6000-$7FFF when present.
    u8 readPrgRam(u16 addr) const {
        return cart.prgRam.empty() ? 0 : cart.prgRam[(addr - 0x6000) % cart.prgRam.size()];
    }
    void writePrgRam(u16 addr, u8 value) {
        if (!cart.prgRam.empty()) cart.prgRam[(addr - 0x6000) % cart.prgRam.size()] = value;
    }
};

// Returns nullptr for an unsupported mapper number.
Mapper* createMapper(int number, Cartridge& cart);

} // namespace nes
