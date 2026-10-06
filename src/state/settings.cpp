#include "state/settings.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>
#include <sys/types.h>

namespace nes {

namespace {

// Same base directory the save states use, so all of this user's per-game
// state lives in one place.
std::string dataRoot() {
    const char* xdg = getenv("XDG_DATA_HOME");
    if (xdg && *xdg) return std::string(xdg) + "/fc_game";
    const char* home = getenv("HOME");
    return home && *home ? std::string(home) + "/.local/share/fc_game" : std::string(".");
}

void makeDirs(const std::string& path) {
    std::string cur;
    for (size_t i = 0; i < path.size(); i++) {
        cur += path[i];
        if (path[i] == '/' && cur.size() > 1) mkdir(cur.c_str(), 0755);
    }
    mkdir(path.c_str(), 0755);
}

// Trims spaces on both ends; a small helper rather than pulling in a library.
std::string trim(const std::string& s) {
    const char* ws = " \t\r\n";
    const size_t a = s.find_first_not_of(ws);
    if (a == std::string::npos) return std::string();
    const size_t b = s.find_last_not_of(ws);
    return s.substr(a, b - a + 1);
}

} // namespace

std::string Settings::path() { return dataRoot() + "/settings.conf"; }

bool Settings::load() {
    FILE* f = fopen(path().c_str(), "rb");
    if (!f) return false;

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        const char* eq = strchr(line, '=');
        if (!eq) continue;                       // not a key = value line
        const std::string key = trim(std::string(line, static_cast<size_t>(eq - line)));
        const std::string val = trim(std::string(eq + 1));
        if (key == "volume") {
            const int v = atoi(val.c_str());
            if (v >= 0 && v <= 100) {
                volume_ = v;
                hasVolume_ = true;
            }
        }
        // Unknown keys are ignored on purpose: a file written by a newer build
        // must not stop an older one from starting.
    }
    fclose(f);
    return hasVolume_;
}

void Settings::save() const {
    const std::string p = path();
    makeDirs(dataRoot());
    FILE* f = fopen(p.c_str(), "wb");
    if (!f) return;                              // read-only home, etc.
    fprintf(f, "# fc_game settings -- written automatically\n");
    fprintf(f, "volume = %d\n", volume_);
    fclose(f);
}

void Settings::setVolume(int percent) {
    volume_ = percent < 0 ? 0 : (percent > 100 ? 100 : percent);
    hasVolume_ = true;
}

} // namespace nes
