#pragma once

#include <memory>
#include <string>
#include <vector>

#include "core/apu.h"
#include "core/bus.h"
#include "core/cartridge.h"
#include "core/controller.h"
#include "core/cpu.h"
#include "core/mapper.h"
#include "core/ppu.h"

namespace nes {

// A whole NES: CPU, PPU, APU, cartridge and the buses between them.
//
// The CPU runs one instruction at a time and the PPU/APU are then advanced by
// the matching number of cycles. That keeps the machine in step closely enough
// for the games this targets without the cost of a fully cycle-accurate bus.
class Console {
public:
    Console();

    bool loadROM(const std::string& path, std::string& err);
    void reset();

    // Runs until the PPU reports a completed frame.
    void stepFrame();

    bool loaded() const { return mapper != nullptr; }

    const u32* framebuffer() const { return ppu.framebuffer(); }
    void drainAudio(std::vector<float>& out) { apu.drainSamples(out); }

    Controller& controller(int port) { return ctrls[port & 1]; }
    Cartridge&  cartridge() { return cart; }
    PPU&        ppuRef() { return ppu; }
    CPU&        cpuRef() { return cpu; }
    Bus&        busRef() { return bus; }
    APU&        apuRef() { return apu; }

    // Persist battery-backed cartridge RAM, if the board has any.
    void saveBattery() { cart.saveBattery(); }

    void serialize(StateWriter& w) const;
    void deserialize(StateReader& r);

    u64 frameCount() const { return frames; }

private:
    // Declaration order matters: the bus and CPU reference the ones above.
    Cartridge cart;
    std::unique_ptr<Mapper> mapper;
    Controller ctrls[2];
    PPU ppu;
    APU apu;
    Bus bus;
    CPU cpu;

    u64 frames = 0;
};

} // namespace nes
