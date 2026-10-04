#pragma once

#include "core/types.h"

namespace nes {

// Standard NES controller ($4016 / $4017).
//
// Reading the register returns one button per read, LSB first, in this order:
//   A, B, Select, Start, Up, Down, Left, Right
// After the 8th read the shift register returns 1s until strobe goes high
// again. Writing 1 to $4016 latches the current button state.
class Controller {
public:
    enum Button { A = 0, B, Select, Start, Up, Down, Left, Right, COUNT };

    void setButton(Button b, bool pressed) {
        // setBit keeps the two edge-detect masks below cheap.
        state = setBit(state, b, pressed);
    }
    bool button(Button b) const { return getBit(state, b); }
    u8 rawState() const { return state; }

    void strobe(bool on) {
        if (on) shift = state;
        // On the falling edge the register stays loaded; it is advanced by reads.
    }

    u8 read() {
        u8 v = (shift & 1) | 0x40;   // bit 6 is the open-bus / expansion bit
        shift = static_cast<u8>((shift >> 1) | 0x80);
        return v;
    }

    void serialize(StateWriter& w) const { w.pod(state); w.pod(shift); }
    void deserialize(StateReader& r) { r.pod(state); r.pod(shift); }

private:
    u8 state = 0;
    u8 shift = 0;
};

} // namespace nes
