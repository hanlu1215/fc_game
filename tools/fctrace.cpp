// fctrace - a CPU call tracer and coverage-aware disassembler.
//
// Built for one job: answering "why is this code not running?" in a game we
// only have as machine code.
//
// The trick that makes it reliable is that the disassembly is driven by which
// addresses the CPU actually executed. A plain linear sweep drifts out of
// alignment as soon as it walks into a data table, which is exactly what makes
// hand-reading a ROM so error prone; here every listing line is anchored to a
// PC we observed the CPU fetch from.
//
// Usage:
//   fctrace <rom> [frames] --input right,jump [--calls [n]] [--unreached]
//           [--dump-cov <file>]
//           [--callers <addr>] [--listing <addr> [count]] [--path <addr>]
//
// Examples:
//   fctrace SuperMario.nes 900 --input right,jump --calls 40
//   fctrace SuperMario.nes 900 --input right,jump --listing B6BE 24
//   fctrace SuperMario.nes 900 --input right,jump --callers B6DE

#include "core/console.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace nes;

// ---------------------------------------------------------------- disassembler
namespace {

enum Mode { IMP, ACC, IMM, ZP, ZPX, ZPY, IZX, IZY, ABS, ABX, ABY, IND, REL, BAD };

struct OpInfo { const char* name; Mode mode; };

// Indexed by opcode. Anything not listed decodes as a 1-byte unknown; the
// coverage resync in the listing makes those visible instead of silently
// shifting everything after them.
const OpInfo kOps[256] = {
    {"BRK",IMP},{"ORA",IZX},{"???",BAD},{"???",BAD},{"???",BAD},{"ORA",ZP},{"ASL",ZP},{"???",BAD},
    {"PHP",IMP},{"ORA",IMM},{"ASL",ACC},{"???",BAD},{"???",BAD},{"ORA",ABS},{"ASL",ABS},{"???",BAD},
    {"BPL",REL},{"ORA",IZY},{"???",BAD},{"???",BAD},{"???",BAD},{"ORA",ZPX},{"ASL",ZPX},{"???",BAD},
    {"CLC",IMP},{"ORA",ABY},{"???",BAD},{"???",BAD},{"???",BAD},{"ORA",ABX},{"ASL",ABX},{"???",BAD},
    {"JSR",ABS},{"AND",IZX},{"???",BAD},{"???",BAD},{"BIT",ZP},{"AND",ZP},{"ROL",ZP},{"???",BAD},
    {"PLP",IMP},{"AND",IMM},{"ROL",ACC},{"???",BAD},{"BIT",ABS},{"AND",ABS},{"ROL",ABS},{"???",BAD},
    {"BMI",REL},{"AND",IZY},{"???",BAD},{"???",BAD},{"???",BAD},{"AND",ZPX},{"ROL",ZPX},{"???",BAD},
    {"SEC",IMP},{"AND",ABY},{"???",BAD},{"???",BAD},{"???",BAD},{"AND",ABX},{"ROL",ABX},{"???",BAD},
    {"RTI",IMP},{"EOR",IZX},{"???",BAD},{"???",BAD},{"???",BAD},{"EOR",ZP},{"LSR",ZP},{"???",BAD},
    {"PHA",IMP},{"EOR",IMM},{"LSR",ACC},{"???",BAD},{"JMP",ABS},{"EOR",ABS},{"LSR",ABS},{"???",BAD},
    {"BVC",REL},{"EOR",IZY},{"???",BAD},{"???",BAD},{"???",BAD},{"EOR",ZPX},{"LSR",ZPX},{"???",BAD},
    {"CLI",IMP},{"EOR",ABY},{"???",BAD},{"???",BAD},{"???",BAD},{"EOR",ABX},{"LSR",ABX},{"???",BAD},
    {"RTS",IMP},{"ADC",IZX},{"???",BAD},{"???",BAD},{"???",BAD},{"ADC",ZP},{"ROR",ZP},{"???",BAD},
    {"PLA",IMP},{"ADC",IMM},{"ROR",ACC},{"???",BAD},{"JMP",IND},{"ADC",ABS},{"ROR",ABS},{"???",BAD},
    {"BVS",REL},{"ADC",IZY},{"???",BAD},{"???",BAD},{"???",BAD},{"ADC",ZPX},{"ROR",ZPX},{"???",BAD},
    {"SEI",IMP},{"ADC",ABY},{"???",BAD},{"???",BAD},{"???",BAD},{"ADC",ABX},{"ROR",ABX},{"???",BAD},
    {"???",BAD},{"STA",IZX},{"???",BAD},{"???",BAD},{"STY",ZP},{"STA",ZP},{"STX",ZP},{"???",BAD},
    {"DEY",IMP},{"???",BAD},{"TXA",IMP},{"???",BAD},{"STY",ABS},{"STA",ABS},{"STX",ABS},{"???",BAD},
    {"BCC",REL},{"STA",IZY},{"???",BAD},{"???",BAD},{"STY",ZPX},{"STA",ZPX},{"STX",ZPY},{"???",BAD},
    {"TYA",IMP},{"STA",ABY},{"TXS",IMP},{"???",BAD},{"???",BAD},{"STA",ABX},{"???",BAD},{"???",BAD},
    {"LDY",IMM},{"LDA",IZX},{"LDX",IMM},{"???",BAD},{"LDY",ZP},{"LDA",ZP},{"LDX",ZP},{"???",BAD},
    {"TAY",IMP},{"LDA",IMM},{"TAX",IMP},{"???",BAD},{"LDY",ABS},{"LDA",ABS},{"LDX",ABS},{"???",BAD},
    {"BCS",REL},{"LDA",IZY},{"???",BAD},{"???",BAD},{"LDY",ZPX},{"LDA",ZPX},{"LDX",ZPY},{"???",BAD},
    {"CLV",IMP},{"LDA",ABY},{"TSX",IMP},{"???",BAD},{"LDY",ABX},{"LDA",ABX},{"LDX",ABY},{"???",BAD},
    {"CPY",IMM},{"CMP",IZX},{"???",BAD},{"???",BAD},{"CPY",ZP},{"CMP",ZP},{"DEC",ZP},{"???",BAD},
    {"INY",IMP},{"CMP",IMM},{"DEX",IMP},{"???",BAD},{"CPY",ABS},{"CMP",ABS},{"DEC",ABS},{"???",BAD},
    {"BNE",REL},{"CMP",IZY},{"???",BAD},{"???",BAD},{"???",BAD},{"CMP",ZPX},{"DEC",ZPX},{"???",BAD},
    {"CLD",IMP},{"CMP",ABY},{"???",BAD},{"???",BAD},{"???",BAD},{"CMP",ABX},{"DEC",ABX},{"???",BAD},
    {"CPX",IMM},{"SBC",IZX},{"???",BAD},{"???",BAD},{"CPX",ZP},{"SBC",ZP},{"INC",ZP},{"???",BAD},
    {"INX",IMP},{"SBC",IMM},{"NOP",IMP},{"???",BAD},{"CPX",ABS},{"SBC",ABS},{"INC",ABS},{"???",BAD},
    {"BEQ",REL},{"SBC",IZY},{"???",BAD},{"???",BAD},{"???",BAD},{"SBC",ZPX},{"INC",ZPX},{"???",BAD},
    {"SED",IMP},{"SBC",ABY},{"???",BAD},{"???",BAD},{"???",BAD},{"SBC",ABX},{"INC",ABX},{"???",BAD},
};

int opLength(Mode m) {
    switch (m) {
        case IMP: case ACC: return 1;
        case IMM: case ZP: case ZPX: case ZPY: case IZX: case IZY: case REL: return 2;
        default: return 3;
    }
}

// ---------------------------------------------------------------- trace state
struct Tracer {
    std::vector<u8>  covered;     // 64K: did the CPU ever execute here?
    std::vector<u32> hits;        // 64K: how many times
    std::vector<u16> callStack;   // software call stack of JSR targets
    std::map<u16, std::map<u16, u32>> callers;   // callee -> caller -> count
    std::map<u16, u32> entries;                  // callee -> count
    std::map<u16, std::vector<u16>> firstStack;  // callee -> stack on first entry
    bool haveFirst = false;

    Console* con = nullptr;
    std::vector<u8> prg;

    Tracer() : covered(0x10000, 0), hits(0x10000, 0) {}

    // Side-effect-free read: RAM below $2000, cartridge ROM at $8000+.
    u8 peek(u16 addr) const {
        if (addr < 0x2000) return con->busRef().ramData()[addr & 0x07FF];
        if (addr >= 0x8000 && !prg.empty())
            return prg[(addr - 0x8000) % prg.size()];
        return 0;
    }
    u16 peek16(u16 addr) const {
        return static_cast<u16>(peek(addr) | (peek(static_cast<u16>(addr + 1)) << 8));
    }

    static void hook(void* ctx, u16 pc, u8 op) {
        static_cast<Tracer*>(ctx)->onInstruction(pc, op);
    }

    void onInstruction(u16 pc, u8 op) {
        covered[pc] = 1;
        hits[pc]++;
        if (op == 0x20) {                       // JSR
            const u16 target = peek16(static_cast<u16>(pc + 1));
            callStack.push_back(target);
            entries[target]++;
            callers[target][pc]++;
            if (!firstStack.count(target)) firstStack[target] = callStack;
        } else if (op == 0x60 || op == 0x40) {  // RTS / RTI
            if (!callStack.empty()) callStack.pop_back();
        }
    }

    // Rebuilds the call chain for a callee from what was recorded at its first
    // entry, which is enough to see who pulls it in.
    std::string chainFor(u16 addr) const {
        auto it = firstStack.find(addr);
        if (it == firstStack.end() || it->second.empty()) return "(entry point)";
        std::string s;
        for (size_t i = 0; i + 1 < it->second.size(); i++) {
            char buf[16];
            snprintf(buf, sizeof(buf), "$%04X > ", it->second[i]);
            s += buf;
        }
        char buf[16];
        snprintf(buf, sizeof(buf), "$%04X", it->second.back());
        s += buf;
        return s;
    }
};

// --------------------------------------------------------------- disassembly
void formatOperand(const Tracer& t, u16 addr, const OpInfo& oi, char* out, size_t n) {
    const u8 a1 = t.peek(static_cast<u16>(addr + 1));
    const u8 a2 = t.peek(static_cast<u16>(addr + 2));
    const u16 abs = static_cast<u16>(a1 | (a2 << 8));
    switch (oi.mode) {
        case IMP: out[0] = '\0'; break;
        case ACC: snprintf(out, n, "A"); break;
        case IMM: snprintf(out, n, "#$%02X", a1); break;
        case ZP:  snprintf(out, n, "$%02X", a1); break;
        case ZPX: snprintf(out, n, "$%02X,X", a1); break;
        case ZPY: snprintf(out, n, "$%02X,Y", a1); break;
        case IZX: snprintf(out, n, "($%02X,X)", a1); break;
        case IZY: snprintf(out, n, "($%02X),Y", a1); break;
        case ABS: snprintf(out, n, "$%04X", abs); break;
        case ABX: snprintf(out, n, "$%04X,X", abs); break;
        case ABY: snprintf(out, n, "$%04X,Y", abs); break;
        case IND: snprintf(out, n, "($%04X)", abs); break;
        case REL: {
            const s16 off = static_cast<s8>(a1);
            snprintf(out, n, "$%04X", static_cast<u16>(addr + 2 + off));
            break;
        }
        default: snprintf(out, n, ".byte $%02X", t.peek(addr)); break;
    }
}

// Walks `count` instructions from `start`. Whenever the linear walk lands on
// an address the CPU never executed, it looks a short way ahead for the next
// address that *was* executed and resyncs there, printing the bytes it skipped
// as data. That is what keeps the listing aligned through jump tables.
void listing(const Tracer& t, u16 start, int count) {
    printf("listing from $%04X (x = executed, . = never executed):\n", start);
    u16 a = start;
    for (int i = 0; i < count && a < 0x10000;) {
        if (!t.covered[a]) {
            // Try to resync onto the next executed address within 6 bytes.
            u16 resync = 0;
            for (int k = 1; k <= 6 && static_cast<u32>(a) + k < 0x10000; k++) {
                if (t.covered[a + k]) { resync = static_cast<u16>(a + k); break; }
            }
            if (!resync) {
                printf("  %04X: %02X                      .byte $%02X\n", a, t.peek(a), t.peek(a));
                a++; i++;
                continue;
            }
            for (u16 b = a; b < resync; b++) {
                printf("  %04X: %02X                      .byte $%02X   (data)\n", b, t.peek(b), t.peek(b));
                i++;
            }
            a = resync;
            continue;
        }
        const u8 op = t.peek(a);
        const OpInfo& oi = kOps[op];
        const int len = opLength(oi.mode);
        char operand[32];
        formatOperand(t, a, oi, operand, sizeof(operand));
        printf("  %04X: %-9s %-4s %-10s hits=%u\n", a,
               [&]() {
                   static char b[16];
                   snprintf(b, sizeof(b), "%02X", op);
                   for (int k = 1; k < len; k++)
                       snprintf(b + strlen(b), sizeof(b) - strlen(b), " %02X", t.peek(a + k));
                   return b;
               }(),
               oi.name, operand, t.hits[a]);
        a = static_cast<u16>(a + len);
        i++;
    }
}

} // namespace

// ---------------------------------------------------------------------- main
int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr,
            "usage: %s <rom> [frames] [--input right,jump] [--calls [n]] [--unreached]\n"
            "          [--callers <addr>] [--listing <addr> [count]] [--path <addr>]\n", argv[0]);
        return 2;
    }

    const std::string rom = argv[1];
    int frames = 600;
    bool doCalls = false, doUnreached = false;
    int  callsN = 30;
    u16  callersAddr = 0, listingAddr = 0, pathAddr = 0;
    int  listingN = 20;
    bool haveCallers = false, haveListing = false, havePath = false;
    std::string dumpCov;
    bool holdRight = false, jump = false;

    for (int i = 2; i < argc; i++) {
        const std::string a = argv[i];
        if (a == "--calls") {
            doCalls = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') callsN = atoi(argv[++i]);
        } else if (a == "--unreached") doUnreached = true;
        else if (a == "--dump-cov" && i + 1 < argc) dumpCov = argv[++i];
        else if (a == "--callers" && i + 1 < argc) { callersAddr = (u16)strtol(argv[++i], nullptr, 16); haveCallers = true; }
        else if (a == "--listing" && i + 1 < argc) {
            listingAddr = (u16)strtol(argv[++i], nullptr, 16); haveListing = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') listingN = atoi(argv[++i]);
        } else if (a == "--path" && i + 1 < argc) { pathAddr = (u16)strtol(argv[++i], nullptr, 16); havePath = true; }
        else if (a == "--input" && i + 1 < argc) {
            std::string s = argv[++i];
            holdRight = s.find("right") != std::string::npos;
            jump = s.find("jump") != std::string::npos;
        }
        else if (!a.empty() && isdigit(static_cast<unsigned char>(a[0]))) frames = atoi(a.c_str());
    }

    Console con;
    std::string err;
    if (!con.loadROM(rom, err)) { fprintf(stderr, "load failed: %s\n", err.c_str()); return 1; }

    Tracer t;
    t.con = &con;
    t.prg = con.cartridge().prgRom;
    con.cpuRef().debugHook = &Tracer::hook;
    con.cpuRef().debugHookCtx = &t;

    // Boot through the title screen, then press Start.
    for (int f = 0; f < 120; f++) con.stepFrame();
    for (int f = 0; f < 40; f++) {
        con.controller(0).setButton(Controller::Start, true);
        con.stepFrame();
    }
    con.controller(0).setButton(Controller::Start, false);
    for (int f = 0; f < frames; f++) {
        if (holdRight) con.controller(0).setButton(Controller::Right, true);
        if (jump) con.controller(0).setButton(Controller::A, (f % 28) < 18);
        con.stepFrame();
    }
    con.cpuRef().debugHook = nullptr;

    if (!dumpCov.empty()) {
        FILE* f = fopen(dumpCov.c_str(), "wb");
        if (f) { fwrite(t.covered.data(), 1, t.covered.size(), f); fclose(f); }
        fprintf(stderr, "coverage bitmap written to %s\n", dumpCov.c_str());
    }

    printf("traced %d frames of %s\n", frames, con.cartridge().displayName.c_str());
    printf("JSR targets seen: %zu   distinct addresses executed: ",
           t.entries.size());
    {
        size_t n = 0;
        for (u8 c : t.covered) n += c;
        printf("%zu\n\n", n);
    }

    if (havePath) {
        if (!t.covered[pathAddr])
            printf("$%04X was NEVER executed.\n", pathAddr);
        else
            printf("$%04X executed %u times; call chain: %s\n",
                   pathAddr, t.hits[pathAddr], t.chainFor(pathAddr).c_str());
    }

    if (haveCallers) {
        auto it = t.callers.find(callersAddr);
        if (it == t.callers.end()) {
            printf("$%04X was never used as a JSR target.\n", callersAddr);
        } else {
            printf("callers of $%04X:\n", callersAddr);
            for (auto& kv : it->second) printf("   from $%04X : %u times\n", kv.first, kv.second);
        }
        printf("\n");
    }

    if (doCalls) {
        std::vector<std::pair<u32, u16>> v;
        for (auto& kv : t.entries) v.push_back({kv.second, kv.first});
        std::sort(v.rbegin(), v.rend());
        printf("most-called functions (JSR targets):\n");
        for (int i = 0; i < callsN && i < (int)v.size(); i++) {
            const u16 a = v[i].second;
            printf("  $%04X  %6u calls", a, v[i].first);
            if (!t.callers[a].empty()) {
                const auto& c = t.callers[a];
                auto best = std::max_element(c.begin(), c.end(),
                    [](const auto& x, const auto& y) { return x.second < y.second; });
                printf("   (mostly from $%04X)", best->first);
            }
            printf("\n");
        }
        printf("\n");
    }

    if (doUnreached) {
        // Every JSR/JMP target in the ROM that the run never touched.
        std::vector<u16> miss;
        const auto& prg = t.prg;
        for (size_t i = 0; i + 2 < prg.size(); i++) {
            if ((prg[i] == 0x20 || prg[i] == 0x4C)) {
                const u16 target = static_cast<u16>(prg[i + 1] | (prg[i + 2] << 8));
                if (target >= 0x8000 && !t.covered[target]) miss.push_back(target);
            }
        }
        std::sort(miss.begin(), miss.end());
        miss.erase(std::unique(miss.begin(), miss.end()), miss.end());
        printf("JSR/JMP targets never executed (%zu):\n  ", miss.size());
        for (size_t i = 0; i < miss.size(); i++) {
            printf("$%04X ", miss[i]);
            if (i % 10 == 9) printf("\n  ");
        }
        printf("\n\n");
    }

    if (haveListing) {
        listing(t, listingAddr, listingN);
        printf("\n");
    }

    return 0;
}
