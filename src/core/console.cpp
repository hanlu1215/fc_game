#include "core/console.h"

namespace nes {

Console::Console() : bus(ppu, apu, ctrls), cpu(bus) {
    ppu.setNmiCallback([this] { cpu.nmi(); });
    // The DMC fetches its sample bytes through the CPU bus.
    apu.setMemoryReader([this](u16 addr) { return bus.read(addr); });
    apu.setSampleRate(AUDIO_RATE);
}

bool Console::loadROM(const std::string& path, std::string& err) {
    // Load straight into the member: the mapper stores a reference to it, so
    // it must not be moved or replaced afterwards.
    Cartridge fresh;
    if (!fresh.loadFromFile(path, err)) return false;

    std::unique_ptr<Mapper> newMapper(createMapper(fresh.mapperNumber, fresh));
    if (!newMapper) {
        err = "暂不支持该卡带的 mapper 类型（编号 "
            + std::to_string(fresh.mapperNumber) + "）";
        return false;
    }

    cart = std::move(fresh);
    // Re-create against the member so the reference points at the live object.
    mapper.reset(createMapper(cart.mapperNumber, cart));
    cart.loadBattery();

    reset();
    return true;
}

void Console::reset() {
    bus.reset();
    bus.setMapper(mapper.get());
    ppu.setCartridge(&cart);
    ppu.setMapper(mapper.get());
    if (mapper) mapper->reset();
    ppu.reset();
    apu.reset();
    cpu.reset();
    frames = 0;
}

void Console::stepFrame() {
    // Generous guard against a ROM that somehow never reaches vblank.
    u32 guard = 0;
    while (!ppu.consumeFrameFlag()) {
        const bool irq = apu.irqActive() || (mapper && mapper->irqActive());
        cpu.setIrqLine(irq);

        u32 cycles = cpu.step();
        cycles += bus.takeDmaStall();

        ppu.step(cycles * 3);
        apu.step(cycles);

        if (++guard > 400000) break;
    }
    frames++;
}

// ---------------------------------------------------------------------------
void Console::serialize(StateWriter& w) const {
    w.pod(frames);

    // CPU + work RAM
    cpu.serialize(w);
    w.blob(bus.ramData(), Bus::ramSize());

    // PPU
    ppu.serialize(w);

    // APU
    apu.serialize(w);

    // Cartridge work RAM + CHR RAM (CHR ROM is immutable and comes from the file)
    w.blob(cart.prgRam.data(), cart.prgRam.size());
    if (cart.chrIsRam) w.blob(cart.chr.data(), cart.chr.size());

    // Mapper
    if (mapper) mapper->serialize(w);

    // Controllers
    for (int i = 0; i < 2; i++) ctrls[i].serialize(w);

    // Mirroring can be changed by the mapper at runtime.
    w.pod(cart.mirroring);
}

void Console::deserialize(StateReader& r) {
    r.pod(frames);

    cpu.deserialize(r);
    r.blob(bus.ramData(), Bus::ramSize());

    ppu.deserialize(r);
    apu.deserialize(r);

    r.blob(cart.prgRam.data(), cart.prgRam.size());
    if (cart.chrIsRam) r.blob(cart.chr.data(), cart.chr.size());

    if (mapper) mapper->deserialize(r);

    for (int i = 0; i < 2; i++) ctrls[i].deserialize(r);

    r.pod(cart.mirroring);
}

} // namespace nes
