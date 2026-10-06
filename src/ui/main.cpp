// fc_game - a NES/Famicom emulator for Linux.
//
// The frontend is deliberately thin: it owns the window, the audio device and
// the menus, and hands one frame at a time to the emulator core.

#include <unistd.h>     // fork, setsid, isatty -- see the detach in App::run

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "core/console.h"
#include "state/savestate.h"
#include "state/settings.h"
#include "ui/audio_out.h"
#include "ui/menu.h"
#include "ui/platform.h"
#include "ui/text.h"

using namespace nes;

namespace {

// ---------------------------------------------------------------------------
// Colours (0x00RRGGBB)
// ---------------------------------------------------------------------------
constexpr u32 kWhite   = 0xFFFFFF;
constexpr u32 kBlack   = 0x000000;
constexpr u32 kDim     = 0x9AA0B0;
constexpr u32 kAccent  = 0xFFD24A;
constexpr u32 kPanel   = 0x101828;
constexpr u32 kBorder  = 0x4A9EFF;
constexpr u32 kSelBg   = 0x2A4A8A;
constexpr u32 kToastBg = 0x14304F;

// Writes the current canvas as a 24-bit BMP. Handy for reporting visual
// problems: the file opens in any image viewer without conversion.
bool saveBMP(const std::string& path, const u32* px, int w, int h) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    const int rowBytes = w * 3;
    const int pad = (4 - (rowBytes % 4)) % 4;
    const u32 dataSize = static_cast<u32>((rowBytes + pad) * h);
    const u32 fileSize = 54 + dataSize;
    u8 hdr[54] = {0};
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(hdr + 2, &fileSize, 4);
    const u32 off = 54;         memcpy(hdr + 10, &off, 4);
    const u32 infoSize = 40;    memcpy(hdr + 14, &infoSize, 4);
    const s32 ww = w, hh = h;
    memcpy(hdr + 18, &ww, 4);   memcpy(hdr + 22, &hh, 4);
    const u16 planes = 1, bpp = 24;
    memcpy(hdr + 26, &planes, 2); memcpy(hdr + 28, &bpp, 2);
    memcpy(hdr + 34, &dataSize, 4);
    fwrite(hdr, 1, 54, f);
    std::vector<u8> row(static_cast<size_t>(rowBytes + pad), 0);
    for (int y = h - 1; y >= 0; y--) {          // BMP rows run bottom-up
        for (int x = 0; x < w; x++) {
            const u32 c = px[static_cast<size_t>(y) * w + x];
            row[x * 3 + 0] = static_cast<u8>(c & 0xFF);
            row[x * 3 + 1] = static_cast<u8>((c >> 8) & 0xFF);
            row[x * 3 + 2] = static_cast<u8>((c >> 16) & 0xFF);
        }
        fwrite(row.data(), 1, row.size(), f);
    }
    fclose(f);
    return true;
}

enum class Screen { MainMenu, Browser, Playing, Paused };

enum class MenuId { LoadRom, Quit, Resume, Save, Load, Volume, Reset, BackToMenu };
struct MenuItem { MenuId id; std::string label; };

// ---------------------------------------------------------------------------
class App {
public:
    int run(int argc, char** argv);

private:
    Console      con_;
    Platform     plat_;
    AudioOut     audio_;
    TextRenderer text_;
    ui::FileBrowser browser_;

    Screen screen_ = Screen::MainMenu;
    int    mainSel_ = 0;
    int    pauseSel_ = 0;
    int    slot_ = 0;
    Settings settings_;         // survives a restart; currently just the volume
    bool   paused_ = false;
    bool   showHelp_ = false;

    std::string toast_;
    std::chrono::steady_clock::time_point toastUntil_{};
    std::chrono::steady_clock::time_point nextFrame_{};

    std::vector<float> audioBuf_;
    bool  haveRom_ = false;

    // Keys currently held, indexed by Key.
    bool held_[static_cast<int>(Key::COUNT)] = {};

    // ---- setup ------------------------------------------------------------
    int  chooseScale() const;
    bool initAll(const std::string& title);
    bool loadRom(const std::string& path);

    // ---- input ------------------------------------------------------------
    void handleKey(const KeyEvent& e);
    void handlePlayingKey(Key k, bool ctrl);
    void handleMenuKey(const KeyEvent& e);
    void applyGamepad();

    // ---- actions ----------------------------------------------------------
    void doSaveState();
    void doLoadState();
    void doReset();
    void toast(const std::string& msg);
    bool toastActive() const;

    // ---- pause menu -------------------------------------------------------
    // One list, shared by the drawing code and the key handler, so the two can
    // never disagree about the order or the meaning of an entry.
    std::vector<MenuItem> pauseItems();
    void setVolume(int percent);

    // ---- drawing ----------------------------------------------------------
    void draw();
    void drawMainMenu();
    void drawBrowser();
    void drawPauseMenu();
    void drawHelp();
    void drawPlayingHud();
    void drawToast();
    void drawCentered(int y, const std::string& s, u32 rgb, int px);

    // ---- text that adapts to the available font ---------------------------
    std::string T(const char* zh, const char* en) const {
        return text_.unicode() ? std::string(zh) : std::string(en);
    }

    // Writes the emulator's内部 state next to the screenshot: OAM, the CPU
    // registers and the whole 2 KiB of work RAM. This is what makes a
    // screenshot useful for diagnosing gameplay bugs.
    void dumpDebugState(const std::string& path);

    // ---- frame pacing -----------------------------------------------------
    void pace();
};

// ---------------------------------------------------------------------------
int App::chooseScale() const {
    // Pick the largest integer scale that comfortably fits the screen.
    int sw = 1920, sh = 1080;
    // X11 screen size isn't exposed through Platform, so assume a sane default
    // and clamp; 3x (768x720) is safe on anything from 1366x768 upward.
    (void)sw; (void)sh;
    const char* env = getenv("FC_SCALE");
    if (env) {
        int v = atoi(env);
        if (v >= 1 && v <= 6) return v;
    }
    return 3;
}

bool App::initAll(const std::string& title) {
    if (!plat_.init(chooseScale(), title)) return false;

    if (text_.init()) {
        fprintf(stderr, "界面字体: %s\n", "FreeType 已就绪");
    } else {
        fprintf(stderr, "警告: 未找到可用的中文字体，界面回退为英文点阵字体\n");
    }

    // The volume the player last chose, or -- on a first run, before there is
    // anything saved -- the FC_VOLUME default. From then on the menu owns it
    // and each change is written straight back out.
    settings_.load();
    if (!settings_.hasVolume()) {
        if (const char* env = getenv("FC_VOLUME")) {
            const int v = atoi(env);
            if (v >= 0 && v <= 100) settings_.setVolume(v);
        }
    }
    audio_.setVolume(settings_.volume() / 100.0f);

    if (audio_.init(AUDIO_RATE)) {
        fprintf(stderr, "音频: 已打开 44100Hz 立体声输出，音量 %d%%\n", settings_.volume());
    } else {
        fprintf(stderr, "警告: 音频不可用（%s），将静音运行\n", audio_.error().c_str());
    }
    audioBuf_.reserve(4096);
    return true;
}

bool App::loadRom(const std::string& path) {
    std::string err;
    if (!con_.loadROM(path, err)) {
        toast(T("加载失败: ", "Load failed: ") + err);
        return false;
    }
    haveRom_ = true;
    paused_ = false;
    screen_ = Screen::Playing;
    plat_.setTitle("fc_game - " + con_.cartridge().displayName);
    toast(T("已加载: ", "Loaded: ") + con_.cartridge().displayName
          + "  [" + std::string(con_.cartridge().mapperNumber == 0 ? "NROM" : "mapper")
          + " " + std::to_string(con_.cartridge().mapperNumber) + "]");
    return true;
}

// ------------------------------------------------------------------- input
void App::applyGamepad() {
    Controller& c = con_.controller(0);
    using B = Controller::Button;
    auto on = [&](Key k) { return held_[static_cast<int>(k)]; };

    c.setButton(B::Up,     on(Key::Up));
    c.setButton(B::Down,   on(Key::Down));
    c.setButton(B::Left,   on(Key::Left));
    c.setButton(B::Right,  on(Key::Right));
    c.setButton(B::A,      on(Key::X) || on(Key::K));
    c.setButton(B::B,      on(Key::Z) || on(Key::J));
    c.setButton(B::Start,  on(Key::Enter));
    c.setButton(B::Select, on(Key::RightShift) || on(Key::C));
}

void App::toast(const std::string& msg) {
    toast_ = msg;
    toastUntil_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(2200);
}

bool App::toastActive() const {
    return !toast_.empty() && std::chrono::steady_clock::now() < toastUntil_;
}

void App::doSaveState() {
    if (!haveRom_) return;
    std::string err;
    if (SaveState::save(con_, con_.cartridge(), slot_, err)) {
        toast(T("已存档 → 槽位 ", "Saved to slot ") + std::to_string(slot_ + 1));
    } else {
        toast(T("存档失败: ", "Save failed: ") + err);
    }
}

void App::doLoadState() {
    if (!haveRom_) return;
    std::string err;
    if (SaveState::load(con_, con_.cartridge(), slot_, err)) {
        toast(T("已读档 ← 槽位 ", "Loaded slot ") + std::to_string(slot_ + 1));
    } else {
        toast(T("读档失败: ", "Load failed: ") + err);
    }
}

void App::doReset() {
    if (!haveRom_) return;
    con_.reset();
    paused_ = false;
    toast(T("已重置", "Reset"));
}

void App::handleKey(const KeyEvent& e) {
    if (e.key == Key::Unknown) { if (e.pressed == false && plat_.shouldClose()) {} return; }
    const int idx = static_cast<int>(e.key);
    if (idx < 0 || idx >= static_cast<int>(Key::COUNT)) return;

    if (e.pressed) held_[idx] = true;
    else           held_[idx] = false;

    if (!e.pressed) return;      // actions fire on press only

    // Global keys, active on every screen.
    switch (e.key) {
        case Key::F11:
            plat_.toggleFullscreen();
            return;
        case Key::F12:
            showHelp_ = !showHelp_;
            return;
        case Key::F8: {
            static int shotNo = 0;
            const std::string stem = "fc_game_shot_" + std::to_string(++shotNo);
            const bool ok = saveBMP(stem + ".bmp", plat_.canvas(),
                                    plat_.canvasW(), plat_.canvasH());
            dumpDebugState(stem + ".txt");
            toast(ok ? T("已保存截图与状态: ", "Saved screenshot + state: ") + stem
                     : T("截图保存失败", "Screenshot failed"));
            return;
        }
        default: break;
    }

    // Alt+1..4 drops a save into that slot, Ctrl+1..4 loads it back. Both are
    // meant to be used mid-game, so they work on any screen.
    if ((e.alt || e.ctrl) && e.key >= Key::Num1 && e.key <= Key::Num4) {
        slot_ = static_cast<int>(e.key) - static_cast<int>(Key::Num1);
        if (e.alt) doSaveState();
        else       doLoadState();
        return;
    }

    if (e.ctrl) {
        switch (e.key) {
            case Key::Q: plat_.shutdown(); std::exit(0);
            case Key::O:
                browser_.open(haveRom_ ? std::string() : ".");
                screen_ = Screen::Browser;
                return;
            case Key::R: doReset(); return;
            default: break;
        }
    }

    if (screen_ == Screen::Playing) handlePlayingKey(e.key, e.ctrl);
    else                            handleMenuKey(e);
}

void App::handlePlayingKey(Key k, bool ctrl) {
    (void)ctrl;
    switch (k) {
        case Key::F1:
            paused_ = !paused_;
            toast(paused_ ? T("已暂停", "Paused") : T("继续", "Resumed"));
            break;
        case Key::Escape:
            screen_ = Screen::Paused;
            pauseSel_ = 0;
            break;
        case Key::F5: doSaveState(); break;
        case Key::F9: doLoadState(); break;
        case Key::F2:
            slot_ = (slot_ + 3) % 4;
            toast(T("存档槽位 ", "Slot ") + std::to_string(slot_ + 1));
            break;
        case Key::F3:
            slot_ = (slot_ + 1) % 4;
            toast(T("存档槽位 ", "Slot ") + std::to_string(slot_ + 1));
            break;
        default: break;
    }
}

void App::handleMenuKey(const KeyEvent& e) {
    const Key k = e.key;

    if (screen_ == Screen::MainMenu) {
        const int n = 2;
        if (k == Key::Up)   mainSel_ = (mainSel_ + n - 1) % n;
        if (k == Key::Down) mainSel_ = (mainSel_ + 1) % n;
        if (k == Key::Enter) {
            if (mainSel_ == 0) { browser_.open("."); screen_ = Screen::Browser; }
            else { plat_.shutdown(); std::exit(0); }
        }
        if (k == Key::Escape) { plat_.shutdown(); std::exit(0); }
        return;
    }

    if (screen_ == Screen::Browser) {
        if (k == Key::Up)    browser_.moveUp();
        if (k == Key::Down)  browser_.moveDown();
        if (k == Key::Left)  browser_.goParent();
        if (k == Key::Enter) {
            std::string rom;
            if (browser_.activate(rom)) loadRom(rom);
        }
        if (k == Key::Escape) {
            screen_ = haveRom_ ? Screen::Paused : Screen::MainMenu;
        }
        return;
    }

    if (screen_ == Screen::Paused) {
        const std::vector<MenuItem> items = pauseItems();
        const int n = static_cast<int>(items.size());
        pauseSel_ %= n;                     // stay valid if the list ever changes
        if (k == Key::Up)   pauseSel_ = (pauseSel_ + n - 1) % n;
        if (k == Key::Down) pauseSel_ = (pauseSel_ + 1) % n;

        const MenuId sel = items[pauseSel_].id;

        // Left/Right act on the highlighted entry when it carries a value, and
        // keep their old meaning -- the save slot -- for everything else.
        if (k == Key::Left || k == Key::Right) {
            if (sel == MenuId::Volume) {
                setVolume(settings_.volume() + (k == Key::Right ? 5 : -5));
            } else {
                slot_ = (slot_ + (k == Key::Right ? 1 : 3)) % 4;
                toast(T("存档槽位 ", "Slot ") + std::to_string(slot_ + 1));
            }
        }

        if (k == Key::Escape) { screen_ = Screen::Playing; return; }

        if (k == Key::Enter) {
            switch (sel) {
                case MenuId::Resume:     screen_ = Screen::Playing; break;
                case MenuId::Save:       doSaveState(); break;
                case MenuId::Load:       doLoadState(); break;
                case MenuId::Reset:      doReset(); screen_ = Screen::Playing; break;
                case MenuId::BackToMenu: screen_ = Screen::MainMenu; break;
                case MenuId::Quit:       plat_.shutdown(); std::exit(0); break;
                default: break;         // Volume and LoadRom have no Enter action
            }
        }
        return;
    }
}

// ----------------------------------------------------------------- drawing
void App::drawCentered(int y, const std::string& s, u32 rgb, int px) {
    const int w = text_.width(s, px);
    text_.drawShadow(plat_.canvas(), plat_.canvasW(), plat_.canvasH(),
                     (plat_.canvasW() - w) / 2, y, s, rgb, px);
}

void App::drawMainMenu() {
    u32* cv = plat_.canvas();
    const int cw = plat_.canvasW(), ch = plat_.canvasH();

    ui::drawRect(cv, cw, ch, 0, 0, cw, ch, 0x0A1020);

    drawCentered(ch / 4, "FC GAME", kAccent, 56);
    drawCentered(ch / 4 + 66, T("NES / 红白机 模拟器", "NES / Famicom Emulator"), kDim, 20);

    const int w = 420, x = (cw - w) / 2;
    const int y0 = ch / 2 + 10;
    ui::drawPanel(cv, cw, ch, x, y0, w, 150, kPanel, kBorder);

    const char* items[2][2] = { { "加载 ROM", "LOAD ROM" }, { "退出", "QUIT" } };
    for (int i = 0; i < 2; i++) {
        const int iy = y0 + 26 + i * 52;
        if (i == mainSel_) ui::drawRect(cv, cw, ch, x + 10, iy - 6, w - 20, 44, kSelBg);
        const std::string label = text_.unicode() ? items[i][0] : items[i][1];
        text_.drawShadow(cv, cw, ch, x + 40, iy, label, i == mainSel_ ? kWhite : kDim, 24);
        if (i == mainSel_) text_.drawShadow(cv, cw, ch, x + 18, iy, ">", kAccent, 24);
    }

    drawCentered(ch - 92, T("载入游戏后按 F12 查看全部快捷键", "Press F12 in game for all shortcuts"), kDim, 16);
    drawCentered(ch - 62, T("Enter 选择   Esc 退出", "Enter select   Esc quit"), kDim, 16);
}

void App::drawBrowser() {
    u32* cv = plat_.canvas();
    const int cw = plat_.canvasW(), ch = plat_.canvasH();

    ui::drawRect(cv, cw, ch, 0, 0, cw, ch, 0x0A1020);
    drawCentered(24, T("选择游戏 ROM", "SELECT ROM"), kAccent, 30);

    const int x = 40, w = cw - 80, y = 76, h = ch - 76 - 70;
    ui::drawPanel(cv, cw, ch, x, y, w, h, kPanel, kBorder);

    text_.drawShadow(cv, cw, ch, x + 16, y + 12, browser_.shortDir(), kAccent, 18);

    const auto& entries = browser_.entries();
    const int rowH = 30;
    const int visible = (h - 60) / rowH;
    const int sel = browser_.selected();

    // Keep the selection inside the visible window.
    int scroll = browser_.scroll();
    if (sel < scroll) scroll = sel;
    if (sel >= scroll + visible) scroll = sel - visible + 1;

    if (entries.empty()) {
        text_.drawShadow(cv, cw, ch, x + 30, y + 70,
                         T("此目录下没有 .nes 文件", "No .nes files in this folder"),
                         kDim, 20);
    }

    for (int i = 0; i < visible && scroll + i < static_cast<int>(entries.size()); i++) {
        const auto& e = entries[scroll + i];
        const int ry = y + 52 + i * rowH;
        if (scroll + i == sel) {
            ui::drawRect(cv, cw, ch, x + 8, ry - 4, w - 16, rowH - 2, kSelBg);
        }
        std::string label = e.isDir ? ("[" + e.name + "]") : e.name;
        if (label.size() > 46) label = label.substr(0, 45) + "…";
        text_.drawShadow(cv, cw, ch, x + 24, ry,
                         label, scroll + i == sel ? kWhite : kDim, 20);
    }

    // Scrollbar hint
    if (static_cast<int>(entries.size()) > visible) {
        const int barH = std::max(20, (h - 60) * visible / static_cast<int>(entries.size()));
        const int barY = y + 52 + (h - 60 - barH) * sel / std::max(1, static_cast<int>(entries.size()) - 1);
        ui::drawRect(cv, cw, ch, x + w - 14, barY, 6, barH, kBorder);
    }

    drawCentered(ch - 46, T("↑↓ 选择   Enter 进入 / 载入   ← 上级目录   Esc 返回",
                            "Up/Down select   Enter open/load   Left parent   Esc back"),
                 kDim, 16);
}

std::vector<MenuItem> App::pauseItems() {
    const bool hasSave = haveRom_ && SaveState::exists(con_.cartridge(), slot_);
    const std::string slotLabel = T("槽位 ", "Slot ") + std::to_string(slot_ + 1);
    const std::string saveLabel = T("存档", "Save state") + "  [" + slotLabel + "]";
    const std::string loadLabel = T("读档", "Load state") + std::string("  [")
                                + (hasSave ? T("有存档", "exists") : T("空", "empty")) + "]";
    const std::string volLabel  = T("音量", "Volume") + "  [" + std::to_string(settings_.volume()) + "%]";

    return {
        { MenuId::Resume,     T("继续游戏", "Resume") },
        { MenuId::Save,       saveLabel },
        { MenuId::Load,       loadLabel },
        { MenuId::Volume,     volLabel },
        { MenuId::Reset,      T("重置游戏", "Reset") },
        { MenuId::BackToMenu, T("返回主菜单", "Back to main menu") },
        { MenuId::Quit,       T("退出程序", "Quit") },
    };
}

void App::setVolume(int percent) {
    const int before = settings_.volume();
    settings_.setVolume(percent);               // clamps
    audio_.setVolume(settings_.volume() / 100.0f);
    // Only touch the disk when the number actually moved, so holding the key
    // down (X auto-repeat) does not rewrite the file over and over.
    if (settings_.volume() != before) settings_.save();
}

void App::drawPauseMenu() {
    u32* cv = plat_.canvas();
    const int cw = plat_.canvasW(), ch = plat_.canvasH();

    // Dim the frozen game picture behind the menu.
    for (int i = 0; i < cw * ch; i++) {
        const u32 c = cv[i];
        cv[i] = (((c >> 16 & 0xFF) / 3) << 16) | (((c >> 8 & 0xFF) / 3) << 8) | ((c & 0xFF) / 3);
    }

    const std::vector<MenuItem> items = pauseItems();
    const int w = 480, h = 60 + static_cast<int>(items.size()) * 46;
    const int x = (cw - w) / 2, y = (ch - h) / 2;
    ui::drawPanel(cv, cw, ch, x, y, w, h, kPanel, kBorder);

    drawCentered(y + 18, T("暂停", "PAUSED"), kAccent, 28);

    for (int i = 0; i < static_cast<int>(items.size()); i++) {
        const int iy = y + 60 + i * 46;
        if (i == pauseSel_) ui::drawRect(cv, cw, ch, x + 12, iy - 8, w - 24, 40, kSelBg);
        text_.drawShadow(cv, cw, ch, x + 44, iy,
                         items[i].label, i == pauseSel_ ? kWhite : kDim, 22);
        if (i == pauseSel_) text_.drawShadow(cv, cw, ch, x + 22, iy, ">", kAccent, 22);
    }

    // Small hint about the running game, plus how to work the entries that
    // carry a value.
    drawCentered(ch - 56, T("← → 调整所选项目的值", "← → adjust the selected value"), kDim, 16);
    const std::string rom = con_.cartridge().displayName;
    drawCentered(ch - 30, rom, kDim, 16);
}

void App::drawHelp() {
    u32* cv = plat_.canvas();
    const int cw = plat_.canvasW(), ch = plat_.canvasH();

    ui::drawRect(cv, cw, ch, 0, 0, cw, ch, 0x060A14);
    drawCentered(22, T("快捷键", "Keyboard shortcuts"), kAccent, 30);

    struct Row { const char* zh; const char* en; };
    const Row rows[] = {
        { "方向键           移动 / 十字键",      "Arrow keys        D-pad" },
        { "X 或 K           A 键",              "X or K            A button" },
        { "Z 或 J           B 键",              "Z or J            B button" },
        { "Enter             开始 (Start)",      "Enter             Start" },
        { "右 Shift 或 C     选择 (Select)",     "Right Shift or C  Select" },
        { "", "" },
        { "F1                暂停 / 继续",       "F1                Pause / resume" },
        { "Esc               打开暂停菜单",      "Esc               Pause menu" },
        { "F5                存档",              "F5                Save state" },
        { "F9                读档",              "F9                Load state" },
        { "F2 / F3           切换存档槽位",      "F2 / F3           Change slot" },
        { "Alt + 1 / 2 / 3 / 4   立即存档到该槽位", "Alt + 1 / 2 / 3 / 4   Save to that slot" },
        { "Ctrl + 1 / 2 / 3 / 4  立即读取该槽位",   "Ctrl + 1 / 2 / 3 / 4  Load that slot" },
        { "F11               全屏切换",          "F11               Fullscreen" },
        { "F8                保存截图 (BMP)",    "F8                Save screenshot (BMP)" },
        { "F12               显示 / 隐藏帮助",   "F12               Toggle this help" },
        { "Ctrl+O            打开 ROM 浏览器",   "Ctrl+O            Open ROM browser" },
        { "Ctrl+R            重置游戏",          "Ctrl+R            Reset game" },
        { "Ctrl+Q            退出",              "Ctrl+Q            Quit" },
    };

    int y = 84;
    for (const Row& r : rows) {
        const std::string s = text_.unicode() ? r.zh : r.en;
        if (s.empty()) { y += 14; continue; }
        text_.drawShadow(cv, cw, ch, 70, y, s, kWhite, 20);
        y += 34;
    }
    drawCentered(ch - 40, T("按 F12 返回游戏", "Press F12 to return"), kDim, 18);
}

void App::drawPlayingHud() {
    if (!paused_) return;
    u32* cv = plat_.canvas();
    const int cw = plat_.canvasW();
    ui::drawRect(cv, cw, plat_.canvasH(), 0, 0, cw, 40, 0x000000);
    // (kept subtle: a dark strip plus the label)
    text_.drawShadow(cv, cw, plat_.canvasH(), 14, 10,
                     T("已暂停  —  按 F1 继续，Esc 打开菜单", "PAUSED  -  F1 to resume, Esc for menu"),
                     kWhite, 18);
}

void App::drawToast() {
    if (!toastActive()) return;
    u32* cv = plat_.canvas();
    const int cw = plat_.canvasW(), ch = plat_.canvasH();
    const int tw = text_.width(toast_, 20) + 40;
    const int th = 44;
    int x = cw - tw - 16;
    const int y = ch - th - 16;
    if (x < 8) x = 8;
    ui::drawPanel(cv, cw, ch, x, y, tw, th, kToastBg, kBorder);
    text_.drawShadow(cv, cw, ch, x + 20, y + 11, toast_, kWhite, 20);
}

void App::draw() {
    switch (screen_) {
        case Screen::MainMenu: drawMainMenu(); break;
        case Screen::Browser:  drawBrowser();  break;
        case Screen::Paused:   drawPauseMenu(); break;
        case Screen::Playing:  drawPlayingHud(); break;
    }
    drawToast();
    if (showHelp_) drawHelp();
}

void App::dumpDebugState(const std::string& path) {
    FILE* f = fopen(path.c_str(), "w");
    if (!f) return;
    const CPU& cpu = con_.cpuRef();
    fprintf(f, "fc_game state dump\n");
    fprintf(f, "rom=%s mapper=%d prgCrc=%08X\n",
            con_.cartridge().displayName.c_str(), con_.cartridge().mapperNumber,
            con_.cartridge().prgCrc);
    fprintf(f, "CPU  PC=%04X A=%02X X=%02X Y=%02X SP=%02X P=%02X\n",
            cpu.pc, cpu.a, cpu.x, cpu.y, cpu.sp, cpu.status);
    fprintf(f, "PPU  ctrl=%02X mask=%02X status=%02X\n",
            con_.ppuRef().ctrlValue(), con_.ppuRef().maskValue(),
            con_.ppuRef().statusValue());
    fprintf(f, "PAD  %02X\n", con_.controller(0).rawState());

    fprintf(f, "\nOAM (sprite  y tile attr   x)\n");
    for (int i = 0; i < 64; i++) {
        const u8 y = con_.ppuRef().oamRead(static_cast<u16>(i * 4 + 0));
        const u8 t = con_.ppuRef().oamRead(static_cast<u16>(i * 4 + 1));
        const u8 a = con_.ppuRef().oamRead(static_cast<u16>(i * 4 + 2));
        const u8 x = con_.ppuRef().oamRead(static_cast<u16>(i * 4 + 3));
        if (y >= 240) continue;              // hidden slot
        fprintf(f, "  %2d  %3d  %02X   %02X  %3d\n", i, y, t, a, x);
    }

    const u8* ram = con_.busRef().ramData();
    fprintf(f, "\nRAM $0000-$07FF\n");
    for (int base = 0; base < 0x800; base += 16) {
        fprintf(f, "%04X:", base);
        for (int i = 0; i < 16; i++) fprintf(f, " %02X", ram[base + i]);
        fprintf(f, "\n");
    }
    fclose(f);
}

// ------------------------------------------------------------------ pacing
void App::pace() {
    // Normally the audio device does the pacing: writing one frame's samples
    // blocks until the device has room, which is exactly 1/60.0988 s. That is
    // only true while the write really does block, though, and it can quietly
    // stop doing so -- a device that gets reconfigured, a plugin that takes
    // the latency setting and ignores it, an ALSA handle shared with a process
    // that has since exited. Trusting it alone lets the loop run at whatever
    // speed the CPU allows, which shows up as the game running fast.
    //
    // So hold the loop to real time as well. When the audio is doing its job
    // the deadline has already passed and this sleep is a no-op; it only bites
    // when the audio is not carrying its weight.
    const auto frameTime = std::chrono::microseconds(16639);   // 1 / 60.0988 s
    const auto now = std::chrono::steady_clock::now();
    if (nextFrame_.time_since_epoch().count() == 0) nextFrame_ = now;
    nextFrame_ += frameTime;
    if (nextFrame_ < now) nextFrame_ = now;                     // fell behind
    std::this_thread::sleep_until(nextFrame_);
}

// --------------------------------------------------------------------- run
int App::run(int argc, char** argv) {
    // The first argument that is not a flag is the ROM to load.
    std::string initialRom;
    bool stayAttached = false;
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        if (a == "-F" || a == "--foreground") stayAttached = true;
        else if (initialRom.empty())          initialRom = a;
    }

    // Launched by hand from a shell, give the prompt back instead of holding
    // the terminal for as long as the window is open.
    //
    // This has to happen *before* anything is opened. A device opened first
    // stays shared across the fork, and an ALSA handle shared with a parent
    // that then exits stops blocking -- which silences this process and lets
    // the main loop spin. So the child opens the display and the sound card
    // itself, and the parent never touches either.
    //
    // Scripts and pipelines are left alone (stdout is not a terminal there),
    // as is an explicit --foreground.
    int readyPipe = -1;
    if (!stayAttached && isatty(STDOUT_FILENO)) {
        int fds[2];
        if (pipe(fds) != 0) {
            fprintf(stderr, "无法转入后台（pipe 失败：%s），继续在前台运行\n", strerror(errno));
        } else {
            const pid_t pid = fork();
            if (pid < 0) {
                close(fds[0]);
                close(fds[1]);
                fprintf(stderr, "无法转入后台（fork 失败：%s），继续在前台运行\n", strerror(errno));
            } else if (pid > 0) {
                // Parent: wait until the child reports that it is up, so the
                // prompt comes back to a started emulator rather than to one
                // that is still loading. If the child dies first the pipe
                // closes empty and we say nothing -- whatever it printed on
                // stderr is the explanation.
                close(fds[1]);
                char ok = 0;
                const ssize_t n = read(fds[0], &ok, 1);
                close(fds[0]);
                if (n == 1) {
                    printf("已在后台运行（PID %d），关闭窗口即退出。\n"
                           "想留在前台：%s -F %s\n", pid, argv[0],
                           initialRom.empty() ? "[rom]" : initialRom.c_str());
                }
                fflush(stdout);
                _exit(n == 1 ? 0 : 1);
            } else {
                close(fds[0]);
                setsid();   // own session, so closing the terminal cannot kill us
                readyPipe = fds[1];
            }
        }
    }

    if (!initAll("fc_game")) return 1;

    if (!initialRom.empty()) {
        loadRom(initialRom);
    }

    // Startup is done and any warning has reached the terminal, so release the
    // parent's prompt and go quiet from here on.
    if (readyPipe >= 0) {
        const char ok = 1;
        (void)!write(readyPipe, &ok, 1);
        close(readyPipe);
        (void)!std::freopen("/dev/null", "r", stdin);
        (void)!std::freopen("/dev/null", "w", stdout);
        (void)!std::freopen("/dev/null", "w", stderr);
    }

    bool running = true;
    while (running && !plat_.shouldClose()) {
        KeyEvent ev;
        while (plat_.pollEvent(ev)) {
            handleKey(ev);
            if (plat_.shouldClose()) { running = false; break; }
        }
        if (!running) break;

        const bool playing = (screen_ == Screen::Playing) && !paused_ && haveRom_
                          && !showHelp_;

        if (playing) {
            applyGamepad();
            con_.stepFrame();

            audioBuf_.clear();
            con_.drainAudio(audioBuf_);

            if (audio_.ok() && !audioBuf_.empty()) {
                audio_.write(audioBuf_.data(), audioBuf_.size());
            }
        } else if (screen_ == Screen::Playing && paused_) {
            applyGamepad();     // let the pad state settle while paused
        }

        plat_.beginFrame(con_.framebuffer());
        draw();
        plat_.present();

        pace();
    }

    // Persist battery-backed cartridge RAM before we go.
    if (haveRom_) con_.saveBattery();
    audio_.shutdown();
    text_.shutdown();
    plat_.shutdown();
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    App app;
    return app.run(argc, argv);
}
