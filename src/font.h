#pragma once
#include <ft2build.h>
#include FT_FREETYPE_H
#include <hb.h>
#include <hb-ft.h>
#include <vector>
#include <map>
#include <string>
#include <cstdint>

struct GlyphInfo {
    int atlasX, atlasY;
    int bearingX, bearingY;
    int width, height;
};

struct ShapedGlyph {
    uint32_t glyphId;
    float xPos, yPos;
    float xAdvance;
    int faceIdx;
    const GlyphInfo* cached = nullptr;
};

struct ShapedRun {
    std::vector<ShapedGlyph> glyphs;
    float totalWidth = 0;
};

class FontManager {
public:
    FontManager();
    ~FontManager();

    bool Init(int sizePx = 18);
    bool SetSize(int sizePx);

    ShapedRun Shape(const std::string& text) const;
    float MeasureWidth(const std::string& text) const;
    float LineHeight() const { return lineHeight_; }
    float Ascent() const { return ascent_; }

    const GlyphInfo& EnsureGlyph(uint32_t glyphId, int faceIdx) const;

    int CurrentSize() const { return currentSizePx_; }

    const uint8_t* AtlasData() const { return atlasData_.data(); }
    int AtlasWidth() const { return atlasW_; }
    int AtlasHeight() const { return atlasH_; }
    size_t AtlasGeneration() const { return atlasGen_; }

private:
    FT_Library ft_ = nullptr;
    FT_Face ftFace_ = nullptr;
    hb_font_t* hbFont_ = nullptr;
    int faceIdx_ = 0;
    int currentSizePx_ = 18;
    float lineHeight_ = 0, ascent_ = 0;

    int atlasW_ = 2048, atlasH_ = 2048;
    mutable std::vector<uint8_t> atlasData_;
    mutable int curX_ = 1, curY_ = 1, rowH_ = 0;
    mutable size_t atlasGen_ = 0;

    static constexpr size_t CACHE_SIZE = 4096;
    static constexpr size_t CACHE_MASK = CACHE_SIZE - 1;
    struct CacheSlot {
        uint64_t key = ~0ULL;
        GlyphInfo info;
    };
    mutable std::vector<CacheSlot> cache_{CACHE_SIZE};

    uint64_t Key(uint32_t glyph, int face) const { return ((uint64_t)currentSizePx_ << 40) | ((uint64_t)face << 32) | glyph; }

    std::string FindFont(const char* family) const;
};
