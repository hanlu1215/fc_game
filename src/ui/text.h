#pragma once

#include <string>

#include "core/types.h"

namespace nes {

// Draws UTF-8 text into an RGB canvas.
//
// FreeType is used when it is available and a CJK-capable font can be found,
// which is what allows the Chinese interface. If either is missing the
// renderer silently falls back to the built-in 8x8 ASCII bitmap font, and the
// menu layer switches itself to English.
class TextRenderer {
public:
    ~TextRenderer() { shutdown(); }

    bool init();
    void shutdown();

    bool unicode() const { return face_ != nullptr; }

    int lineHeight(int px) const;
    int width(const std::string& utf8, int px) const;

    // Draws with the baseline chosen so `y` is the top of the line.
    void draw(u32* canvas, int cw, int ch, int x, int y,
              const std::string& utf8, u32 rgb, int px);

    // Draws a drop shadow first, which keeps text legible over the game image.
    void drawShadow(u32* canvas, int cw, int ch, int x, int y,
                    const std::string& utf8, u32 rgb, int px, u32 shadow = 0x000000);

    // Alpha-blends one pixel; public so the FreeType glyph blitter can reach it.
    void blend(u32* canvas, int cw, int ch, int x, int y, u32 rgb, u8 alpha);

private:
    void* lib_ = nullptr;    // FT_Library
    void* face_ = nullptr;   // FT_Face
    int   facePx_ = 0;       // size currently loaded into the face
    std::string fontPath_;

    void setSize(int px);
    void drawBitmap(u32* canvas, int cw, int ch, int x, int y,
                    const std::string& ascii, u32 rgb, int px);
};

} // namespace nes
