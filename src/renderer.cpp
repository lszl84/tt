#include "renderer.h"
#include <cstdio>
#include <cstring>
#include <cmath>

static const char* vertSrc = R"(#version 330 core
layout(location=0) in vec2 aPos;
layout(location=1) in vec2 aUV;
layout(location=2) in vec4 aColor;
layout(location=3) in float aUseTex;
layout(location=4) in vec2 aRectSize;
uniform mat4 uProj;
out vec2 vUV;
out vec4 vColor;
out float vUseTex;
out vec2 vRectSize;
void main() {
    gl_Position = uProj * vec4(aPos, 0.0, 1.0);
    vUV = aUV;
    vColor = aColor;
    vUseTex = aUseTex;
    vRectSize = aRectSize;
}
)";

static const char* fragSrc = R"(#version 330 core
in vec2 vUV;
in vec4 vColor;
in float vUseTex;
in vec2 vRectSize;
uniform sampler2D uAtlas;
out vec4 fragColor;

float roundedRectSDF(vec2 p, vec2 halfSize, float radius) {
    vec2 d = abs(p) - halfSize + radius;
    return length(max(d, 0.0)) - radius;
}

void main() {
    if (vUseTex > 1.5) {
        float radius = vUseTex - 2.0;
        vec2 halfSize = vRectSize * 0.5;
        vec2 p = vUV - halfSize;
        float d = roundedRectSDF(p, halfSize, radius);
        float aa = 1.0 - smoothstep(-0.5, 0.5, d);
        fragColor = vec4(vColor.rgb, vColor.a * aa);
    } else if (vUseTex > 0.5) {
        float alpha = texture(uAtlas, vUV).r;
        fragColor = vec4(vColor.rgb, vColor.a * alpha);
    } else {
        fragColor = vColor;
    }
}
)";

static GLuint CompileShader(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512]; glGetShaderInfoLog(s, 512, nullptr, log);
        fprintf(stderr, "Shader error: %s\n", log);
    }
    return s;
}

bool Renderer::Init() {
    GLuint vs = CompileShader(GL_VERTEX_SHADER, vertSrc);
    GLuint fs = CompileShader(GL_FRAGMENT_SHADER, fragSrc);
    program_ = glCreateProgram();
    glAttachShader(program_, vs);
    glAttachShader(program_, fs);
    glLinkProgram(program_);
    glDeleteShader(vs);
    glDeleteShader(fs);

    locProj_ = glGetUniformLocation(program_, "uProj");

    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, x));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, u));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, r));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, useTex));
    glEnableVertexAttribArray(4);
    glVertexAttribPointer(4, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, rectW));

    glBindVertexArray(0);

    glGenTextures(1, &atlasTex_);
    glBindTexture(GL_TEXTURE_2D, atlasTex_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    return true;
}

void Renderer::Shutdown() {
    if (program_) glDeleteProgram(program_);
    if (vao_) glDeleteVertexArrays(1, &vao_);
    if (vbo_) glDeleteBuffers(1, &vbo_);
    if (atlasTex_) glDeleteTextures(1, &atlasTex_);
}

void Renderer::BeginFrame(int bufW, int bufH, int logicW, int logicH, const FontManager& fm) {
    vpW_ = bufW;
    vpH_ = bufH;
    logicW_ = logicW;
    logicH_ = logicH;
    pxScale_ = (logicW > 0) ? (float)bufW / logicW : 1.0f;
    fm_ = &fm;
    batch_.clear();
    batch_.reserve(8192);

    glViewport(0, 0, vpW_, vpH_);
    glClearColor(0.11f, 0.12f, 0.15f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    UpdateAtlasTexture();
}

void Renderer::EndFrame() {
    UpdateAtlasTexture();
    FlushBatch();
}

void Renderer::UpdateAtlasTexture() {
    if (!fm_) return;
    size_t gen = fm_->AtlasGeneration();
    if (gen == lastAtlasGen_) return;
    lastAtlasGen_ = gen;

    glBindTexture(GL_TEXTURE_2D, atlasTex_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, fm_->AtlasWidth(), fm_->AtlasHeight(),
                 0, GL_RED, GL_UNSIGNED_BYTE, fm_->AtlasData());
}

void Renderer::FlushBatch() {
    if (batch_.empty()) return;

    glUseProgram(program_);

    float proj[16] = {};
    proj[0] = 2.0f / logicW_;
    proj[5] = -2.0f / logicH_;
    proj[10] = -1.0f;
    proj[12] = -1.0f;
    proj[13] = 1.0f;
    proj[15] = 1.0f;
    glUniformMatrix4fv(locProj_, 1, GL_FALSE, proj);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, atlasTex_);

    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, batch_.size() * sizeof(Vertex), batch_.data(), GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)batch_.size());
    glBindVertexArray(0);

    batch_.clear();
}

void Renderer::PushQuad(float x0, float y0, float x1, float y1,
                         float u0, float v0, float u1, float v1,
                         const Color& c, float useTex) {
    Vertex v{};
    v.r = c.r; v.g = c.g; v.b = c.b; v.a = c.a;
    v.useTex = useTex;

    v.x=x0; v.y=y0; v.u=u0; v.v=v0; batch_.push_back(v);
    v.x=x1; v.y=y0; v.u=u1; v.v=v0; batch_.push_back(v);
    v.x=x1; v.y=y1; v.u=u1; v.v=v1; batch_.push_back(v);
    v.x=x0; v.y=y0; v.u=u0; v.v=v0; batch_.push_back(v);
    v.x=x1; v.y=y1; v.u=u1; v.v=v1; batch_.push_back(v);
    v.x=x0; v.y=y1; v.u=u0; v.v=v1; batch_.push_back(v);
}

void Renderer::DrawRect(float x, float y, float w, float h, const Color& c) {
    PushQuad(x, y, x+w, y+h, 0,0,0,0, c, 0.0f);
}

void Renderer::DrawTriangle(float x1, float y1, float x2, float y2, float x3, float y3, const Color& c) {
    Vertex v{};
    v.r = c.r; v.g = c.g; v.b = c.b; v.a = c.a;
    v.useTex = 0.0f;
    v.x = x1; v.y = y1; batch_.push_back(v);
    v.x = x2; v.y = y2; batch_.push_back(v);
    v.x = x3; v.y = y3; batch_.push_back(v);
}

void Renderer::DrawRoundedRect(float x, float y, float w, float h, float radius, const Color& c) {
    if (radius < 0.5f) { DrawRect(x, y, w, h, c); return; }
    Vertex v{};
    v.r = c.r; v.g = c.g; v.b = c.b; v.a = c.a;
    v.useTex = 2.0f + radius;
    v.rectW = w; v.rectH = h;

    v.x=x;   v.y=y;   v.u=0; v.v=0; batch_.push_back(v);
    v.x=x+w; v.y=y;   v.u=w; v.v=0; batch_.push_back(v);
    v.x=x+w; v.y=y+h; v.u=w; v.v=h; batch_.push_back(v);
    v.x=x;   v.y=y;   v.u=0; v.v=0; batch_.push_back(v);
    v.x=x+w; v.y=y+h; v.u=w; v.v=h; batch_.push_back(v);
    v.x=x;   v.y=y+h; v.u=0; v.v=h; batch_.push_back(v);
}

void Renderer::DrawShapedRun(const ShapedRun& run, float x, float y, const Color& c) {
    if (!fm_) return;
    float s = pxScale_;
    float asc = fm_->Ascent() / s;
    for (auto& g : run.glyphs) {
        const GlyphInfo& gi = g.cached ? *g.cached : fm_->EnsureGlyph(g.glyphId, g.faceIdx);
        if (gi.width == 0 || gi.height == 0) continue;

        float gx = x + g.xPos / s + gi.bearingX / s;
        float gy = y + asc - gi.bearingY / s;
        float gw = gi.width / s;
        float gh = gi.height / s;

        float atlasW = (float)fm_->AtlasWidth();
        float atlasH = (float)fm_->AtlasHeight();

        float u0 = gi.atlasX / atlasW;
        float v0 = gi.atlasY / atlasH;
        float u1 = (gi.atlasX + gi.width) / atlasW;
        float v1 = (gi.atlasY + gi.height) / atlasH;

        PushQuad(gx, gy, gx + gw, gy + gh, u0, v0, u1, v1, c, 1.0f);
    }
}

void Renderer::DrawText(const std::string& text, float x, float y, const Color& c) {
    if (!fm_ || text.empty()) return;
    ShapedRun run = fm_->Shape(text);
    DrawShapedRun(run, x, y, c);
}

float Renderer::MeasureText(const std::string& text) const {
    if (!fm_ || text.empty()) return 0;
    return fm_->MeasureWidth(text) / pxScale_;
}

void Renderer::PushClip(float x, float y, float w, float h) {
    UpdateAtlasTexture();
    FlushBatch();
    glEnable(GL_SCISSOR_TEST);
    int sx = (int)(x * pxScale_);
    int sy = vpH_ - (int)((y + h) * pxScale_);
    int sw = (int)(w * pxScale_);
    int sh = (int)(h * pxScale_);
    glScissor(sx, sy, sw, sh);
}

void Renderer::PopClip() {
    UpdateAtlasTexture();
    FlushBatch();
    glDisable(GL_SCISSOR_TEST);
}

void Renderer::DrawTextDirect(const FontManager& fm, int bufW, int bufH, int logicW, int logicH,
                               const std::string& text, float x, float y, const Color& c) {
    // Save state
    int oldVpW = vpW_, oldVpH = vpH_;
    int oldLogicW = logicW_, oldLogicH = logicH_;
    float oldPxScale = pxScale_;
    const FontManager* oldFm = fm_;

    // Set new state
    vpW_ = bufW; vpH_ = bufH;
    logicW_ = logicW; logicH_ = logicH;
    pxScale_ = (logicW > 0) ? (float)bufW / logicW : 1.0f;
    fm_ = &fm;

    glViewport(0, 0, vpW_, vpH_);
    UpdateAtlasTexture();

    DrawText(text, x, y, c);
    EndFrame();

    // Restore state
    vpW_ = oldVpW; vpH_ = oldVpH;
    logicW_ = oldLogicW; logicH_ = oldLogicH;
    pxScale_ = oldPxScale;
    fm_ = oldFm;
}
