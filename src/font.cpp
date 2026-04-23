#include "font.h"
#include <algorithm>
#include <cstdio>
#include <cstring>

#ifdef __APPLE__
#  include <CoreText/CoreText.h>
#  include <CoreFoundation/CoreFoundation.h>
#else
#  include <fontconfig/fontconfig.h>
#endif

FontManager::FontManager() = default;

FontManager::~FontManager() {
    if (hbFont_) hb_font_destroy(hbFont_);
    if (ftFace_) FT_Done_Face(ftFace_);
    if (ft_) FT_Done_FreeType(ft_);
}

#ifdef __APPLE__
std::string FontManager::FindFont(const char* family) const {
    CFStringRef name = CFStringCreateWithCString(nullptr, family, kCFStringEncodingUTF8);
    if (!name) return {};
    CTFontDescriptorRef desc = CTFontDescriptorCreateWithNameAndSize(name, 0.0);
    CFRelease(name);
    if (!desc) return {};

    std::string path;
    CFURLRef url = (CFURLRef)CTFontDescriptorCopyAttribute(desc, kCTFontURLAttribute);
    if (url) {
        char buf[1024];
        if (CFURLGetFileSystemRepresentation(url, true, (UInt8*)buf, sizeof(buf))) {
            // CoreText will happily return *any* font when asked for an unknown
            // family. Verify the family actually matches, so the caller's
            // fallback chain still works instead of locking in the first hit.
            CFStringRef matched = (CFStringRef)CTFontDescriptorCopyAttribute(desc, kCTFontFamilyNameAttribute);
            if (matched) {
                char mbuf[256];
                if (CFStringGetCString(matched, mbuf, sizeof(mbuf), kCFStringEncodingUTF8)) {
                    if (std::strcmp(mbuf, family) == 0) path = buf;
                }
                CFRelease(matched);
            }
        }
        CFRelease(url);
    }
    CFRelease(desc);
    return path;
}
#else
std::string FontManager::FindFont(const char* family) const {
    FcInit();
    FcPattern* pat = FcPatternCreate();
    FcPatternAddString(pat, FC_FAMILY, (const FcChar8*)family);
    FcConfigSubstitute(nullptr, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);

    FcResult result;
    FcPattern* match = FcFontMatch(nullptr, pat, &result);
    std::string path;
    if (match) {
        FcChar8* file = nullptr;
        if (FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch)
            path = (const char*)file;
        FcPatternDestroy(match);
    }
    FcPatternDestroy(pat);
    return path;
}
#endif

bool FontManager::Init(int sizePx) {
    if (FT_Init_FreeType(&ft_)) return false;

    // Try a few sans fonts. On macOS we prefer the system font (SF Pro) via
    // its public family name, then fall back to what's likely installed.
    const char* tryFonts[] = {
#ifdef __APPLE__
        "Inter", "SF Pro Text", "Helvetica Neue", "Helvetica", "Arial",
#else
        "Inter", "Noto Sans", "DejaVu Sans", "Liberation Sans", "FreeSans",
#endif
    };
    std::string path;
    for (auto name : tryFonts) {
        path = FindFont(name);
        if (!path.empty()) {
            std::fprintf(stderr, "Font: %s (%s)\n", name, path.c_str());
            break;
        }
    }
#ifdef __APPLE__
    // Last-resort: load Helvetica.ttc directly. Ships with every macOS
    // install, so this only fails if the OS is badly broken.
    if (path.empty()) {
        const char* fallback = "/System/Library/Fonts/Helvetica.ttc";
        FT_Face probe = nullptr;
        if (FT_New_Face(ft_, fallback, 0, &probe) == 0) {
            FT_Done_Face(probe);
            path = fallback;
            std::fprintf(stderr, "Font: system fallback (%s)\n", fallback);
        }
    }
#endif

    if (path.empty()) {
        std::fprintf(stderr, "No suitable font found\n");
        return false;
    }

    if (FT_New_Face(ft_, path.c_str(), 0, &ftFace_)) {
        std::fprintf(stderr, "Failed to load font: %s\n", path.c_str());
        return false;
    }
    FT_Set_Pixel_Sizes(ftFace_, 0, sizePx);

    hbFont_ = hb_ft_font_create_referenced(ftFace_);

    currentSizePx_ = sizePx;
    ascent_ = ftFace_->size->metrics.ascender / 64.0f;
    lineHeight_ = ftFace_->size->metrics.height / 64.0f;

    atlasData_.resize(atlasW_ * atlasH_, 0);
    return true;
}

bool FontManager::SetSize(int sizePx) {
    if (!ftFace_ || sizePx == currentSizePx_) return true;
    FT_Set_Pixel_Sizes(ftFace_, 0, sizePx);
    if (hbFont_) hb_ft_font_changed(hbFont_);
    currentSizePx_ = sizePx;
    ascent_ = ftFace_->size->metrics.ascender / 64.0f;
    lineHeight_ = ftFace_->size->metrics.height / 64.0f;
    // Do NOT clear atlas or cache — old glyphs remain valid,
    // new size gets fresh cache entries via the size-inclusive key.
    return true;
}

ShapedRun FontManager::Shape(const std::string& text) const {
    ShapedRun result;
    if (text.empty()) return result;

    hb_buffer_t* buf = hb_buffer_create();
    hb_buffer_add_utf8(buf, text.c_str(), (int)text.size(), 0, (int)text.size());
    hb_buffer_set_direction(buf, HB_DIRECTION_LTR);
    hb_buffer_set_script(buf, HB_SCRIPT_COMMON);
    hb_buffer_set_language(buf, hb_language_get_default());
    hb_shape(hbFont_, buf, nullptr, 0);

    unsigned int count = 0;
    hb_glyph_info_t* info = hb_buffer_get_glyph_infos(buf, &count);
    hb_glyph_position_t* pos = hb_buffer_get_glyph_positions(buf, &count);

    float cursorX = 0;
    for (unsigned int i = 0; i < count; i++) {
        ShapedGlyph g;
        g.glyphId = info[i].codepoint;
        g.xPos = cursorX + pos[i].x_offset / 64.0f;
        g.yPos = pos[i].y_offset / 64.0f;
        g.xAdvance = pos[i].x_advance / 64.0f;
        g.faceIdx = faceIdx_;
        g.cached = &EnsureGlyph(g.glyphId, faceIdx_);
        cursorX += g.xAdvance;
        result.glyphs.push_back(g);
    }

    hb_buffer_destroy(buf);
    result.totalWidth = cursorX;
    return result;
}

float FontManager::MeasureWidth(const std::string& text) const {
    return Shape(text).totalWidth;
}

const GlyphInfo& FontManager::EnsureGlyph(uint32_t glyphId, int faceIdx) const {
    uint64_t key = Key(glyphId, faceIdx);

    size_t idx = (size_t)(key * 0x9E3779B97F4A7C15ULL) & CACHE_MASK;
    for (size_t i = 0; i < 64; i++) {
        auto& slot = cache_[idx];
        if (slot.key == key) return slot.info;
        if (slot.key == ~0ULL) break;
        idx = (idx + 1) & CACHE_MASK;
    }

    // Cache miss: rasterize
    GlyphInfo gi{};
    if (FT_Load_Glyph(ftFace_, glyphId, FT_LOAD_RENDER) == 0 && ftFace_->glyph->bitmap.buffer) {
        auto* slot = ftFace_->glyph;
        int w = (int)slot->bitmap.width;
        int h = (int)slot->bitmap.rows;
        if (w > 0 && h > 0) {
            if (curX_ + w + 1 >= atlasW_) {
                curX_ = 1;
                curY_ += rowH_ + 1;
                rowH_ = 0;
            }
            if (curY_ + h + 1 < atlasH_) {
                for (int r = 0; r < h; r++)
                    std::memcpy(&atlasData_[(curY_ + r) * atlasW_ + curX_],
                                &slot->bitmap.buffer[r * slot->bitmap.pitch], w);
                gi.atlasX = curX_;
                gi.atlasY = curY_;
                gi.bearingX = slot->bitmap_left;
                gi.bearingY = slot->bitmap_top;
                gi.width = w;
                gi.height = h;
                curX_ += w + 1;
                if (h > rowH_) rowH_ = h;
            }
        }
    }

    // Insert into cache
    idx = (size_t)(key * 0x9E3779B97F4A7C15ULL) & CACHE_MASK;
    for (size_t i = 0; i < 64; i++) {
        auto& s = cache_[idx];
        if (s.key == ~0ULL || s.key == key) {
            s.key = key;
            s.info = gi;
            atlasGen_++;
            return s.info;
        }
        idx = (idx + 1) & CACHE_MASK;
    }
    auto& s = cache_[(size_t)(key * 0x9E3779B97F4A7C15ULL) & CACHE_MASK];
    s.key = key;
    s.info = gi;
    atlasGen_++;
    return s.info;
}
