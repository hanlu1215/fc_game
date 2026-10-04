#pragma once

#include <string>

#include "core/console.h"
#include "core/types.h"

namespace nes {

// Save states are written as a chunked container:
//
//   "FCST" | version | PRG CRC32 | payload CRC32 | chunks...
//
// Every chunk is  id(u32) size(u32) payload. The PRG checksum ties a state to
// the exact cartridge image, which is the single best guard against loading a
// state into the wrong game and producing a wild jump.
class SaveState {
public:
    static constexpr u32 kMagic   = 0x54534346;   // "FCST"
    static constexpr u32 kVersion = 2;   // bumped: v1 states came from a build with a broken BMI

    // Directory holding every save for one cartridge.
    static std::string slotDir(const Cartridge& cart);
    static std::string slotPath(const Cartridge& cart, int slot);

    static bool save(const Console& con, const Cartridge& cart, int slot, std::string& err);
    static bool load(Console& con, const Cartridge& cart, int slot, std::string& err);

    static bool exists(const Cartridge& cart, int slot);
    static u64  timestamp(const Cartridge& cart, int slot);
};

} // namespace nes
