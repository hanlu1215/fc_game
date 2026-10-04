#include "core/cartridge.h"

#include <cstdio>
#include <cstring>

namespace nes {

// ---------------------------------------------------------------- CRC-32/ISO
u32 Cartridge::crc32(const u8* data, size_t n) {
    static u32 table[256];
    static bool init = false;
    if (!init) {
        for (u32 i = 0; i < 256; i++) {
            u32 c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        init = true;
    }
    u32 c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) c = table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

// ------------------------------------------------------------------- loading
bool Cartridge::loadFromFile(const std::string& file, std::string& err) {
    FILE* f = fopen(file.c_str(), "rb");
    if (!f) { err = "无法打开文件"; return false; }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 16) { fclose(f); err = "文件太小，不是有效的 NES 文件"; return false; }

    std::vector<u8> image(static_cast<size_t>(size));
    size_t got = fread(image.data(), 1, image.size(), f);
    fclose(f);
    if (got != image.size()) { err = "读取文件失败"; return false; }

    if (!loadFromMemory(image, err)) return false;
    path = file;

    // Display name = file name without directory or extension.
    size_t slash = file.find_last_of('/');
    std::string base = (slash == std::string::npos) ? file : file.substr(slash + 1);
    size_t dot = base.find_last_of('.');
    displayName = (dot == std::string::npos) ? base : base.substr(0, dot);
    return true;
}

bool Cartridge::loadFromMemory(const std::vector<u8>& image, std::string& err) {
    prgRom.clear();
    chr.clear();
    prgRam.assign(8192, 0);
    chrIsRam = false;
    battery = false;
    fourScreen = false;
    mirroring = Mirroring::Horizontal;
    mapperNumber = 0;

    if (image.size() < 16 || memcmp(image.data(), "NES\x1A", 4) != 0) {
        err = "不是有效的 iNES 文件（缺少 NES\\x1A 标识）";
        return false;
    }

    const u8* h = image.data();
    u32 prgBanks = h[4];
    u32 chrBanks = h[5];
    u8  flags6 = h[6];
    u8  flags7 = h[7];

    if (prgBanks == 0) { err = "PRG ROM 大小为 0，文件已损坏"; return false; }

    const bool nes2 = (flags7 & 0x0C) == 0x08;

    mapperNumber = (flags6 >> 4) | (flags7 & 0xF0);
    fourScreen = (flags6 & 0x08) != 0;
    battery = (flags6 & 0x02) != 0;
    const bool hasTrainer = (flags6 & 0x04) != 0;

    if (nes2) {
        // NES 2.0: the mapper number gains its high bits from byte 8.
        mapperNumber |= (h[8] & 0x0F) << 8;
        u32 prgRamShift = h[10] & 0x0F;
        if (prgRamShift) prgRam.assign(static_cast<size_t>(64) << prgRamShift, 0);
    }

    mirroring = fourScreen ? Mirroring::FourScreen
              : ((flags6 & 0x01) ? Mirroring::Vertical : Mirroring::Horizontal);

    size_t offset = 16;
    if (hasTrainer) offset += 512;   // trainer is not part of the CPU address space

    size_t prgBytes = static_cast<size_t>(prgBanks) * 16384;
    size_t chrBytes = static_cast<size_t>(chrBanks) * 8192;

    if (offset + prgBytes + chrBytes > image.size()) {
        err = "文件长度与头信息不符（PRG/CHR 数据被截断）";
        return false;
    }

    prgRom.assign(image.begin() + offset, image.begin() + offset + prgBytes);
    offset += prgBytes;

    if (chrBanks == 0) {
        // No CHR ROM: the board has 8 KiB of CHR RAM.
        chr.assign(8192, 0);
        chrIsRam = true;
    } else {
        chr.assign(image.begin() + offset, image.begin() + offset + chrBytes);
    }

    prgCrc = crc32(prgRom.data(), prgRom.size());
    return true;
}

// ------------------------------------------------------------ battery saves
std::string Cartridge::batteryPath() const {
    if (path.empty()) return {};
    size_t dot = path.find_last_of('.');
    return (dot == std::string::npos) ? path + ".sav" : path.substr(0, dot) + ".sav";
}

bool Cartridge::saveBattery() const {
    if (!battery || path.empty() || prgRam.empty()) return false;
    std::string p = batteryPath();
    FILE* f = fopen(p.c_str(), "wb");
    if (!f) return false;
    fwrite(prgRam.data(), 1, prgRam.size(), f);
    fclose(f);
    return true;
}

void Cartridge::loadBattery() {
    if (!battery || path.empty() || prgRam.empty()) return;
    std::string p = batteryPath();
    FILE* f = fopen(p.c_str(), "rb");
    if (!f) return;
    size_t got = fread(prgRam.data(), 1, prgRam.size(), f);
    if (got < prgRam.size()) {
        // Short file: keep what we read and leave the rest zeroed.
    }
    fclose(f);
}

} // namespace nes
