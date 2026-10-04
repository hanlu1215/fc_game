#include "core/mapper.h"

namespace nes {
namespace {

// ===========================================================================
// Mapper 0 - NROM
// Fixed 16 or 32 KiB PRG, fixed 8 KiB CHR (ROM or RAM). No bank switching.
// ===========================================================================
class Mapper0 : public Mapper {
public:
    explicit Mapper0(Cartridge& c) : Mapper(c) {}

    u8 cpuRead(u16 addr) override {
        if (addr >= 0x6000 && addr < 0x8000) return readPrgRam(addr);
        if (addr < 0x8000) return 0;
        u32 off = addr - 0x8000;
        if (cart.prgRom.size() == 16384) off &= 0x3FFF;   // 16 KiB mirrored
        return cart.prgRom[off % cart.prgRom.size()];
    }
    void cpuWrite(u16 addr, u8 value) override {
        if (addr >= 0x6000 && addr < 0x8000) writePrgRam(addr, value);
        // Writes to PRG ROM are ignored.
    }
    u8   ppuRead(u16 addr) override { return readChr8k(addr & 0x1FFF); }
    void ppuWrite(u16 addr, u8 value) override { writeChr8k(addr & 0x1FFF, value); }

    const char* name() const override { return "NROM"; }
};

// ===========================================================================
// Mapper 1 - MMC1 (SxROM)
// A serial shift register: five writes of one bit each assemble a value.
// Writing a byte with bit 7 set resets the register and forces PRG mode 3.
// ===========================================================================
class Mapper1 : public Mapper {
public:
    explicit Mapper1(Cartridge& c) : Mapper(c) { reset(); }

    void reset() override {
        shift = 0x10;
        control = 0x1C;          // PRG mode 3, CHR mode 0
        chrBank0 = chrBank1 = prgBank = 0;
        updateBanks();
    }

    u8 cpuRead(u16 addr) override {
        if (addr >= 0x6000 && addr < 0x8000) return readPrgRam(addr);
        if (addr < 0x8000) return 0;
        return cart.prgRom[prgOffset(addr) % cart.prgRom.size()];
    }

    void cpuWrite(u16 addr, u8 value) override {
        if (addr >= 0x6000 && addr < 0x8000) { writePrgRam(addr, value); return; }
        if (addr < 0x8000) return;

        if (value & 0x80) {                 // reset the shift register
            shift = 0x10;
            control |= 0x0C;                // PRG mode 3 = fix last bank at $C000
            updateBanks();
            return;
        }

        // The bit currently in position 0 signals that this is the 5th write.
        const bool complete = (shift & 1) != 0;
        shift = static_cast<u8>((shift >> 1) | ((value & 1) << 4));

        if (complete) {
            const u8 v = shift & 0x1F;
            switch ((addr >> 13) & 3) {
                case 0: control  = v; updateMirroring(); break;
                case 1: chrBank0 = v; break;
                case 2: chrBank1 = v; break;
                case 3: prgBank  = v; break;
            }
            shift = 0x10;
            updateBanks();
        }
    }

    u8 ppuRead(u16 addr) override {
        addr &= 0x1FFF;
        u32 off;
        if (control & 0x10) {                       // 4 KiB CHR mode
            off = (addr < 0x1000)
                ? (static_cast<u32>(chrBank0) * 4096 + (addr & 0x0FFF))
                : (static_cast<u32>(chrBank1) * 4096 + (addr & 0x0FFF));
        } else {                                    // 8 KiB CHR mode
            off = static_cast<u32>(chrBank0 & 0xFE) * 4096 + addr;
        }
        return cart.chr[off % cart.chr.size()];
    }
    void ppuWrite(u16 addr, u8 value) override { writeChr8k(addr & 0x1FFF, value); }

    const char* name() const override { return "MMC1"; }

    void serialize(StateWriter& w) override {
        w.pod(shift); w.pod(control); w.pod(chrBank0); w.pod(chrBank1); w.pod(prgBank);
    }
    void deserialize(StateReader& r) override {
        r.pod(shift); r.pod(control); r.pod(chrBank0); r.pod(chrBank1); r.pod(prgBank);
        updateBanks();
    }

private:
    u8 shift = 0x10, control = 0x1C, chrBank0 = 0, chrBank1 = 0, prgBank = 0;

    void updateMirroring() {
        switch (control & 3) {
            case 0: cart.mirroring = Mirroring::SingleLower; break;
            case 1: cart.mirroring = Mirroring::SingleUpper; break;
            case 2: cart.mirroring = Mirroring::Vertical;    break;
            default: cart.mirroring = Mirroring::Horizontal; break;
        }
    }

    // Resolve $8000-$FFFF into a PRG offset under the current mode.
    u32 prgOffset(u16 addr) const {
        const u32 banks = static_cast<u32>(cart.prgRom.size() / 16384);
        const u32 last = banks - 1;
        switch ((control >> 2) & 3) {
            case 0:
            case 1: {                               // 32 KiB switch, low bit ignored
                u32 base = (prgBank & 0x0E) % (banks ? banks : 1);
                return (base * 16384 + (addr - 0x8000)) % cart.prgRom.size();
            }
            case 2:                                 // fix first, switch $C000
                if (addr < 0xC000) return (addr - 0x8000) % cart.prgRom.size();
                return ((static_cast<u32>(prgBank & 0x0F) % banks) * 16384 + (addr - 0xC000))
                       % cart.prgRom.size();
            default:                                // switch $8000, fix last
                if (addr >= 0xC000) return (last * 16384 + (addr - 0xC000)) % cart.prgRom.size();
                return ((static_cast<u32>(prgBank & 0x0F) % banks) * 16384 + (addr - 0x8000))
                       % cart.prgRom.size();
        }
    }

    void updateBanks() {}
};

// ===========================================================================
// Mapper 2 - UxROM
// Switchable 16 KiB PRG at $8000, last bank fixed at $C000. CHR RAM only.
// ===========================================================================
class Mapper2 : public Mapper {
public:
    explicit Mapper2(Cartridge& c) : Mapper(c) { reset(); }

    void reset() override { bank = 0; }

    u8 cpuRead(u16 addr) override {
        if (addr >= 0x6000 && addr < 0x8000) return readPrgRam(addr);
        if (addr < 0x8000) return 0;
        return cart.prgRom[prgOffset(addr) % cart.prgRom.size()];
    }
    void cpuWrite(u16 addr, u8 value) override {
        if (addr >= 0x6000 && addr < 0x8000) { writePrgRam(addr, value); return; }
        if (addr < 0x8000) return;
        bank = value & 0x0F;
    }
    u8   ppuRead(u16 addr) override { return readChr8k(addr & 0x1FFF); }
    void ppuWrite(u16 addr, u8 value) override { writeChr8k(addr & 0x1FFF, value); }

    const char* name() const override { return "UxROM"; }

    void serialize(StateWriter& w) override { w.pod(bank); }
    void deserialize(StateReader& r) override { r.pod(bank); }

private:
    u8 bank = 0;
    u32 prgOffset(u16 addr) const {
        const u32 banks = static_cast<u32>(cart.prgRom.size() / 16384);
        if (addr >= 0xC000) return (banks - 1) * 16384 + (addr - 0xC000);
        return (static_cast<u32>(bank) % banks) * 16384 + (addr - 0x8000);
    }
};

// ===========================================================================
// Mapper 3 - CNROM
// NROM-style fixed PRG, but the 8 KiB CHR bank is switchable.
// ===========================================================================
class Mapper3 : public Mapper {
public:
    explicit Mapper3(Cartridge& c) : Mapper(c) { reset(); }

    void reset() override { bank = 0; }

    u8 cpuRead(u16 addr) override {
        if (addr >= 0x6000 && addr < 0x8000) return readPrgRam(addr);
        if (addr < 0x8000) return 0;
        u32 off = addr - 0x8000;
        if (cart.prgRom.size() == 16384) off &= 0x3FFF;
        return cart.prgRom[off % cart.prgRom.size()];
    }
    void cpuWrite(u16 addr, u8 value) override {
        if (addr >= 0x6000 && addr < 0x8000) { writePrgRam(addr, value); return; }
        if (addr < 0x8000) return;
        bank = value & 0x03;
    }
    u8 ppuRead(u16 addr) override {
        const u32 banks = static_cast<u32>(cart.chr.size() / 8192);
        const u32 b = banks ? (bank % banks) : 0;
        return cart.chr[(b * 8192 + (addr & 0x1FFF)) % cart.chr.size()];
    }
    void ppuWrite(u16 addr, u8 value) override { writeChr8k(addr & 0x1FFF, value); }

    const char* name() const override { return "CNROM"; }

    void serialize(StateWriter& w) override { w.pod(bank); }
    void deserialize(StateReader& r) override { r.pod(bank); }

private:
    u8 bank = 0;
};

// ===========================================================================
// Mapper 4 - MMC3 (TxROM)
// Fine-grained PRG/CHR banking plus a scanline IRQ counter, which games use
// for split-screen effects (status bars that do not scroll).
// ===========================================================================
class Mapper4 : public Mapper {
public:
    explicit Mapper4(Cartridge& c) : Mapper(c) { reset(); }

    void reset() override {
        bankSelect = 0;
        for (u8& r : regs) r = 0;
        prgRamEnabled = true;
        irqLatch = irqCounter = 0;
        irqReload = irqEnabled = irqPending = false;
        cart.mirroring = Mirroring::Horizontal;
    }

    u8 cpuRead(u16 addr) override {
        if (addr >= 0x6000 && addr < 0x8000) return readPrgRam(addr);
        if (addr < 0x8000) return 0;
        return cart.prgRom[prgOffset(addr) % cart.prgRom.size()];
    }

    void cpuWrite(u16 addr, u8 value) override {
        if (addr >= 0x6000 && addr < 0x8000) { writePrgRam(addr, value); return; }
        if (addr < 0x8000) return;

        const bool even = (addr & 1) == 0;
        if (addr < 0xA000) {
            if (even) {
                bankSelect = value;
            } else if ((bankSelect & 7) < 6) {
                regs[bankSelect & 7] = value;
            } else {
                regs[bankSelect & 7] = value & 0x3F;   // PRG banks mask to 6 bits
            }
        } else if (addr < 0xC000) {
            if (even) {
                cart.mirroring = (value & 1) ? Mirroring::Horizontal : Mirroring::Vertical;
            } else {
                prgRamEnabled = (value & 0x80) != 0;
            }
        } else if (addr < 0xE000) {
            if (even)      irqLatch = value;
            else           irqCounter = 0;          // writing $C001 reloads at next tick
        } else {
            if (even) { irqEnabled = false; irqPending = false; }
            else      { irqEnabled = true; }
        }
    }

    u8 ppuRead(u16 addr) override {
        addr &= 0x1FFF;
        const bool chrMode = (bankSelect & 0x80) != 0;   // 1 KiB banks in upper half
        const u32 banks1k = static_cast<u32>(cart.chr.size() / 1024);
        u32 bank;
        u32 off;

        if (addr < 0x0400)      { bank = chrMode ? regs[2] : (regs[0] & 0xFE);     off = addr & 0x03FF; }
        else if (addr < 0x0800) { bank = chrMode ? regs[3] : (regs[0] & 0xFE);     off = addr & 0x03FF; }
        else if (addr < 0x0C00) { bank = chrMode ? regs[4] : (regs[1] & 0xFE);     off = addr & 0x03FF; }
        else if (addr < 0x1000) { bank = chrMode ? regs[5] : (regs[1] & 0xFE);     off = addr & 0x03FF; }
        else if (addr < 0x1400) { bank = chrMode ? (regs[0] & 0xFE) : regs[2];     off = addr & 0x03FF; }
        else if (addr < 0x1800) { bank = chrMode ? (regs[0] & 0xFE) : regs[3];     off = addr & 0x03FF; }
        else if (addr < 0x1C00) { bank = chrMode ? (regs[1] & 0xFE) : regs[4];     off = addr & 0x03FF; }
        else                    { bank = chrMode ? (regs[1] & 0xFE) : regs[5];     off = addr & 0x03FF; }

        if (banks1k == 0) return 0;
        return cart.chr[((bank % banks1k) * 1024 + off) % cart.chr.size()];
    }
    void ppuWrite(u16 addr, u8 value) override { writeChr8k(addr & 0x1FFF, value); }

    // The PPU calls this once per visible scanline as A12 rises.
    void scanline() override {
        if (irqCounter == 0 || irqReload) {
            irqCounter = irqLatch;
            irqReload = false;
        } else {
            irqCounter--;
        }
        if (irqCounter == 0 && irqEnabled) irqPending = true;
    }

    bool irqActive() const override { return irqPending; }
    void irqClear() override { irqPending = false; }

    const char* name() const override { return "MMC3"; }

    void serialize(StateWriter& w) override {
        w.pod(bankSelect);
        w.raw(regs, sizeof(regs));
        w.boolean(prgRamEnabled);
        w.pod(irqLatch); w.pod(irqCounter);
        w.boolean(irqReload); w.boolean(irqEnabled); w.boolean(irqPending);
    }
    void deserialize(StateReader& r) override {
        r.pod(bankSelect);
        r.raw(regs, sizeof(regs));
        r.boolean(prgRamEnabled);
        r.pod(irqLatch); r.pod(irqCounter);
        r.boolean(irqReload); r.boolean(irqEnabled); r.boolean(irqPending);
    }

private:
    u8  bankSelect = 0;
    u8  regs[8] = {};
    bool prgRamEnabled = true;
    u8  irqLatch = 0, irqCounter = 0;
    bool irqReload = false, irqEnabled = false, irqPending = false;

    u32 prgOffset(u16 addr) const {
        const u32 banks = static_cast<u32>(cart.prgRom.size() / 8192);
        if (banks == 0) return 0;
        const u32 last = banks - 1;
        const u32 second = (banks >= 2) ? banks - 2 : 0;
        const bool prgMode = (bankSelect & 0x40) != 0;
        u32 bank;
        u32 base;

        if (addr < 0xA000) {
            bank = prgMode ? second : regs[6];
            base = 0x8000;
        } else if (addr < 0xC000) {
            bank = regs[7];
            base = 0xA000;
        } else if (addr < 0xE000) {
            bank = prgMode ? regs[6] : second;
            base = 0xC000;
        } else {
            bank = last;
            base = 0xE000;
        }
        return ((bank % banks) * 8192 + (addr - base)) % cart.prgRom.size();
    }
};

// ===========================================================================
// Mapper 7 - AxROM
// Switchable 32 KiB PRG and a single-screen nametable select (used by games
// that page-flip the whole screen). CHR RAM only.
// ===========================================================================
class Mapper7 : public Mapper {
public:
    explicit Mapper7(Cartridge& c) : Mapper(c) { reset(); }

    void reset() override {
        bank = 0;
        cart.mirroring = Mirroring::SingleLower;
    }

    u8 cpuRead(u16 addr) override {
        if (addr >= 0x6000 && addr < 0x8000) return readPrgRam(addr);
        if (addr < 0x8000) return 0;
        return cart.prgRom[prgOffset(addr) % cart.prgRom.size()];
    }
    void cpuWrite(u16 addr, u8 value) override {
        if (addr >= 0x6000 && addr < 0x8000) { writePrgRam(addr, value); return; }
        if (addr < 0x8000) return;
        bank = value & 0x07;
        cart.mirroring = (value & 0x10) ? Mirroring::SingleUpper : Mirroring::SingleLower;
    }
    u8   ppuRead(u16 addr) override { return readChr8k(addr & 0x1FFF); }
    void ppuWrite(u16 addr, u8 value) override { writeChr8k(addr & 0x1FFF, value); }

    const char* name() const override { return "AxROM"; }

    void serialize(StateWriter& w) override { w.pod(bank); }
    void deserialize(StateReader& r) override { r.pod(bank); }

private:
    u8 bank = 0;
    u32 prgOffset(u16 addr) const {
        const u32 banks = static_cast<u32>(cart.prgRom.size() / 32768);
        if (banks == 0) return addr - 0x8000;
        return ((static_cast<u32>(bank) % banks) * 32768 + (addr - 0x8000))
               % cart.prgRom.size();
    }
};

} // namespace

// ---------------------------------------------------------------------------
Mapper* createMapper(int number, Cartridge& cart) {
    switch (number) {
        case 0:  return new Mapper0(cart);
        case 1:  return new Mapper1(cart);
        case 2:  return new Mapper2(cart);
        case 3:  return new Mapper3(cart);
        case 4:  return new Mapper4(cart);
        case 7:  return new Mapper7(cart);
        default: return nullptr;
    }
}

} // namespace nes
