#pragma once

#include <string>
#include <vector>

#include "core/types.h"
#include "ui/text.h"

namespace nes {
namespace ui {

struct FileEntry {
    std::string name;      // display name (".." for the parent directory)
    std::string path;      // full path
    bool isDir = false;
};

// Directory browser that lists sub-directories and .nes files.
class FileBrowser {
public:
    void open(const std::string& startDir);
    void refresh();

    void moveUp();
    void moveDown();

    // Enters the highlighted entry. Returns true and fills `romPath` when a
    // ROM file was chosen; returns false when the selection changed directory
    // or nothing happened.
    bool activate(std::string& romPath);

    void goParent();

    const std::vector<FileEntry>& entries() const { return entries_; }
    int  selected() const { return selected_; }
    int  scroll() const { return scroll_; }
    const std::string& dir() const { return dir_; }
    const std::string& error() const { return error_; }
    bool atRoot() const { return dir_ == "/"; }
    // Shorter form of the current directory for the title bar.
    std::string shortDir() const;

private:
    std::string dir_;
    std::vector<FileEntry> entries_;
    int selected_ = 0;
    int scroll_ = 0;
    std::string error_;
};

// ---- drawing helpers ------------------------------------------------------
void drawPanel(u32* cv, int cw, int ch, int x, int y, int w, int h,
               u32 fill, u32 border);
void drawRect(u32* cv, int cw, int ch, int x, int y, int w, int h, u32 color);

} // namespace ui
} // namespace nes
