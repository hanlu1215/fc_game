#include "ui/menu.h"

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace nes {
namespace ui {

namespace {

bool hasNesExtension(const std::string& name) {
    if (name.size() < 4) return false;
    std::string ext = name.substr(name.size() - 4);
    for (char& c : ext) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return ext == ".nes";
}

std::string joinPath(const std::string& dir, const std::string& name) {
    if (dir.empty()) return name;
    if (dir.back() == '/') return dir + name;
    return dir + "/" + name;
}

// Collapse a path so ".." never appears literally in the title bar.
std::string normalize(const std::string& in) {
    std::vector<std::string> parts;
    std::string cur;
    for (size_t i = 0; i <= in.size(); i++) {
        if (i == in.size() || in[i] == '/') {
            if (cur == "..") {
                if (!parts.empty()) parts.pop_back();
            } else if (!cur.empty() && cur != ".") {
                parts.push_back(cur);
            }
            cur.clear();
        } else {
            cur += in[i];
        }
    }
    std::string out = in.empty() || in[0] != '/' ? "" : "/";
    if (in.size() && in[0] == '/') out = "/";
    for (size_t i = 0; i < parts.size(); i++) {
        if (i) out += "/";
        out += parts[i];
    }
    if (out.empty()) out = "/";
    return out;
}

std::string homeDir() {
    const char* h = getenv("HOME");
    return (h && *h) ? h : "/";
}

} // namespace

// ------------------------------------------------------------------ browser
void FileBrowser::open(const std::string& startDir) {
    dir_ = startDir.empty() ? homeDir() : normalize(startDir);
    selected_ = 0;
    scroll_ = 0;
    refresh();
}

void FileBrowser::refresh() {
    entries_.clear();
    error_.clear();

    DIR* d = opendir(dir_.c_str());
    if (!d) {
        error_ = "无法打开目录";
        return;
    }

    std::vector<FileEntry> dirs, files;
    while (struct dirent* e = readdir(d)) {
        const std::string name = e->d_name;
        if (name == "." || name == "..") continue;
        if (!name.empty() && name[0] == '.') continue;   // hide dotfiles

        const std::string full = joinPath(dir_, name);
        struct stat st{};
        if (stat(full.c_str(), &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            dirs.push_back({name, full, true});
        } else if (S_ISREG(st.st_mode) && hasNesExtension(name)) {
            files.push_back({name, full, false});
        }
    }
    closedir(d);

    auto byName = [](const FileEntry& a, const FileEntry& b) {
        return strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
    };
    std::sort(dirs.begin(), dirs.end(), byName);
    std::sort(files.begin(), files.end(), byName);

    if (!atRoot()) entries_.push_back({"..", normalize(dir_ + "/.."), true});
    entries_.insert(entries_.end(), dirs.begin(), dirs.end());
    entries_.insert(entries_.end(), files.begin(), files.end());

    if (selected_ >= static_cast<int>(entries_.size())) {
        selected_ = entries_.empty() ? 0 : static_cast<int>(entries_.size()) - 1;
    }
    if (selected_ < 0) selected_ = 0;
}

void FileBrowser::moveUp() {
    if (entries_.empty()) return;
    selected_ = (selected_ - 1 + static_cast<int>(entries_.size()))
              % static_cast<int>(entries_.size());
}

void FileBrowser::moveDown() {
    if (entries_.empty()) return;
    selected_ = (selected_ + 1) % static_cast<int>(entries_.size());
}

bool FileBrowser::activate(std::string& romPath) {
    if (entries_.empty()) return false;
    const FileEntry& e = entries_[selected_];
    if (e.isDir) {
        dir_ = normalize(e.path);
        selected_ = 0;
        scroll_ = 0;
        refresh();
        return false;
    }
    romPath = e.path;
    return true;
}

void FileBrowser::goParent() {
    if (atRoot()) return;
    dir_ = normalize(dir_ + "/..");
    selected_ = 0;
    scroll_ = 0;
    refresh();
}

std::string FileBrowser::shortDir() const {
    const std::string home = homeDir();
    if (dir_.compare(0, home.size(), home) == 0) {
        return "~" + dir_.substr(home.size());
    }
    return dir_;
}

// ------------------------------------------------------------------ drawing
void drawRect(u32* cv, int cw, int ch, int x, int y, int w, int h, u32 color) {
    const int x0 = std::max(0, x), y0 = std::max(0, y);
    const int x1 = std::min(cw, x + w), y1 = std::min(ch, y + h);
    for (int yy = y0; yy < y1; yy++) {
        u32* row = cv + static_cast<size_t>(yy) * cw;
        for (int xx = x0; xx < x1; xx++) row[xx] = color;
    }
}

void drawPanel(u32* cv, int cw, int ch, int x, int y, int w, int h,
               u32 fill, u32 border) {
    drawRect(cv, cw, ch, x, y, w, h, border);
    drawRect(cv, cw, ch, x + 2, y + 2, w - 4, h - 4, fill);
}

} // namespace ui
} // namespace nes
