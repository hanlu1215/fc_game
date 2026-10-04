#include "ui/platform.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/XKBlib.h>
#include <X11/extensions/XShm.h>

#include <sys/ipc.h>
#include <sys/shm.h>

#include <cstdio>
#include <cstring>
#include <unordered_map>

namespace nes {

struct Platform::Impl {
    Display* dpy = nullptr;
    Window   win = 0;
    GC       gc = nullptr;
    int      screen = 0;
    Visual*  visual = nullptr;
    int      depth = 24;
    Colormap cmap = 0;

    bool     useShm = false;
    XImage*  image = nullptr;
    XShmSegmentInfo shm {};

    Atom wmDelete = 0;

    std::unordered_map<unsigned long, Key> keymap;
};

// ---------------------------------------------------------------- key table
namespace {

Key keysymToKey(unsigned long ks) {
    switch (ks) {
        case XK_Up: return Key::Up;
        case XK_Down: return Key::Down;
        case XK_Left: return Key::Left;
        case XK_Right: return Key::Right;
        case XK_Return: case XK_KP_Enter: return Key::Enter;
        case XK_Escape: return Key::Escape;
        case XK_Tab: return Key::Tab;
        case XK_space: return Key::Space;
        case XK_BackSpace: return Key::Backspace;
        case XK_Shift_L: return Key::LeftShift;
        case XK_Shift_R: return Key::RightShift;
        case XK_Control_L: return Key::LeftCtrl;
        case XK_Control_R: return Key::RightCtrl;
        case XK_F1: return Key::F1;   case XK_F2: return Key::F2;
        case XK_F3: return Key::F3;   case XK_F4: return Key::F4;
        case XK_F5: return Key::F5;   case XK_F6: return Key::F6;
        case XK_F7: return Key::F7;   case XK_F8: return Key::F8;
        case XK_F9: return Key::F9;   case XK_F10: return Key::F10;
        case XK_F11: return Key::F11; case XK_F12: return Key::F12;
        case XK_a: case XK_A: return Key::A;
        case XK_b: case XK_B: return Key::B;
        case XK_c: case XK_C: return Key::C;
        case XK_d: case XK_D: return Key::D;
        case XK_e: case XK_E: return Key::E;
        case XK_f: case XK_F: return Key::F;
        case XK_g: case XK_G: return Key::G;
        case XK_h: case XK_H: return Key::H;
        case XK_i: case XK_I: return Key::I;
        case XK_j: case XK_J: return Key::J;
        case XK_k: case XK_K: return Key::K;
        case XK_l: case XK_L: return Key::L;
        case XK_m: case XK_M: return Key::M;
        case XK_n: case XK_N: return Key::N;
        case XK_o: case XK_O: return Key::O;
        case XK_p: case XK_P: return Key::P;
        case XK_q: case XK_Q: return Key::Q;
        case XK_r: case XK_R: return Key::R;
        case XK_s: case XK_S: return Key::S;
        case XK_t: case XK_T: return Key::T;
        case XK_u: case XK_U: return Key::U;
        case XK_v: case XK_V: return Key::V;
        case XK_w: case XK_W: return Key::W;
        case XK_x: case XK_X: return Key::X;
        case XK_y: case XK_Y: return Key::Y;
        case XK_z: case XK_Z: return Key::Z;
        case XK_0: return Key::Num0;  case XK_1: return Key::Num1;
        case XK_2: return Key::Num2;  case XK_3: return Key::Num3;
        case XK_4: return Key::Num4;  case XK_5: return Key::Num5;
        case XK_6: return Key::Num6;  case XK_7: return Key::Num7;
        case XK_8: return Key::Num8;  case XK_9: return Key::Num9;
        case XK_comma: return Key::Comma;
        case XK_period: return Key::Period;
        case XK_slash: return Key::Slash;
        case XK_minus: return Key::Minus;
        case XK_equal: return Key::Equal;
        default: return Key::Unknown;
    }
}

} // namespace

// -------------------------------------------------------------------- init
bool Platform::init(int scale, const std::string& title) {
    impl_ = new Impl();
    scale_ = scale < 1 ? 1 : (scale > 6 ? 6 : scale);

    Display* dpy = XOpenDisplay(nullptr);
    if (!dpy) {
        fprintf(stderr, "无法连接到 X 显示（DISPLAY=%s）\n",
                getenv("DISPLAY") ? getenv("DISPLAY") : "(未设置)");
        return false;
    }
    impl_->dpy = dpy;
    dpy_ = dpy;

    impl_->screen = DefaultScreen(dpy);
    impl_->visual = DefaultVisual(dpy, impl_->screen);
    impl_->depth  = DefaultDepth(dpy, impl_->screen);
    impl_->cmap   = DefaultColormap(dpy, impl_->screen);

    cw_ = 256 * scale_;
    ch_ = 240 * scale_;

    XSetWindowAttributes attrs{};
    attrs.background_pixel = BlackPixel(dpy, impl_->screen);
    attrs.border_pixel = BlackPixel(dpy, impl_->screen);
    attrs.event_mask = KeyPressMask | KeyReleaseMask | StructureNotifyMask
                     | ExposureMask | FocusChangeMask;

    impl_->win = XCreateWindow(dpy, RootWindow(dpy, impl_->screen),
                               0, 0, static_cast<unsigned>(cw_), static_cast<unsigned>(ch_),
                               0, impl_->depth, InputOutput, impl_->visual,
                               CWBackPixel | CWBorderPixel | CWEventMask, &attrs);
    win_ = impl_->win;
    if (!impl_->win) { fprintf(stderr, "创建窗口失败\n"); return false; }

    XStoreName(dpy, impl_->win, title.c_str());

    // Ask the window manager to tell us about the close button.
    impl_->wmDelete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, impl_->win, &impl_->wmDelete, 1);

    // Don't let auto-repeat look like the player tapping a button repeatedly.
    Bool supported = False;
    XkbSetDetectableAutoRepeat(dpy, True, &supported);

    impl_->gc = XCreateGC(dpy, impl_->win, 0, nullptr);

    // Try the shared-memory fast path; fall back to a plain XImage if the
    // server has no MIT-SHM (works everywhere, just copies through the wire).
    int major = 0, minor = 0;
    Bool haveShm = XShmQueryExtension(dpy);
    if (haveShm) XShmQueryVersion(dpy, &major, &minor, &haveShm);

    if (haveShm) {
        impl_->image = XShmCreateImage(dpy, impl_->visual, impl_->depth, ZPixmap,
                                       nullptr, &impl_->shm,
                                       static_cast<unsigned>(cw_), static_cast<unsigned>(ch_));
        if (impl_->image) {
            impl_->shm.shmid = shmget(IPC_PRIVATE,
                                      static_cast<size_t>(impl_->image->bytes_per_line) * ch_,
                                      IPC_CREAT | 0600);
            if (impl_->shm.shmid >= 0) {
                impl_->shm.shmaddr = impl_->image->data =
                    static_cast<char*>(shmat(impl_->shm.shmid, nullptr, 0));
                impl_->shm.readOnly = False;
                if (impl_->shm.shmaddr != reinterpret_cast<char*>(-1) &&
                    XShmAttach(dpy, &impl_->shm)) {
                    impl_->useShm = true;
                    // The segment is ours alone once attached.
                    shmctl(impl_->shm.shmid, IPC_RMID, nullptr);
                }
            }
        }
        if (!impl_->useShm && impl_->image) {
            XDestroyImage(impl_->image);
            impl_->image = nullptr;
        }
    }

    if (!impl_->useShm) {
        // Fallback: a normal client-side image the server copies from.
        char* data = static_cast<char*>(calloc(static_cast<size_t>(cw_) * ch_ * 4, 1));
        impl_->image = XCreateImage(dpy, impl_->visual, impl_->depth, ZPixmap,
                                    0, data, static_cast<unsigned>(cw_),
                                    static_cast<unsigned>(ch_), 32, 0);
    }

    allocCanvas(cw_, ch_);
    XMapWindow(dpy, impl_->win);
    XFlush(dpy);

    // Centre the picture if the window manager hands us a bigger window.
    recenter();
    return true;
}

void Platform::allocCanvas(int w, int h) {
    canvas_.assign(static_cast<size_t>(w) * h, 0xFF000000u);
    scaledRow_.assign(static_cast<size_t>(w), 0);
}

void Platform::recenter() {
    if (!impl_ || !impl_->dpy) return;
    XWindowAttributes wa{};
    XGetWindowAttributes(impl_->dpy, impl_->win, &wa);
    offX_ = (wa.width  - cw_) / 2;
    offY_ = (wa.height - ch_) / 2;
}

void Platform::shutdown() {
    if (!impl_) return;
    Display* dpy = impl_->dpy;
    if (dpy) {
        if (impl_->useShm) {
            XShmDetach(dpy, &impl_->shm);
            if (impl_->image) { impl_->image->data = nullptr; XDestroyImage(impl_->image); }
            if (impl_->shm.shmaddr) shmdt(impl_->shm.shmaddr);
        } else if (impl_->image) {
            XDestroyImage(impl_->image);   // also frees the data we allocated
        }
        impl_->image = nullptr;
        if (impl_->gc) XFreeGC(dpy, impl_->gc);
        if (impl_->win) XDestroyWindow(dpy, impl_->win);
        XCloseDisplay(dpy);
    }
    delete impl_;
    impl_ = nullptr;
    dpy_ = nullptr;
}

// ------------------------------------------------------------------- events
bool Platform::pollEvent(KeyEvent& ev) {
    if (!impl_ || !impl_->dpy) return false;
    Display* dpy = impl_->dpy;

    while (XPending(dpy) > 0) {
        XEvent e;
        XNextEvent(dpy, &e);

        switch (e.type) {
            case KeyPress:
            case KeyRelease: {
                const KeySym ks = XLookupKeysym(&e.xkey, 0);
                const Key k = keysymToKey(ks);
                if (k == Key::Unknown) break;      // keep looking for a usable event
                ev.key = k;
                ev.pressed = (e.type == KeyPress);
                ev.ctrl = (e.xkey.state & ControlMask) != 0;
                return true;
            }
            case ClientMessage:
                if (static_cast<Atom>(e.xclient.data.l[0]) == impl_->wmDelete) {
                    wantClose_ = true;
                    ev.key = Key::Unknown;
                    ev.pressed = false;
                    return true;
                }
                break;
            case ConfigureNotify:
                recenter();
                break;
            case Expose:
                break;      // the next present() repaints
            default:
                break;
        }
    }
    return false;
}

// ------------------------------------------------------------------ drawing
void Platform::beginFrame(const u32* nesFb) {
    // Nearest-neighbour integer upscale. Each source row is expanded once into
    // scaledRow_ and then copied to the `scale_` destination rows it covers.
    for (int y = 0; y < 240; y++) {
        const u32* src = nesFb + static_cast<size_t>(y) * 256;
        for (int x = 0; x < 256; x++) {
            const u32 c = src[x] & 0x00FFFFFFu;
            u32* dst = scaledRow_.data() + static_cast<size_t>(x) * scale_;
            for (int t = 0; t < scale_; t++) dst[t] = c;
        }
        u32* row = canvas_.data() + static_cast<size_t>(y * scale_) * cw_;
        for (int s = 0; s < scale_; s++) {
            memcpy(row, scaledRow_.data(), static_cast<size_t>(cw_) * sizeof(u32));
            row += cw_;
        }
    }
}

void Platform::present() {
    if (!impl_ || !impl_->dpy || !impl_->image) return;
    Display* dpy = impl_->dpy;

    u32* dst = reinterpret_cast<u32*>(impl_->image->data);
    const int stride = impl_->image->bytes_per_line / 4;
    for (int y = 0; y < ch_; y++) {
        memcpy(dst + static_cast<size_t>(y) * stride,
               canvas_.data() + static_cast<size_t>(y) * cw_,
               static_cast<size_t>(cw_) * sizeof(u32));
    }

    if (impl_->useShm) {
        XShmPutImage(dpy, impl_->win, impl_->gc, impl_->image,
                     0, 0, offX_ < 0 ? 0 : offX_, offY_ < 0 ? 0 : offY_,
                     static_cast<unsigned>(cw_), static_cast<unsigned>(ch_), False);
        XFlush(dpy);
    } else {
        XPutImage(dpy, impl_->win, impl_->gc, impl_->image,
                  0, 0, offX_ < 0 ? 0 : offX_, offY_ < 0 ? 0 : offY_,
                  static_cast<unsigned>(cw_), static_cast<unsigned>(ch_));
        XFlush(dpy);
    }
}

void Platform::setTitle(const std::string& t) {
    if (impl_ && impl_->dpy) XStoreName(impl_->dpy, impl_->win, t.c_str());
}

void Platform::toggleFullscreen() {
    if (!impl_ || !impl_->dpy) return;
    Display* dpy = impl_->dpy;
    const Atom wmState = XInternAtom(dpy, "_NET_WM_STATE", False);
    const Atom fs      = XInternAtom(dpy, "_NET_WM_STATE_FULLSCREEN", False);

    XEvent e{};
    e.type = ClientMessage;
    e.xclient.window = impl_->win;
    e.xclient.message_type = wmState;
    e.xclient.format = 32;
    e.xclient.data.l[0] = fullscreen_ ? 0 : 1;   // 0 = remove, 1 = add
    e.xclient.data.l[1] = static_cast<long>(fs);
    e.xclient.data.l[2] = 0;
    XSendEvent(dpy, DefaultRootWindow(dpy), False, SubstructureNotifyMask, &e);
    fullscreen_ = !fullscreen_;
    XFlush(dpy);
    recenter();
}

} // namespace nes
