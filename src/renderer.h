#pragma once
#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glext.h>
#include "font.h"
#include <vector>

struct Color {
    float r, g, b, a;
    constexpr Color() : r(0), g(0), b(0), a(1) {}
    constexpr Color(float r_, float g_, float b_, float a_ = 1.0f) : r(r_), g(g_), b(b_), a(a_) {}
    static constexpr Color RGB(int ri, int gi, int bi, int ai = 255) {
        return {ri / 255.f, gi / 255.f, bi / 255.f, ai / 255.f};
    }
};

class Renderer {
public:
    bool Init();
    void Shutdown();

    void BeginFrame(int bufW, int bufH, int logicW, int logicH, const FontManager& fm);
    void EndFrame();

    void DrawRect(float x, float y, float w, float h, const Color& c);
    void DrawRoundedRect(float x, float y, float w, float h, float radius, const Color& c);
    void DrawTriangle(float x1, float y1, float x2, float y2, float x3, float y3, const Color& c);
    void DrawText(const std::string& text, float x, float y, const Color& c);
    void DrawShapedRun(const ShapedRun& run, float x, float y, const Color& c);
    float MeasureText(const std::string& text) const;

    void PushClip(float x, float y, float w, float h);
    void PopClip();

    // Direct draw to current framebuffer without BeginFrame/EndFrame.
    // Useful for overlay drawing (e.g., CSD title bar text).
    void DrawTextDirect(const FontManager& fm, int bufW, int bufH, int logicW, int logicH,
                        const std::string& text, float x, float y, const Color& c);

    int ViewportW() const { return vpW_; }
    int ViewportH() const { return vpH_; }

private:
    GLuint program_ = 0;
    GLuint vao_ = 0, vbo_ = 0;
    GLuint atlasTex_ = 0;
    GLint locProj_ = -1;

    int vpW_ = 0, vpH_ = 0;        // Physical buffer size
    int logicW_ = 0, logicH_ = 0;   // Logical (app) size
    float pxScale_ = 1.0f;          // Physical / logical
    const FontManager* fm_ = nullptr;
    size_t lastAtlasGen_ = 0;

    struct Vertex {
        float x, y;
        float u, v;
        float r, g, b, a;
        float useTex;
        float rectW, rectH;
    };
    std::vector<Vertex> batch_;

    void PushQuad(float x0, float y0, float x1, float y1,
                  float u0, float v0, float u1, float v1,
                  const Color& c, float useTex);
    void UpdateAtlasTexture();
    void FlushBatch();
};
