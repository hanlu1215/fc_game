#include "ui/text.h"
#include "ui/font8x8.h"

#include <cstring>
#include <vector>

#ifdef HAVE_FREETYPE
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_SYNTHESIS_H
#endif

namespace nes {

namespace {

// Candidate fonts, best first. The .ttc collections hold several languages, so
// the face index is searched for a Simplified-Chinese family.
const char* kFontCandidates[] = {
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf",
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Bold.ttc",
    "/usr/share/fonts/opentype/noto/NotoSerifCJK-Regular.ttc",
    "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
    "/usr/share/fonts/truetype/liberation2/LiberationSans-Regular.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
};

size_t decodeUtf8(const std::string& s, size_t i, u32& cp) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (c < 0x80) { cp = c; return 1; }
    if ((c >> 5) == 0x06 && i + 1 < s.size()) {
        cp = ((c & 0x1Fu) << 6) | (s[i + 1] & 0x3Fu);
        return 2;
    }
    if ((c >> 4) == 0x0E && i + 2 < s.size()) {
        cp = ((c & 0x0Fu) << 12) | ((s[i + 1] & 0x3Fu) << 6) | (s[i + 2] & 0x3Fu);
        return 3;
    }
    if ((c >> 3) == 0x1E && i + 3 < s.size()) {
        cp = ((c & 0x07u) << 18) | ((s[i + 1] & 0x3Fu) << 12)
           | ((s[i + 2] & 0x3Fu) << 6) | (s[i + 3] & 0x3Fu);
        return 4;
    }
    cp = '?';
    return 1;
}

} // namespace

// -------------------------------------------------------------------- init
bool TextRenderer::init() {
#ifdef HAVE_FREETYPE
    FT_Library lib = nullptr;
    if (FT_Init_FreeType(&lib) != 0) return false;

    for (const char* path : kFontCandidates) {
        FT_Face probe = nullptr;
        // face_index -1 loads nothing but reports how many faces the file has.
        if (FT_New_Face(lib, path, -1, &probe) != 0) continue;
        const long numFaces = probe->num_faces;
        FT_Done_Face(probe);

        FT_Face chosen = nullptr;
        for (long i = 0; i < (numFaces > 0 ? numFaces : 1); i++) {
            FT_Face f = nullptr;
            if (FT_New_Face(lib, path, i, &f) != 0) continue;
            const char* fam = f->family_name ? f->family_name : "";
            // Prefer a Simplified-Chinese face from a .ttc collection.
            if (numFaces <= 1 || strstr(fam, "SC") != nullptr
                || strstr(fam, "Droid") != nullptr) {
                chosen = f;
                break;
            }
            FT_Done_Face(f);
        }
        if (!chosen && FT_New_Face(lib, path, 0, &chosen) == 0) { /* keep it */ }

        if (chosen) {
            lib_ = lib;
            face_ = chosen;
            fontPath_ = path;
            setSize(18);
            return true;
        }
    }
    FT_Done_FreeType(lib);
    return false;    // no usable font: fall back to the bitmap font
#else
    return false;
#endif
}

void TextRenderer::shutdown() {
#ifdef HAVE_FREETYPE
    if (face_) {
        FT_Done_Face(static_cast<FT_Face>(face_));
        face_ = nullptr;
    }
    if (lib_) {
        FT_Done_FreeType(static_cast<FT_Library>(lib_));
        lib_ = nullptr;
    }
#endif
    facePx_ = 0;
}

void TextRenderer::setSize(int px) {
    if (!face_ || px == facePx_) return;
#ifdef HAVE_FREETYPE
    FT_Set_Pixel_Sizes(static_cast<FT_Face>(face_), 0, static_cast<FT_UInt>(px));
    facePx_ = px;
#else
    (void)px;
#endif
}

int TextRenderer::lineHeight(int px) const {
    if (face_) return px + px / 3;
    const int s = px < 8 ? 1 : px / 8;      // bitmap font scales in whole steps
    return 8 * s + 4;
}

// ------------------------------------------------------------------ drawing
void TextRenderer::blend(u32* cv, int cw, int ch, int x, int y, u32 rgb, u8 a) {
    if (x < 0 || y < 0 || x >= cw || y >= ch || a == 0) return;
    u32& d = cv[static_cast<size_t>(y) * cw + x];
    if (a == 255) { d = rgb & 0x00FFFFFFu; return; }
    const u32 ia = 255 - a;
    const u32 r = (((rgb >> 16) & 0xFF) * a + ((d >> 16) & 0xFF) * ia) / 255;
    const u32 g = (((rgb >> 8) & 0xFF) * a + ((d >> 8) & 0xFF) * ia) / 255;
    const u32 b = ((rgb & 0xFF) * a + (d & 0xFF) * ia) / 255;
    d = (r << 16) | (g << 8) | b;
}

#ifdef HAVE_FREETYPE
static void drawFtGlyph(TextRenderer* self, u32* cv, int cw, int ch,
                        int penX, int baseY, FT_Face face, u32 rgb) {
    FT_GlyphSlot g = face->glyph;
    const int bx = penX + g->bitmap_left;
    const int by = baseY - g->bitmap_top;
    for (unsigned row = 0; row < g->bitmap.rows; row++) {
        for (unsigned col = 0; col < g->bitmap.width; col++) {
            const u8 a = g->bitmap.buffer[row * g->bitmap.pitch + col];
            if (a) self->blend(cv, cw, ch, bx + static_cast<int>(col),
                               by + static_cast<int>(row), rgb, a);
        }
    }
}
#else
static void drawFtGlyph(void*, u32*, int, int, int, int, void*, u32) {}
#endif

void TextRenderer::draw(u32* canvas, int cw, int ch, int x, int y,
                        const std::string& utf8, u32 rgb, int px) {
    if (utf8.empty()) return;

    if (face_) {
#ifdef HAVE_FREETYPE
        setSize(px);
        FT_Face face = static_cast<FT_Face>(face_);
        const int baseY = y + face->size->metrics.ascender / 64;
        int penX = x;
        size_t i = 0;
        while (i < utf8.size()) {
            u32 cp = 0;
            i += decodeUtf8(utf8, i, cp);
            if (cp == '\n') continue;
            if (FT_Load_Char(face, cp, FT_LOAD_RENDER) != 0) continue;
            drawFtGlyph(this, canvas, cw, ch, penX, baseY, face, rgb);
            penX += face->glyph->advance.x >> 6;
        }
#endif
        return;
    }
    drawBitmap(canvas, cw, ch, x, y, utf8, rgb, px);
}

void TextRenderer::drawBitmap(u32* canvas, int cw, int ch, int x, int y,
                              const std::string& ascii, u32 rgb, int px) {
    const int s = px < 8 ? 1 : px / 8;
    int penX = x;
    for (size_t i = 0; i < ascii.size(); i++) {
        unsigned char c = static_cast<unsigned char>(ascii[i]);
        if (c < 0x20 || c > 0x7F) c = '?';      // non-ASCII needs FreeType
        const u8* glyph = kFont8x8[c - 0x20];
        for (int row = 0; row < 8; row++) {
            for (int col = 0; col < 8; col++) {
                if (!(glyph[row] & (1u << col))) continue;
                for (int sy = 0; sy < s; sy++) {
                    for (int sx = 0; sx < s; sx++) {
                        blend(canvas, cw, ch, penX + col * s + sx, y + row * s + sy,
                              rgb, 255);
                    }
                }
            }
        }
        penX += 8 * s;
    }
}

int TextRenderer::width(const std::string& utf8, int px) const {
    if (utf8.empty()) return 0;
    if (face_) {
#ifdef HAVE_FREETYPE
        TextRenderer* self = const_cast<TextRenderer*>(this);
        self->setSize(px);
        FT_Face face = static_cast<FT_Face>(face_);
        int w = 0;
        size_t i = 0;
        while (i < utf8.size()) {
            u32 cp = 0;
            i += decodeUtf8(utf8, i, cp);
            if (FT_Load_Char(face, cp, FT_LOAD_DEFAULT) == 0)
                w += face->glyph->advance.x >> 6;
        }
        return w;
#endif
    }
    const int s = px < 8 ? 1 : px / 8;
    return static_cast<int>(utf8.size()) * 8 * s;
}

void TextRenderer::drawShadow(u32* canvas, int cw, int ch, int x, int y,
                              const std::string& utf8, u32 rgb, int px, u32 shadow) {
    const int off = face_ ? 2 : 2;
    draw(canvas, cw, ch, x + off, y + off, utf8, shadow, px);
    draw(canvas, cw, ch, x, y, utf8, rgb, px);
}

} // namespace nes
