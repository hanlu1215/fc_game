#pragma once

#include <functional>

#include "core/cartridge.h"
#include "core/mapper.h"
#include "core/types.h"

namespace nes {

// Ricoh 2C02 picture processing unit.
//
// The PPU is clocked at three times the CPU rate. It is modelled at dot
// granularity so that mid-frame scroll changes ($2005/$2006 writes during
// horizontal blanking, the technique behind split-screen status bars) behave
// correctly.
class PPU {
public:
    PPU() { reset(); }

    void setCartridge(Cartridge* c) { cart = c; }
    void setMapper(Mapper* m) { mapper = m; }
    void setNmiCallback(std::function<void()> cb) { onNmi = std::move(cb); }

    void reset();

    // Advance the PPU by `dots` dots (1/3 of a CPU cycle each).
    void step(u32 dots);

    // True once per frame, when scanline wraps back to the pre-render line.
    bool consumeFrameFlag() { bool f = frameReady; frameReady = false; return f; }

    const u32* framebuffer() const { return fb; }

    u8   readRegister(u16 addr);
    void writeRegister(u16 addr, u8 value);

    // Direct OAM access, used by the $4014 DMA path.
    u8   oamRead(u16 addr) const { return oam[addr & 0xFF]; }
    void oamWrite(u16 addr, u8 value) { oam[addr & 0xFF] = value; }
    bool renderingEnabled() const { return (mask & 0x18) != 0; }

    // Read-only views of the registers, for the on-screen debug overlay.
    u8 ctrlValue() const { return ctrl; }
    u8 maskValue() const { return mask; }
    u8 statusValue() const { return status; }
    u8 dbgVram(u16 addr) { return ppuBusRead(static_cast<u16>(addr & 0x3FFF)); }
    u8 dbgPal(int i) const { return palRam[i & 0x1F]; }
    int dbgScanline() const { return scanline; }

    void serialize(StateWriter& w) const;
    void deserialize(StateReader& r);

    // Palette lookup: 64 NES colours as 0x00RRGGBB.
    static const u32* paletteRGB();

private:
    Cartridge* cart = nullptr;
    Mapper* mapper = nullptr;
    std::function<void()> onNmi;

    // ---- memory -----------------------------------------------------------
    u8  vram[4096] = {};        // 2 KiB nametables (4 KiB when four-screen)
    u8  palRam[32] = {};        // palette RAM
    u8  oam[256] = {};          // primary object attribute memory
    u32 fb[256 * 240] = {};     // RGB framebuffer

    // ---- registers --------------------------------------------------------
    u16 v = 0;                  // current VRAM address (15 bits)
    u16 t = 0;                  // temporary VRAM address (15 bits)
    u8  fineX = 0;              // fine X scroll (3 bits)
    bool writeToggle = false;   // shared by $2005/$2006
    u8  readBuffer = 0;         // buffered $2007 read
    u8  ioBus = 0;              // PPU open bus, for the low bits of $2002

    u8 ctrl = 0;                // $2000
    u8 mask = 0;                // $2001
    u8 status = 0;              // $2002
    u8 oamAddr = 0;             // $2003

    int scanline = 261;         // 261 is the pre-render line
    int dot = 0;
    bool frameReady = false;
    bool oddFrame = false;
    bool nmiLinePrev = false;   // for edge detection on the NMI line

    // ---- background pipeline ---------------------------------------------
    u16 bgShiftLo = 0, bgShiftHi = 0;
    u16 atShiftLo = 0, atShiftHi = 0;
    u8  ntLatch = 0;            // nametable byte being fetched
    u8  atLatch = 0;            // attribute byte being fetched
    u8  bgLoLatch = 0, bgHiLatch = 0;   // pattern low/high being fetched

    // ---- sprite pipeline --------------------------------------------------
    struct SpriteSlot {
        u8  x = 0;
        u8  attr = 0;
        u8  row = 0;            // row within the sprite, 0..height-1
        u8  tile = 0;           // tile index from OAM
        u8  patLo = 0, patHi = 0;
        bool sprite0 = false;
    };
    SpriteSlot spriteLine[8];
    int spriteCount = 0;

    // ---- internals --------------------------------------------------------
    void clock();
    void renderPixel(int x);
    void evaluateSprites(int nextScanline);
    void fetchSpritePatterns();
    // The NMI output is a level (vblank AND $2000 bit 7), not a pulse; the
    // CPU sees an edge. Re-evaluated every dot and after every register write.
    void updateNmiLine();

    u8   ppuBusRead(u16 addr);
    void ppuBusWrite(u16 addr, u8 value);
    u8   paletteRead(u16 addr) const;
    void paletteWrite(u16 addr, u8 value);
    u16  nametableIndex(u16 addr) const;

    void incrementX();
    void incrementY();
    void copyX() { v = static_cast<u16>((v & ~0x041F) | (t & 0x041F)); }
    void copyY() { v = static_cast<u16>((v & ~0x7BE0) | (t & 0x7BE0)); }

    void loadShifters();
    void updateShifters();
    void fetchNametable();
    void fetchAttribute();
    void fetchPatternLow();
    void fetchPatternHigh();
};

} // namespace nes
