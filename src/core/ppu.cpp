#include "core/ppu.h"

namespace nes {

// ---------------------------------------------------------------------------
// 2C02 palette. Values are approximations of the NTSC signal as commonly
// rendered by emulators; only the low 6 bits of a palette entry are used.
// ---------------------------------------------------------------------------
static const u32 kPalette[64] = {
    0x626262, 0x001FB2, 0x2404C8, 0x5200CC, 0x7300B5, 0x800D6D, 0x7A1B00, 0x663A00,
    0x456000, 0x0E7200, 0x007A00, 0x006A1E, 0x005C4E, 0x000000, 0x000000, 0x000000,
    0xABABAB, 0x0B51FF, 0x4B36FF, 0x8A2BFF, 0xB320E8, 0xD01C9B, 0xCC3A26, 0xB06000,
    0x868A00, 0x4D9E00, 0x16A800, 0x009D45, 0x008E86, 0x000000, 0x000000, 0x000000,
    0xFFFFFF, 0x5FA8FF, 0x8E8CFF, 0xC37AFF, 0xE95FF7, 0xFF5FB0, 0xFF7A45, 0xF0A400,
    0xC8C400, 0x8FD500, 0x5BE04A, 0x3ADD8A, 0x3BD8C8, 0x4E4E4E, 0x000000, 0x000000,
    0xFFFFFF, 0xB6DEFF, 0xCCCCFF, 0xE3C2FF, 0xF5BAFF, 0xFFB8E8, 0xFFC2B0, 0xFFD699,
    0xEEEE99, 0xD9F299, 0xC2F5A2, 0xB4F5BE, 0xB4F5D8, 0xB0B0B0, 0x000000, 0x000000,
};

const u32* PPU::paletteRGB() { return kPalette; }

// ---------------------------------------------------------------------------
void PPU::reset() {
    memset(vram, 0, sizeof(vram));
    memset(oam, 0, sizeof(oam));
    memset(fb, 0, sizeof(fb));
    for (int i = 0; i < 32; i++) palRam[i] = (i % 4 == 0) ? 0x0F : 0x00;

    v = t = 0;
    fineX = 0;
    writeToggle = false;
    readBuffer = 0;
    ioBus = 0;
    ctrl = mask = status = 0;
    oamAddr = 0;

    scanline = 261;
    dot = 0;
    frameReady = false;
    oddFrame = false;

    bgShiftLo = bgShiftHi = atShiftLo = atShiftHi = 0;
    ntLatch = atLatch = bgLoLatch = bgHiLatch = 0;
    spriteCount = 0;
    memset(spriteLine, 0, sizeof(spriteLine));
}

// ---------------------------------------------------------------------------
// PPU address space: $0000-$1FFF cartridge, $2000-$3EFF nametables,
// $3F00-$3FFF palette.
// ---------------------------------------------------------------------------
u16 PPU::nametableIndex(u16 addr) const {
    const u16 off = addr & 0x03FF;
    const int n = (addr >> 10) & 3;          // which of the four nametables

    if (!cart) return off;
    if (cart->fourScreen) return static_cast<u16>(n * 0x400 + off);

    switch (cart->mirroring) {
        // "Vertical": CIRAM A10 = PPU A10 -> NT0==NT2, NT1==NT3.
        case Mirroring::Vertical:    return static_cast<u16>((n & 1) * 0x400 + off);
        // "Horizontal": CIRAM A10 = PPU A11 -> NT0==NT1, NT2==NT3.
        case Mirroring::Horizontal:  return static_cast<u16>((n >> 1) * 0x400 + off);
        case Mirroring::SingleLower: return off;
        case Mirroring::SingleUpper: return static_cast<u16>(0x400 + off);
        default:                     return static_cast<u16>((n & 1) * 0x400 + off);
    }
}

u8 PPU::paletteRead(u16 addr) const {
    u16 a = addr & 0x1F;
    // $3F10/$3F14/$3F18/$3F1C mirror $3F00/$3F04/$3F08/$3F0C.
    if (a >= 0x10 && (a % 4) == 0) a -= 0x10;
    return palRam[a];
}

void PPU::paletteWrite(u16 addr, u8 value) {
    u16 a = addr & 0x1F;
    if (a >= 0x10 && (a % 4) == 0) a -= 0x10;
    palRam[a] = value & 0x3F;
}

u8 PPU::ppuBusRead(u16 addr) {
    addr &= 0x3FFF;
    if (addr < 0x2000) return mapper ? mapper->ppuRead(addr) : 0;
    if (addr < 0x3F00) return vram[nametableIndex(addr - 0x2000)];
    return paletteRead(addr);
}

void PPU::ppuBusWrite(u16 addr, u8 value) {
    addr &= 0x3FFF;
    if (addr < 0x2000) { if (mapper) mapper->ppuWrite(addr, value); return; }
    if (addr < 0x3F00) { vram[nametableIndex(addr - 0x2000)] = value; return; }
    paletteWrite(addr, value);
}

// ---------------------------------------------------------------------------
// CPU-facing registers
// ---------------------------------------------------------------------------
u8 PPU::readRegister(u16 addr) {
    switch (addr & 7) {
        case 2: {   // $2002 PPUSTATUS
            const u8 result = static_cast<u8>((status & 0xE0) | (ioBus & 0x1F));
            status &= ~0x80;        // reading clears the vblank flag
            writeToggle = false;    // and resets the address/scroll latch
            ioBus = result;
            updateNmiLine();
            return result;
        }
        case 4: {   // $2004 OAMDATA
            const u8 result = oam[oamAddr];
            ioBus = result;
            return result;
        }
        case 7: {   // $2007 PPUDATA - buffered read
            u16 a = v & 0x3FFF;
            u8 result;
            if (a >= 0x3F00) {
                // Palette reads come back immediately; the buffer is refilled
                // from the nametable mirror underneath.
                result = paletteRead(a);
                readBuffer = vram[nametableIndex(static_cast<u16>(a - 0x1000) - 0x2000)];
            } else {
                result = readBuffer;
                readBuffer = ppuBusRead(a);
            }
            v = static_cast<u16>((v + ((ctrl & 0x04) ? 32 : 1)) & 0x7FFF);
            ioBus = result;
            return result;
        }
        default:
            return ioBus;
    }
}

void PPU::writeRegister(u16 addr, u8 value) {
    ioBus = value;
    switch (addr & 7) {
        case 0:     // $2000 PPUCTRL
            ctrl = value;
            t = static_cast<u16>((t & ~0x0C00) | ((value & 0x03) << 10));
            // Enabling NMI while vblank is already flagged raises the line
            // immediately; games that enable NMI late rely on this.
            updateNmiLine();
            break;

        case 1:     // $2001 PPUMASK
            mask = value;
            break;

        case 3:     // $2003 OAMADDR
            oamAddr = value;
            break;

        case 4:     // $2004 OAMDATA
            oam[oamAddr++] = value;
            break;

        case 5:     // $2005 PPUSCROLL - two writes, X first then Y
            if (!writeToggle) {
                fineX = static_cast<u8>(value & 0x07);
                t = static_cast<u16>((t & ~0x001F) | (value >> 3));
                writeToggle = true;
            } else {
                t = static_cast<u16>((t & ~0x73E0)
                    | ((value & 0x07) << 12)
                    | (static_cast<u16>(value & 0xF8) << 2));
                writeToggle = false;
            }
            break;

        case 6:     // $2006 PPUADDR - two writes, high byte then low
            if (!writeToggle) {
                // Bit 14 of t is forced clear by the first write.
                t = static_cast<u16>((t & 0x00FF) | ((value & 0x3F) << 8));
                writeToggle = true;
            } else {
                t = static_cast<u16>((t & 0xFF00) | value);
                v = static_cast<u16>(t & 0x7FFF);
                writeToggle = false;
            }
            break;

        case 7:     // $2007 PPUDATA
            ppuBusWrite(v & 0x3FFF, value);
            v = static_cast<u16>((v + ((ctrl & 0x04) ? 32 : 1)) & 0x7FFF);
            break;

        default:
            break;
    }
}

// ---------------------------------------------------------------------------
// Background fetch pipeline
// ---------------------------------------------------------------------------
void PPU::loadShifters() {
    bgShiftLo = static_cast<u16>((bgShiftLo & 0xFF00) | bgLoLatch);
    bgShiftHi = static_cast<u16>((bgShiftHi & 0xFF00) | bgHiLatch);
    atShiftLo = static_cast<u16>((atShiftLo & 0xFF00) | ((atLatch & 0x01) ? 0xFF : 0x00));
    atShiftHi = static_cast<u16>((atShiftHi & 0xFF00) | ((atLatch & 0x02) ? 0xFF : 0x00));
}

void PPU::updateShifters() {
    bgShiftLo = static_cast<u16>(bgShiftLo << 1);
    bgShiftHi = static_cast<u16>(bgShiftHi << 1);
    atShiftLo = static_cast<u16>(atShiftLo << 1);
    atShiftHi = static_cast<u16>(atShiftHi << 1);
}

void PPU::fetchNametable() {
    ntLatch = ppuBusRead(static_cast<u16>(0x2000 | (v & 0x0FFF)));
}

void PPU::fetchAttribute() {
    const u16 addr = static_cast<u16>(0x23C0 | (v & 0x0C00)
                    | ((v >> 4) & 0x38) | ((v >> 2) & 0x07));
    u8 at = ppuBusRead(addr);
    // Pick the 2-bit quadrant this tile falls in.
    if (v & 0x40) at >>= 4;         // bottom half of the 32x32 attribute block
    if (v & 0x02) at >>= 2;         // right half
    atLatch = static_cast<u8>(at & 0x03);
}

void PPU::fetchPatternLow() {
    const u16 base = (ctrl & 0x10) ? 0x1000 : 0x0000;
    const u16 addr = static_cast<u16>(base + ntLatch * 16 + ((v >> 12) & 7));
    bgLoLatch = ppuBusRead(addr);
}

void PPU::fetchPatternHigh() {
    const u16 base = (ctrl & 0x10) ? 0x1000 : 0x0000;
    const u16 addr = static_cast<u16>(base + ntLatch * 16 + ((v >> 12) & 7) + 8);
    bgHiLatch = ppuBusRead(addr);
}

void PPU::incrementX() {
    if ((v & 0x001F) == 31) { v &= ~0x001F; v ^= 0x0400; }
    else v++;
}

void PPU::incrementY() {
    if ((v & 0x7000) != 0x7000) {
        v = static_cast<u16>(v + 0x1000);
    } else {
        v &= ~0x7000;
        int y = (v & 0x03E0) >> 5;
        if (y == 29)      { y = 0; v ^= 0x0800; }
        else if (y == 31) { y = 0; }
        else              y++;
        v = static_cast<u16>((v & ~0x03E0) | (y << 5));
    }
}

// ---------------------------------------------------------------------------
// Sprites
// ---------------------------------------------------------------------------
void PPU::evaluateSprites(int nextScanline) {
    spriteCount = 0;
    const int height = (ctrl & 0x20) ? 16 : 8;
    bool overflow = false;

    for (int i = 0; i < 64; i++) {
        const int row = nextScanline - oam[i * 4 + 0];
        if (row < 0 || row >= height) continue;

        if (spriteCount < 8) {
            SpriteSlot& s = spriteLine[spriteCount];
            s.x = oam[i * 4 + 3];
            s.attr = oam[i * 4 + 2];
            s.tile = oam[i * 4 + 1];
            s.row = static_cast<u8>(row);
            s.sprite0 = (i == 0);
            spriteCount++;
        } else {
            overflow = true;    // more than 8 sprites on this line
        }
    }
    if (overflow) status |= 0x20;
}

void PPU::fetchSpritePatterns() {
    const bool big = (ctrl & 0x20) != 0;
    const int height = big ? 16 : 8;

    for (int i = 0; i < spriteCount; i++) {
        SpriteSlot& s = spriteLine[i];

        int r = s.row;
        if (s.attr & 0x80) r = height - 1 - r;   // vertical flip

        u16 addr;
        if (!big) {
            const u16 base = (ctrl & 0x08) ? 0x1000 : 0x0000;
            addr = static_cast<u16>(base + s.tile * 16 + r);
        } else {
            // 8x16: the tile index's low bit picks the 4 KiB pattern table,
            // and the index's high bit picks the 16-pixel half.
            const u16 base = (s.tile & 0x01) ? 0x1000 : 0x0000;
            u16 tile = static_cast<u16>(s.tile & 0xFE);
            if (r >= 8) { tile++; r -= 8; }
            addr = static_cast<u16>(base + tile * 16 + r);
        }

        s.patLo = ppuBusRead(addr);
        s.patHi = ppuBusRead(static_cast<u16>(addr + 8));
    }
}

// ---------------------------------------------------------------------------
// NMI output line: level = vblank flag AND $2000 bit 7. The CPU reacts to the
// rising edge, which is why enabling NMI while vblank is already set fires one
// immediately, and why reading $2002 then rewriting $2000 fires another.
// ---------------------------------------------------------------------------
void PPU::updateNmiLine() {
    const bool line = (status & 0x80) != 0 && (ctrl & 0x80) != 0;
    if (line && !nmiLinePrev && onNmi) onNmi();
    nmiLinePrev = line;
}

// ---------------------------------------------------------------------------
// Pixel composition
// ---------------------------------------------------------------------------
void PPU::renderPixel(int x) {
    const bool bgEnabled = (mask & 0x08) != 0;
    const bool spEnabled = (mask & 0x10) != 0;
    const bool bgLeft    = (mask & 0x02) != 0;
    const bool spLeft    = (mask & 0x04) != 0;

    u8 bgPixel = 0, bgPal = 0;
    if (bgEnabled && (x >= 8 || bgLeft)) {
        const u16 mux = static_cast<u16>(0x8000 >> fineX);
        bgPixel = static_cast<u8>(((bgShiftLo & mux) ? 1 : 0)
                                | ((bgShiftHi & mux) ? 2 : 0));
        bgPal = static_cast<u8>(((atShiftLo & mux) ? 1 : 0)
                              | ((atShiftHi & mux) ? 2 : 0));
    }

    u8 spPixel = 0, spPal = 0;
    bool spIsSprite0 = false;
    bool spBehind = false;
    if (spEnabled && (x >= 8 || spLeft)) {
        for (int i = 0; i < spriteCount; i++) {
            const SpriteSlot& s = spriteLine[i];
            const int offset = x - s.x;
            if (offset < 0 || offset > 7) continue;
            // OAM attribute bit 6 mirrors the tile horizontally. Pattern bit 7
            // is the leftmost pixel, so a flipped sprite reads from bit 0 up.
            const u8 bit = (s.attr & 0x40) ? static_cast<u8>(offset)
                                           : static_cast<u8>(7 - offset);
            const u8 px = static_cast<u8>(((s.patLo >> bit) & 1) | (((s.patHi >> bit) & 1) << 1));
            if (px == 0) continue;
            spPixel = px;
            spPal = static_cast<u8>(s.attr & 0x03);
            spBehind = (s.attr & 0x20) != 0;
            spIsSprite0 = s.sprite0;
            break;      // lower OAM index wins
        }
    }

    // Sprite 0 hit: an opaque sprite-0 pixel over an opaque background pixel.
    // Both must actually be visible (left-edge clipping suppresses it).
    if (spIsSprite0 && bgPixel != 0 && spPixel != 0) {
        if (x < 255) status |= 0x40;
    }

    u8 colorIndex;
    if (bgPixel == 0 && spPixel == 0)      colorIndex = 0;
    else if (bgPixel == 0)                 colorIndex = static_cast<u8>(0x10 + spPal * 4 + spPixel);
    else if (spPixel == 0)                 colorIndex = static_cast<u8>(bgPal * 4 + bgPixel);
    else if (spBehind)                     colorIndex = static_cast<u8>(bgPal * 4 + bgPixel);
    else                                   colorIndex = static_cast<u8>(0x10 + spPal * 4 + spPixel);

    u8 palEntry = paletteRead(colorIndex);
    if (mask & 0x01) palEntry &= 0x30;      // greyscale

    fb[scanline * 256 + x] = kPalette[palEntry & 0x3F];
}

// ---------------------------------------------------------------------------
// Main clock
// ---------------------------------------------------------------------------
void PPU::clock() {
    const bool rendering = renderingEnabled();
    const bool visible = (scanline >= 0 && scanline < 240);
    const bool preRender = (scanline == 261);

    // ---- start-of-dot events -------------------------------------------
    if (preRender && dot == 1) {
        status &= ~0xE0;            // clear vblank, sprite 0, overflow
    }
    if (scanline == 241 && dot == 1) {
        status |= 0x80;             // vblank begins
        frameReady = true;          // the framebuffer is now stable
    }

    // ---- background fetch pipeline --------------------------------------
    // The shift registers advance ONLY inside the fetch windows. During the
    // sprite fetch (dots 258-320) they must hold still, otherwise the tile
    // data is shifted clean out of the 16-bit registers before it is shown.
    if ((visible || preRender) && rendering) {
        if ((dot >= 2 && dot < 258) || (dot >= 321 && dot < 338)) {
            updateShifters();
            switch ((dot - 1) % 8) {
                case 0: loadShifters(); fetchNametable(); break;
                case 2: fetchAttribute(); break;
                case 4: fetchPatternLow(); break;
                case 6: fetchPatternHigh(); break;
                case 7: incrementX(); break;
                default: break;
            }
        }
        if (dot == 256) incrementY();
        if (dot == 257) { loadShifters(); copyX(); }
        if (preRender && dot >= 280 && dot <= 304) copyY();
        if (dot == 338 || dot == 340) fetchNametable();
    }

    // ---- pixel output ----------------------------------------------------
    if (visible && dot >= 1 && dot <= 256) {
        renderPixel(dot - 1);
    }

    // ---- sprite work for the next scanline -------------------------------
    if ((visible || preRender) && dot == 257) {
        const int next = (scanline == 261) ? 0 : scanline + 1;
        evaluateSprites(next);
        fetchSpritePatterns();
    }

    // MMC3 clocks its scanline counter as A12 rises, which in the common
    // CHR layout happens once per rendered scanline.
    if ((visible || preRender) && dot == 260 && rendering && mapper) {
        mapper->scanline();
    }

    // ---- NMI line --------------------------------------------------------
    updateNmiLine();

    // ---- advance to the next dot ----------------------------------------
    if (scanline == 261 && dot == 340) {
        scanline = 0;
        // Odd frames with rendering enabled are one dot short: dot 0 of
        // scanline 0 is skipped.
        dot = (oddFrame && rendering) ? 1 : 0;
        oddFrame = !oddFrame;
    } else if (dot == 340) {
        dot = 0;
        scanline++;
    } else {
        dot++;
    }
}

void PPU::step(u32 dots) {
    while (dots--) clock();
}

// ---------------------------------------------------------------------------
// Save states
// ---------------------------------------------------------------------------
void PPU::serialize(StateWriter& w) const {
    w.pod(v); w.pod(t); w.pod(fineX); w.boolean(writeToggle);
    w.pod(readBuffer); w.pod(ioBus);
    w.pod(ctrl); w.pod(mask); w.pod(status); w.pod(oamAddr);
    w.pod(scanline); w.pod(dot); w.boolean(oddFrame);
    w.pod(bgShiftLo); w.pod(bgShiftHi); w.pod(atShiftLo); w.pod(atShiftHi);
    w.pod(ntLatch); w.pod(atLatch); w.pod(bgLoLatch); w.pod(bgHiLatch);
    w.pod(spriteCount);
    w.raw(spriteLine, sizeof(spriteLine));
    w.raw(vram, sizeof(vram));
    w.raw(palRam, sizeof(palRam));
    w.raw(oam, sizeof(oam));
}

void PPU::deserialize(StateReader& r) {
    r.pod(v); r.pod(t); r.pod(fineX); r.boolean(writeToggle);
    r.pod(readBuffer); r.pod(ioBus);
    r.pod(ctrl); r.pod(mask); r.pod(status); r.pod(oamAddr);
    r.pod(scanline); r.pod(dot); r.boolean(oddFrame);
    r.pod(bgShiftLo); r.pod(bgShiftHi); r.pod(atShiftLo); r.pod(atShiftHi);
    r.pod(ntLatch); r.pod(atLatch); r.pod(bgLoLatch); r.pod(bgHiLatch);
    r.pod(spriteCount);
    r.raw(spriteLine, sizeof(spriteLine));
    r.raw(vram, sizeof(vram));
    r.raw(palRam, sizeof(palRam));
    r.raw(oam, sizeof(oam));
    frameReady = false;
}

} // namespace nes
