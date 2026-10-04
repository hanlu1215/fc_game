#pragma once

#include "core/types.h"

namespace nes {

class Mapper;

// A parsed .nes image plus the board wiring it implies.
//
// The cartridge owns the ROM/RAM arrays; the mapper only decides which slice
// of them a given CPU/PPU address maps onto.
class Cartridge {
public:
    std::vector<u8> prgRom;     // PRG ROM, size is a multiple of 16 KiB
    std::vector<u8> chr;        // CHR ROM, or CHR RAM when chrIsRam is set
    std::vector<u8> prgRam;     // work RAM at $6000-$7FFF (battery-backed if `battery`)
    bool chrIsRam = false;

    Mirroring mirroring = Mirroring::Horizontal;
    bool battery = false;
    bool fourScreen = false;
    int  mapperNumber = 0;
    u32  prgCrc = 0;
    std::string path;
    std::string displayName;

    bool loadFromFile(const std::string& file, std::string& err);
    bool loadFromMemory(const std::vector<u8>& image, std::string& err);

    // Battery-backed PRG RAM persistence (<rom>.sav next to the ROM).
    std::string batteryPath() const;
    bool saveBattery() const;
    void loadBattery();

    u32  prgSize() const { return static_cast<u32>(prgRom.size()); }
    u32  chrSize() const { return static_cast<u32>(chr.size()); }

    static u32 crc32(const u8* data, size_t n);
};

} // namespace nes
