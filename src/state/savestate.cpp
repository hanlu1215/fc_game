#include "state/savestate.h"

#include <cstdio>
#include <cstdlib>
#include <sys/stat.h>
#include <sys/types.h>

namespace nes {

namespace {

// Where saves live: $XDG_DATA_HOME/fc_game/saves, or ~/.local/share/fc_game/saves
std::string saveRoot() {
    const char* xdg = getenv("XDG_DATA_HOME");
    std::string base;
    if (xdg && *xdg) {
        base = xdg;
    } else {
        const char* home = getenv("HOME");
        base = home && *home ? std::string(home) + "/.local/share" : ".";
    }
    return base + "/fc_game/saves";
}

void makeDirs(const std::string& path) {
    // Create every component of `path` that does not exist yet.
    std::string cur;
    for (size_t i = 0; i < path.size(); i++) {
        cur += path[i];
        if (path[i] == '/' && cur.size() > 1) {
            mkdir(cur.c_str(), 0755);
        }
    }
    mkdir(path.c_str(), 0755);
}

std::string hex8(u32 v) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%08X", v);
    return buf;
}

} // namespace

std::string SaveState::slotDir(const Cartridge& cart) {
    // Identity is the PRG checksum, so renaming the ROM keeps saves working.
    return saveRoot() + "/" + hex8(cart.prgCrc);
}

std::string SaveState::slotPath(const Cartridge& cart, int slot) {
    return slotDir(cart) + "/slot" + std::to_string(slot) + ".state";
}

bool SaveState::save(const Console& con, const Cartridge& cart, int slot, std::string& err) {
    makeDirs(slotDir(cart));

    // Serialise the whole machine into a byte vector first.
    StateWriter w;
    con.serialize(w);

    // Header
    std::vector<u8> out;
    auto put32 = [&out](u32 v) {
        out.push_back(static_cast<u8>(v & 0xFF));
        out.push_back(static_cast<u8>((v >> 8) & 0xFF));
        out.push_back(static_cast<u8>((v >> 16) & 0xFF));
        out.push_back(static_cast<u8>((v >> 24) & 0xFF));
    };
    put32(kMagic);
    put32(kVersion);
    put32(cart.prgCrc);
    put32(Cartridge::crc32(w.data.data(), w.data.size()));
    put32(static_cast<u32>(w.data.size()));
    out.insert(out.end(), w.data.begin(), w.data.end());

    const std::string path = slotPath(cart, slot);
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) { err = "无法写入存档文件：" + path; return false; }
    const size_t wrote = fwrite(out.data(), 1, out.size(), f);
    fclose(f);
    if (wrote != out.size()) { err = "存档写入不完整"; return false; }
    return true;
}

bool SaveState::load(Console& con, const Cartridge& cart, int slot, std::string& err) {
    const std::string path = slotPath(cart, slot);
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) { err = "该槽位没有存档"; return false; }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 20) { fclose(f); err = "存档文件已损坏"; return false; }

    std::vector<u8> in(static_cast<size_t>(size));
    size_t got = fread(in.data(), 1, in.size(), f);
    fclose(f);
    if (got != in.size()) { err = "存档读取失败"; return false; }

    auto get32 = [&in](size_t off) -> u32 {
        return static_cast<u32>(in[off]) | (static_cast<u32>(in[off + 1]) << 8)
             | (static_cast<u32>(in[off + 2]) << 16) | (static_cast<u32>(in[off + 3]) << 24);
    };

    if (get32(0) != kMagic) { err = "不是有效的存档文件"; return false; }
    const u32 version = get32(4);
    if (version > kVersion) { err = "存档来自更新的版本，无法读取"; return false; }

    const u32 prgCrc = get32(8);
    if (prgCrc != cart.prgCrc) {
        err = "存档属于另一个游戏（ROM 校验不匹配）";
        return false;
    }

    const u32 payloadCrc = get32(12);
    const u32 payloadLen = get32(16);
    if (20 + payloadLen > in.size()) { err = "存档数据被截断"; return false; }
    if (Cartridge::crc32(in.data() + 20, payloadLen) != payloadCrc) {
        err = "存档校验失败（文件已损坏）";
        return false;
    }

    StateReader r(in.data() + 20, payloadLen);
    con.deserialize(r);
    if (!r.ok()) { err = "存档内容不完整"; return false; }
    return true;
}

bool SaveState::exists(const Cartridge& cart, int slot) {
    struct stat st;
    return stat(slotPath(cart, slot).c_str(), &st) == 0;
}

u64 SaveState::timestamp(const Cartridge& cart, int slot) {
    struct stat st;
    if (stat(slotPath(cart, slot).c_str(), &st) != 0) return 0;
    return static_cast<u64>(st.st_mtime);
}

} // namespace nes
