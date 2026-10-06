#pragma once

#include <string>
#include <vector>

#include "core/types.h"

namespace nes {

// Frontend key codes. X11 keysyms are translated into these so the rest of the
// UI never has to know which windowing system it is running on.
enum class Key {
    Unknown,
    Up, Down, Left, Right,
    Enter, Escape, Tab, Space, Backspace,
    LeftShift, RightShift, LeftCtrl, RightCtrl,
    F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
    A, B, C, D, E, F, G, H, I, J, K, L, M,
    N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
    Num0, Num1, Num2, Num3, Num4, Num5, Num6, Num7, Num8, Num9,
    Comma, Period, Slash, Minus, Equal,
    COUNT
};

struct KeyEvent {
    Key  key = Key::Unknown;
    bool pressed = false;
    bool ctrl = false;
    bool alt = false;      // Mod1, i.e. the usual Alt key
};

// An X11 window with an XShm-backed canvas.
//
// The NES picture is scaled up into `canvas`, and the menus draw directly into
// that same buffer at window resolution, so text stays crisp instead of being
// stretched along with the game image.
class Platform {
public:
    ~Platform() { shutdown(); }

    bool init(int scale, const std::string& title);
    void shutdown();

    // Pumps X events; returns false when there are no more pending events.
    bool pollEvent(KeyEvent& ev);

    // Scales the 256x240 NES picture into the canvas.
    void beginFrame(const u32* nesFb);

    u32* canvas() { return canvas_.data(); }
    int  canvasW() const { return cw_; }
    int  canvasH() const { return ch_; }
    int  gameW() const { return 256 * scale_; }
    int  gameH() const { return 240 * scale_; }
    int  gameX() const { return offX_; }
    int  gameY() const { return offY_; }

    void present();
    void setTitle(const std::string& t);
    void toggleFullscreen();

    bool shouldClose() const { return wantClose_; }
    bool ok() const { return dpy_ != nullptr; }
    int  scale() const { return scale_; }

private:
    struct Impl;
    Impl* impl_ = nullptr;

    void* dpy_ = nullptr;      // Display*
    unsigned long win_ = 0;
    bool wantClose_ = false;
    bool fullscreen_ = false;
    int  scale_ = 3;
    int  cw_ = 0, ch_ = 0;
    int  offX_ = 0, offY_ = 0;
    std::vector<u32> canvas_;
    std::vector<u32> scaledRow_;

    void allocCanvas(int w, int h);
    void recenter();
};

} // namespace nes
