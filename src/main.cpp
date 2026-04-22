// Time Tracker - Platform layer (Wayland + X11)
// Smooth-resize OpenGL demo with Wayland (primary) and X11 (fallback) backends.
//
// Wayland backend: patterned on GTK4's GDK Wayland backend
// (gdk/wayland/gdksurface-wayland.c, gdkglcontext-wayland.c):
//
//   1. xdg_surface.configure: ack_configure IMMEDIATELY, stash size, mark dirty.
//   2. Rendering is paced by wl_surface.frame callbacks.
//   3. Size changes apply at the start of the next render.
//   4. eglSwapInterval(0): let compositor's frame callbacks pace us.
//   5. Own CSD drawn directly into GL surface — one atomic commit.
//
// X11 backend: EGL window with _NET_WM_SYNC_REQUEST for smooth resize.

#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glext.h>

#ifdef HAVE_WAYLAND
#include <wayland-client.h>
#include <wayland-egl.h>
#include <EGL/egl.h>
#include <wayland-cursor.h>
#include "xdg-shell-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"
#include <xkbcommon/xkbcommon.h>
#endif

#ifdef HAVE_X11
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/extensions/sync.h>
#include <X11/keysym.h>
#include <EGL/egl.h>
#endif

#include <poll.h>
#include <cerrno>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "app.h"

namespace {

constexpr int TITLEBAR_H     = 40;
constexpr int BORDER_GRAB    = 6;
constexpr int CLOSE_SIZE     = 24;
constexpr int CLOSE_MARGIN   = 8;
constexpr int SHADOW_EXTENT  = 32;
constexpr int CORNER_RADIUS  = 12;
constexpr float SHADOW_SIGMA = 14.0f;

constexpr float TITLEBAR_R = 0.13f;
constexpr float TITLEBAR_G = 0.14f;
constexpr float TITLEBAR_B = 0.18f;

App g_app;

// ============================================================================
// Shared GL helpers
// ============================================================================

bool compile_shader(GLuint shader, const char* src) {
    glShaderSource(shader, 1, &src, nullptr);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        std::fprintf(stderr, "shader compile error: %s\n", log);
        return false;
    }
    return true;
}

GLuint link_program(const char* vs_src, const char* fs_src) {
    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    if (!compile_shader(vs, vs_src) || !compile_shader(fs, fs_src)) return 0;
    GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glLinkProgram(p);
    GLint linked = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[1024];
        glGetProgramInfoLog(p, sizeof(log), nullptr, log);
        std::fprintf(stderr, "program link error: %s\n", log);
        glDeleteProgram(p);
        p = 0;
    }
    glDeleteShader(vs);
    glDeleteShader(fs);
    return p;
}

// ============================================================================
// Wayland backend
// ============================================================================

#ifdef HAVE_WAYLAND

const char* kCloseVS = R"(#version 330 core
layout(location = 0) in vec2 aPos;
uniform vec4 uRect;
uniform vec2 uScreen;
out vec2 vUV;
void main() {
    vec2 px  = uRect.xy + aPos * uRect.zw;
    vec2 ndc = vec2(px.x / uScreen.x, 1.0 - px.y / uScreen.y) * 2.0 - 1.0;
    gl_Position = vec4(ndc, 0.0, 1.0);
    vUV = aPos;
}
)";

const char* kCloseFS = R"(#version 330 core
in  vec2 vUV;
out vec4 fragColor;
uniform float uHover;
uniform float uPressed;
uniform vec3  uBarColor;
void main() {
    vec2  p  = vUV * 2.0 - 1.0;
    float r  = length(p);
    float aa = fwidth(r);
    float disc = 1.0 - smoothstep(1.0 - aa, 1.0, r);
    mat2 R = mat2(0.70710678, -0.70710678, 0.70710678, 0.70710678);
    vec2 q = R * p;
    const float cap_r = 0.055, cap_L = 0.34;
    float d1 = length(vec2(max(abs(q.x) - cap_L, 0.0), q.y)) - cap_r;
    float d2 = length(vec2(max(abs(q.y) - cap_L, 0.0), q.x)) - cap_r;
    float dx  = min(d1, d2);
    float aax = fwidth(dx);
    float xmask = 1.0 - smoothstep(0.0, aax, dx);
    vec3 normal_c  = vec3(0.245, 0.255, 0.295);
    vec3 hover_c   = vec3(0.320, 0.330, 0.370);
    vec3 pressed_c = vec3(0.420, 0.430, 0.470);
    vec3 bg = mix(normal_c, hover_c, uHover);
    bg      = mix(bg, pressed_c, uPressed);
    vec3 disc_col = mix(bg, vec3(1.0), xmask);
    vec3 col      = mix(uBarColor, disc_col, disc);
    fragColor     = vec4(col, 1.0);
}
)";

const char* kFullQuadVS = R"(#version 330 core
layout(location = 0) in vec2 aPos;
out vec2 vPx;
out vec2 vUV;
uniform vec2 uScreen;
void main() {
    vec2 px  = aPos * uScreen;
    vec2 ndc = vec2(aPos.x, 1.0 - aPos.y) * 2.0 - 1.0;
    gl_Position = vec4(ndc, 0.0, 1.0);
    vPx = px;
    vUV = vec2(aPos.x, 1.0 - aPos.y);
}
)";

const char* kComposeFS = R"(#version 330 core
in  vec2 vPx;
in  vec2 vUV;
out vec4 fragColor;
uniform sampler2D uTex;
uniform vec2  uScreen;
uniform vec4  uWindow;
uniform float uRadius;
uniform float uSigma;
uniform float uShadow;
uniform float uOutline;
void main() {
    vec2 center = uWindow.xy + uWindow.zw * 0.5;
    vec2 halfs  = uWindow.zw * 0.5;
    vec2 q = abs(vPx - center) - halfs + vec2(uRadius);
    float sdf = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - uRadius;
    float fw = fwidth(sdf);
    float inside = 1.0 - smoothstep(-fw * 0.5, fw * 0.5, sdf);
    float outline_band = smoothstep(-fw * 1.5, -fw * 0.5, sdf) *
                         (1.0 - smoothstep(-fw * 0.5, fw * 0.5, sdf));
    vec4 content = texture(uTex, vUV);
    vec3 content_rgb = mix(content.rgb, content.rgb * (1.0 - uOutline), outline_band);
    float d = max(sdf, 0.0);
    float shadow_alpha = uShadow * exp(-(d * d) / (2.0 * uSigma * uSigma));
    float out_a = inside + shadow_alpha * (1.0 - inside);
    vec3  out_rgb = content_rgb * inside / max(out_a, 1e-4);
    fragColor = vec4(out_rgb, out_a);
}
)";

struct WaylandApp {
    wl_display*         display        = nullptr;
    wl_compositor*      compositor     = nullptr;
    xdg_wm_base*        wm_base        = nullptr;
    zxdg_decoration_manager_v1* deco_mgr = nullptr;
    zxdg_toplevel_decoration_v1* deco    = nullptr;
    wl_seat*            seat           = nullptr;
    wl_pointer*         pointer        = nullptr;
    wl_keyboard*        keyboard       = nullptr;
    wl_shm*             shm            = nullptr;

    bool                use_csd        = true;

    wl_cursor_theme* cursor_theme   = nullptr;
    wl_surface*      cursor_surface = nullptr;

        // Outputs & scale
    struct OutputState {
        wl_output* output = nullptr;
        int scale = 1;
    };
    std::vector<OutputState> outputs;
    int compositor_max_scale = 1;

    GLuint close_prog      = 0;
    GLuint close_vao       = 0;
    GLuint close_vbo       = 0;
    GLint  uRect_loc       = -1;
    GLint  uScreen_loc     = -1;
    GLint  uHover_loc      = -1;
    GLint  uPressed_loc    = -1;
    GLint  uBarColor_loc   = -1;
    float  close_hover_amt = 0.0f;
    bool   close_pressed   = false;

    GLuint fbo     = 0;
    GLuint fbo_tex = 0;
    int    fbo_w   = 0;
    int    fbo_h   = 0;

    GLuint content_fbo     = 0;
    GLuint content_fbo_tex = 0;
    int    content_fbo_w   = 0;
    int    content_fbo_h   = 0;

    GLuint quad_vao = 0;
    GLuint quad_vbo = 0;

    GLuint compose_prog           = 0;
    GLint  compose_uTex_loc       = -1;
    GLint  compose_uScreen_loc    = -1;
    GLint  compose_uWindow_loc    = -1;
    GLint  compose_uRadius_loc    = -1;
    GLint  compose_uSigma_loc     = -1;
    GLint  compose_uShadow_loc    = -1;
    GLint  compose_uOutline_loc   = -1;

    wl_surface*    surface   = nullptr;
    xdg_surface*   xsurface  = nullptr;
    xdg_toplevel*  toplevel  = nullptr;

    wl_egl_window* egl_window  = nullptr;
    EGLDisplay     egl_display = EGL_NO_DISPLAY;
    EGLContext     egl_context = EGL_NO_CONTEXT;
    EGLSurface     egl_surface = EGL_NO_SURFACE;
    EGLConfig      egl_config  = nullptr;

    int min_width  = 400;
    int min_height = 560;

    int width          = 520;
    int height         = 640;
    int pending_width  = 0;
    int pending_height = 0;

    int scale            = 1;
    int applied_buffer_w = 0;
    int applied_buffer_h = 0;

    bool egl_ready     = false;
    bool running       = true;
    bool dirty         = true;
    bool frame_pending = false;

    bool tiled      = false;
    bool maximized  = false;
    bool fullscreen = false;

    double      px = 0, py = 0;
    bool        hover_close       = false;
    uint32_t    last_enter_serial = 0;
    const char* current_cursor    = nullptr;

    bool keyboard_focus = false;

    // Keyboard state
    xkb_context* xkb_ctx = nullptr;
    xkb_keymap*  xkb_km = nullptr;
    xkb_state*   xkb_st = nullptr;
    uint32_t     mods_depressed = 0;
    uint32_t     mods_latched = 0;
    uint32_t     mods_locked = 0;
    uint32_t     group = 0;
};

bool wl_floating(const WaylandApp& app) {
    return !app.tiled && !app.maximized && !app.fullscreen;
}
int wl_eff_shadow(const WaylandApp& app) { return (app.use_csd && wl_floating(app)) ? SHADOW_EXTENT : 0; }
int wl_eff_radius(const WaylandApp& app) { return (app.use_csd && wl_floating(app)) ? CORNER_RADIUS : 0; }

bool init_close_button(WaylandApp& app) {
    app.close_prog = link_program(kCloseVS, kCloseFS);
    if (!app.close_prog) return false;
    app.uRect_loc     = glGetUniformLocation(app.close_prog, "uRect");
    app.uScreen_loc   = glGetUniformLocation(app.close_prog, "uScreen");
    app.uHover_loc    = glGetUniformLocation(app.close_prog, "uHover");
    app.uPressed_loc  = glGetUniformLocation(app.close_prog, "uPressed");
    app.uBarColor_loc = glGetUniformLocation(app.close_prog, "uBarColor");

    const float quad[] = {0.0f,0.0f, 1.0f,0.0f, 1.0f,1.0f, 0.0f,0.0f, 1.0f,1.0f, 0.0f,1.0f};
    glGenVertexArrays(1, &app.close_vao);
    glBindVertexArray(app.close_vao);
    glGenBuffers(1, &app.close_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, app.close_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glBindVertexArray(0);
    return true;
}

bool init_compose(WaylandApp& app) {
    app.compose_prog = link_program(kFullQuadVS, kComposeFS);
    if (!app.compose_prog) return false;
    app.compose_uTex_loc     = glGetUniformLocation(app.compose_prog, "uTex");
    app.compose_uScreen_loc  = glGetUniformLocation(app.compose_prog, "uScreen");
    app.compose_uWindow_loc  = glGetUniformLocation(app.compose_prog, "uWindow");
    app.compose_uRadius_loc  = glGetUniformLocation(app.compose_prog, "uRadius");
    app.compose_uSigma_loc   = glGetUniformLocation(app.compose_prog, "uSigma");
    app.compose_uShadow_loc  = glGetUniformLocation(app.compose_prog, "uShadow");
    app.compose_uOutline_loc = glGetUniformLocation(app.compose_prog, "uOutline");

    const float quad[] = {0.0f,0.0f, 1.0f,0.0f, 1.0f,1.0f, 0.0f,0.0f, 1.0f,1.0f, 0.0f,1.0f};
    glGenVertexArrays(1, &app.quad_vao);
    glBindVertexArray(app.quad_vao);
    glGenBuffers(1, &app.quad_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, app.quad_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glBindVertexArray(0);
    return true;
}

void ensure_fbo(WaylandApp& app, int w, int h) {
    if (w == app.fbo_w && h == app.fbo_h && app.fbo) return;
    if (!app.fbo)     glGenFramebuffers(1, &app.fbo);
    if (!app.fbo_tex) glGenTextures(1, &app.fbo_tex);

    glBindTexture(GL_TEXTURE_2D, app.fbo_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    glBindFramebuffer(GL_FRAMEBUFFER, app.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, app.fbo_tex, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    app.fbo_w = w; app.fbo_h = h;
}

void ensure_content_fbo(WaylandApp& app, int w, int h) {
    if (w == app.content_fbo_w && h == app.content_fbo_h && app.content_fbo) return;
    if (!app.content_fbo)     glGenFramebuffers(1, &app.content_fbo);
    if (!app.content_fbo_tex) glGenTextures(1, &app.content_fbo_tex);

    glBindTexture(GL_TEXTURE_2D, app.content_fbo_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    glBindFramebuffer(GL_FRAMEBUFFER, app.content_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, app.content_fbo_tex, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    app.content_fbo_w = w; app.content_fbo_h = h;
}

struct CloseRect { int x, y, w, h; };

CloseRect close_rect_buf(const WaylandApp& app) {
    const int S  = app.scale;
    const int sh = wl_eff_shadow(app);
    const int x = (sh + app.width - CLOSE_MARGIN - CLOSE_SIZE) * S;
    const int y = (sh + (TITLEBAR_H - CLOSE_SIZE) / 2) * S;
    return { x, y, CLOSE_SIZE * S, CLOSE_SIZE * S };
}

void draw_close_button(const WaylandApp& app) {
    const int S  = app.scale;
    const int sh = wl_eff_shadow(app);
    const int W  = (app.width  + 2 * sh) * S;
    const int H  = (app.height + 2 * sh) * S;
    const auto r = close_rect_buf(app);

    glUseProgram(app.close_prog);
    glUniform4f(app.uRect_loc, float(r.x), float(r.y), float(r.w), float(r.h));
    glUniform2f(app.uScreen_loc, float(W), float(H));
    glUniform1f(app.uHover_loc, app.close_hover_amt);
    glUniform1f(app.uPressed_loc, app.close_pressed ? 1.0f : 0.0f);
    glUniform3f(app.uBarColor_loc, TITLEBAR_R, TITLEBAR_G, TITLEBAR_B);

    glBindVertexArray(app.close_vao);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);
    glUseProgram(0);
}

void wl_render_app(WaylandApp& app) {
    const int S    = app.scale;

    if (!app.use_csd) {
        // SSD: no titlebar, render straight to screen
        const int winW = app.width  * S;
        const int winH = app.height * S;
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, winW, winH);
        g_app.winW = app.width;
        g_app.winH = app.height;
        g_app.bufW = winW;
        g_app.bufH = winH;
        g_app.scale = app.scale;
        g_app.Paint();
        return;
    }

    // CSD mode: render into FBOs with shadow + titlebar + content + close button

    const int sh  = wl_eff_shadow(app);
    const int rad = wl_eff_radius(app);
    const int W   = (app.width  + 2 * sh) * S;
    const int H   = (app.height + 2 * sh) * S;

    // Content area dimensions (window minus titlebar)
    const int contentW = app.width * S;
    const int contentH = (app.height - TITLEBAR_H) * S;

    // 1) Render app content into its own FBO
    ensure_content_fbo(app, contentW, contentH);
    glBindFramebuffer(GL_FRAMEBUFFER, app.content_fbo);
    glViewport(0, 0, contentW, contentH);
    glClearColor(0.11f, 0.12f, 0.15f, 1.0f);  // BG color
    glClear(GL_COLOR_BUFFER_BIT);

    g_app.winW = app.width;
    g_app.winH = app.height - TITLEBAR_H;
    g_app.bufW = contentW;
    g_app.bufH = contentH;
    g_app.scale = app.scale;
    g_app.Paint();

    // 2) Compose CSD frame into main FBO
    ensure_fbo(app, W, H);
    glBindFramebuffer(GL_FRAMEBUFFER, app.fbo);
    glViewport(0, 0, W, H);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    // Draw titlebar (in GL coords: y=H minus shadow is top, titlebar below that)
    // Titlebar spans the full window width, just below the top shadow
    // In GL coords (y=0 bottom): titlebar bottom = H - (sh + TITLEBAR_H)*S, height = TITLEBAR_H*S
    glEnable(GL_SCISSOR_TEST);
    glScissor(sh * S, H - (sh + TITLEBAR_H) * S, app.width * S, TITLEBAR_H * S);
    glClearColor(TITLEBAR_R, TITLEBAR_G, TITLEBAR_B, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);

    // Draw title text into title bar
    {
        int oldSize = g_app.fontManager.CurrentSize();
        g_app.fontManager.SetSize(20 * S);
        float textW = g_app.renderer.MeasureText("TT");
        float textX = sh * S + (app.width * S - textW) / 2.0f;
        float textY = sh * S + (TITLEBAR_H * S - g_app.fontManager.LineHeight()) / 2.0f;
        g_app.renderer.DrawTextDirect(g_app.fontManager, W, H, W, H,
                                       "TT", textX, textY, Color(1.0f, 1.0f, 1.0f, 1.0f));
        g_app.fontManager.SetSize(oldSize);
    }

    // Blit content FBO into correct position in main FBO
    // Content sits below the titlebar, inside the shadow margins
    // In GL y-up coords: content occupies (sh*S, sh*S) to (sh*S+contentW, sh*S+contentH)
    glBindFramebuffer(GL_READ_FRAMEBUFFER, app.content_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, app.fbo);
    glBlitFramebuffer(
        0, 0, contentW, contentH,
        sh * S, sh * S, sh * S + contentW, sh * S + contentH,
        GL_COLOR_BUFFER_BIT, GL_NEAREST);

    // Reset for drawing the close button
    glBindFramebuffer(GL_FRAMEBUFFER, app.fbo);
    glViewport(0, 0, W, H);
    draw_close_button(app);

    // 3) Final compose pass: shadow + rounded corners onto screen
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, W, H);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    const int winW = app.width * S;
    const int winH = app.height * S;
    const int wx   = sh * S;
    const int wy   = sh * S;

    glUseProgram(app.compose_prog);
    glUniform2f(app.compose_uScreen_loc, float(W), float(H));
    glUniform4f(app.compose_uWindow_loc, float(wx), float(wy), float(winW), float(winH));
    glUniform1f(app.compose_uRadius_loc, float(rad * S));
    glUniform1f(app.compose_uSigma_loc,  SHADOW_SIGMA * S);
    glUniform1f(app.compose_uShadow_loc,  wl_floating(app) ? 0.275f : 0.0f);
    glUniform1f(app.compose_uOutline_loc, wl_floating(app) ? 0.18f  : 0.0f);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, app.fbo_tex);
    glUniform1i(app.compose_uTex_loc, 0);

    glBindVertexArray(app.quad_vao);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);
    glUseProgram(0);
}

void wl_commit_frame(WaylandApp& app);

void frame_done(void* data, wl_callback* cb, uint32_t) {
    wl_callback_destroy(cb);
    auto& app = *static_cast<WaylandApp*>(data);
    app.frame_pending = false;
    if (app.dirty && app.egl_ready) wl_commit_frame(app);
}
const wl_callback_listener frame_listener = { .done = frame_done };

void wl_sync_surface_geometry(WaylandApp& app) {
    if (!app.use_csd) {
        xdg_surface_set_window_geometry(app.xsurface, 0, 0, app.width, app.height);
        wl_surface_set_input_region(app.surface, nullptr);
        if (wl_region* r = wl_compositor_create_region(app.compositor)) {
            wl_region_add(r, 0, 0, app.width, app.height);
            wl_surface_set_opaque_region(app.surface, r);
            wl_region_destroy(r);
        }
        return;
    }

    const int sh = wl_eff_shadow(app);
    xdg_surface_set_window_geometry(app.xsurface, sh, sh, app.width, app.height);

    if (wl_region* r = wl_compositor_create_region(app.compositor)) {
        wl_region_add(r, sh, sh, app.width, app.height);
        wl_surface_set_input_region(app.surface, r);
        wl_region_destroy(r);
    }
    if (wl_region* r = wl_compositor_create_region(app.compositor)) {
        const int rad = wl_eff_radius(app);
        if (app.width > 2 * rad && app.height > 2 * rad) {
            wl_region_add(r, sh + rad, sh + rad, app.width - 2 * rad, app.height - 2 * rad);
        }
        wl_surface_set_opaque_region(app.surface, r);
        wl_region_destroy(r);
    }
}

bool wl_needs_continuous_frames(const WaylandApp& app) {
    // Animations
    if (app.use_csd && std::abs(app.close_hover_amt - (app.hover_close ? 1.0f : 0.0f)) > 0.01f)
        return true;
    if (std::abs(g_app.summaryExpandAnim - (g_app.summaryExpanded ? 1.0f : 0.0f)) > 0.001f)
        return true;
    // Time-based visuals
    if (g_app.activeTask >= 0) return true;
    if (g_app.inputFocused) return true;
    return false;
}

int wl_next_timeout_ms() {
    auto now = std::chrono::steady_clock::now();
    auto next = now + std::chrono::hours(24);

    // Active task: next second boundary
    if (g_app.activeTask >= 0 && g_app.activeTask < (int)g_app.tasks.size()) {
        auto elapsed = now - g_app.tasks[g_app.activeTask].startTime;
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
        auto next_sec = now + std::chrono::milliseconds(1000 - (ms % 1000));
        if (next_sec < next) next = next_sec;
    }
    // Cursor blink: every 500ms
    if (g_app.inputFocused) {
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
        auto next_blink = now + std::chrono::milliseconds(500 - (ms % 500));
        if (next_blink < next) next = next_blink;
    }
    // Auto-save: every 30s
    {
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - g_app.lastSaveTime).count();
        auto next_save = now + std::chrono::seconds(30 - (elapsed % 30));
        if (next_save < next) next = next_save;
    }

    if (next > now + std::chrono::hours(23)) return -1;
    int ms = (int)std::chrono::duration_cast<std::chrono::milliseconds>(next - now).count();
    return std::max(0, ms);
}

void wl_commit_frame(WaylandApp& app) {
    if (!app.egl_ready || !app.dirty || app.frame_pending) return;

    if (app.pending_width > 0 && app.pending_height > 0) {
        app.width  = app.pending_width;
        app.height = app.pending_height;
    }
    const int sh = wl_eff_shadow(app);
    const int want_w = (app.width  + 2 * sh) * app.scale;
    const int want_h = (app.height + 2 * sh) * app.scale;
    if (want_w != app.applied_buffer_w || want_h != app.applied_buffer_h) {
        wl_egl_window_resize(app.egl_window, want_w, want_h, 0, 0);
        app.applied_buffer_w = want_w;
        app.applied_buffer_h = want_h;
        wl_sync_surface_geometry(app);
    }

    if (app.use_csd) {
        const float target = app.hover_close ? 1.0f : 0.0f;
        const float step = 0.18f;
        if (target > app.close_hover_amt)
            app.close_hover_amt = std::min(target, app.close_hover_amt + step);
        else
            app.close_hover_amt = std::max(target, app.close_hover_amt - step);
    }

    wl_render_app(app);

    // Only request another frame callback if continuous rendering needed
    if (wl_needs_continuous_frames(app)) {
        wl_callback* cb = wl_surface_frame(app.surface);
        wl_callback_add_listener(cb, &frame_listener, &app);
        app.frame_pending = true;
    }

    eglSwapBuffers(app.egl_display, app.egl_surface);
    app.dirty = false;
}

void xdg_surface_configure(void* data, xdg_surface* s, uint32_t serial) {
    auto& app = *static_cast<WaylandApp*>(data);
    xdg_surface_ack_configure(s, serial);
    app.dirty = true;
    if (app.egl_ready && !app.frame_pending) wl_commit_frame(app);
}
const xdg_surface_listener xdg_surface_listener_impl = { .configure = xdg_surface_configure };

void surface_preferred_buffer_scale(void* data, wl_surface*, int32_t factor) {
    auto& app = *static_cast<WaylandApp*>(data);
    if (factor < 1) factor = 1;
    if (factor == app.scale) return;
    app.scale = factor;
    app.compositor_max_scale = factor;
    wl_surface_set_buffer_scale(app.surface, factor);
    app.applied_buffer_w = 0; app.applied_buffer_h = 0;
    app.dirty = true;
    if (app.egl_ready && !app.frame_pending) wl_commit_frame(app);
}
void surface_preferred_buffer_transform(void*, wl_surface*, uint32_t) {}
void surface_enter(void* data, wl_surface*, wl_output* output) {
    auto& app = *static_cast<WaylandApp*>(data);
    // Mark this output as relevant
    for (auto& o : app.outputs) {
        if (o.output == output) { break; }
    }
}
void surface_leave(void* data, wl_surface*, wl_output*) {
}
const wl_surface_listener surface_listener_impl = {
    .enter  = surface_enter, .leave = surface_leave,
    .preferred_buffer_scale = surface_preferred_buffer_scale,
    .preferred_buffer_transform = surface_preferred_buffer_transform,
};

void toplevel_configure(void* data, xdg_toplevel*, int32_t w, int32_t h, wl_array* states) {
    auto& app = *static_cast<WaylandApp*>(data);
    if (w > 0) app.pending_width  = std::max(w, app.min_width);
    if (h > 0) app.pending_height = std::max(h, app.min_height);

    bool tiled = false, max = false, full = false;
    uint32_t* s;
    for (s = (uint32_t*)states->data; (const char*)s < (const char*)states->data + states->size; ++s) {
        switch (*s) {
        case XDG_TOPLEVEL_STATE_MAXIMIZED:    max  = true; break;
        case XDG_TOPLEVEL_STATE_FULLSCREEN:   full = true; break;
        case XDG_TOPLEVEL_STATE_TILED_LEFT:
        case XDG_TOPLEVEL_STATE_TILED_RIGHT:
        case XDG_TOPLEVEL_STATE_TILED_TOP:
        case XDG_TOPLEVEL_STATE_TILED_BOTTOM: tiled = true; break;
        }
    }
    const bool was_floating = wl_floating(app);
    app.tiled = tiled; app.maximized = max; app.fullscreen = full;
    if (was_floating != wl_floating(app)) {
        app.applied_buffer_w = 0; app.applied_buffer_h = 0;
    }
}
void toplevel_close(void* data, xdg_toplevel*) { static_cast<WaylandApp*>(data)->running = false; }
const xdg_toplevel_listener toplevel_listener_impl = { .configure = toplevel_configure, .close = toplevel_close };

void toplevel_decoration_configure(void* data, zxdg_toplevel_decoration_v1*, uint32_t mode) {
    auto& app = *static_cast<WaylandApp*>(data);
    bool was_csd = app.use_csd;
    app.use_csd = (mode != ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    if (was_csd != app.use_csd) {
        app.applied_buffer_w = 0; app.applied_buffer_h = 0;
        app.dirty = true;
        if (app.egl_ready && !app.frame_pending) wl_commit_frame(app);
    }
}
const zxdg_toplevel_decoration_v1_listener deco_listener_impl = { .configure = toplevel_decoration_configure };

uint32_t edge_for(const WaylandApp& app, double x, double y) {
    const bool L = x < BORDER_GRAB, R = x > app.width - BORDER_GRAB;
    const bool T = y < BORDER_GRAB, B = y > app.height - BORDER_GRAB;
    if (T && L) return XDG_TOPLEVEL_RESIZE_EDGE_TOP_LEFT;
    if (T && R) return XDG_TOPLEVEL_RESIZE_EDGE_TOP_RIGHT;
    if (B && L) return XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_LEFT;
    if (B && R) return XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_RIGHT;
    if (L)      return XDG_TOPLEVEL_RESIZE_EDGE_LEFT;
    if (R)      return XDG_TOPLEVEL_RESIZE_EDGE_RIGHT;
    if (T)      return XDG_TOPLEVEL_RESIZE_EDGE_TOP;
    if (B)      return XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM;
    return XDG_TOPLEVEL_RESIZE_EDGE_NONE;
}

const char* cursor_name_for_edge(uint32_t edge) {
    switch (edge) {
    case XDG_TOPLEVEL_RESIZE_EDGE_TOP:          return "n-resize";
    case XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM:       return "s-resize";
    case XDG_TOPLEVEL_RESIZE_EDGE_LEFT:         return "w-resize";
    case XDG_TOPLEVEL_RESIZE_EDGE_RIGHT:        return "e-resize";
    case XDG_TOPLEVEL_RESIZE_EDGE_TOP_LEFT:     return "nw-resize";
    case XDG_TOPLEVEL_RESIZE_EDGE_TOP_RIGHT:    return "ne-resize";
    case XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_LEFT:  return "sw-resize";
    case XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_RIGHT: return "se-resize";
    default:                                    return "default";
    }
}

wl_cursor* lookup_cursor(wl_cursor_theme* theme, const char* name) {
    if (wl_cursor* c = wl_cursor_theme_get_cursor(theme, name)) return c;
    struct { const char* css; const char* legacy; } alias[] = {
        { "default", "left_ptr" }, { "n-resize", "top_side" },
        { "s-resize", "bottom_side" }, { "w-resize", "left_side" },
        { "e-resize", "right_side" }, { "nw-resize", "top_left_corner" },
        { "ne-resize", "top_right_corner" }, { "sw-resize", "bottom_left_corner" },
        { "se-resize", "bottom_right_corner" },
    };
    for (const auto& a : alias) {
        if (std::strcmp(name, a.css) == 0)
            if (wl_cursor* c = wl_cursor_theme_get_cursor(theme, a.legacy)) return c;
    }
    return nullptr;
}

bool in_close(const WaylandApp& app, double x, double y) {
    const int cx = app.width - CLOSE_MARGIN - CLOSE_SIZE;
    const int cy = (TITLEBAR_H - CLOSE_SIZE) / 2;
    return x >= cx && x < cx + CLOSE_SIZE && y >= cy && y < cy + CLOSE_SIZE;
}

void set_cursor(WaylandApp& app, const char* name) {
    if (!app.pointer || !app.cursor_theme || !app.cursor_surface) return;
    if (app.current_cursor == name) return;
    app.current_cursor = name;

    wl_cursor* cursor = lookup_cursor(app.cursor_theme, name);
    if (!cursor || cursor->image_count == 0) return;
    wl_cursor_image* image = cursor->images[0];
    wl_buffer* buffer = wl_cursor_image_get_buffer(image);
    if (!buffer) return;

    wl_pointer_set_cursor(app.pointer, app.last_enter_serial, app.cursor_surface,
                          image->hotspot_x, image->hotspot_y);
    wl_surface_attach(app.cursor_surface, buffer, 0, 0);
    wl_surface_damage_buffer(app.cursor_surface, 0, 0, image->width, image->height);
    wl_surface_commit(app.cursor_surface);
}

void update_hover(WaylandApp& app) {
    if (!app.use_csd) {
        if (app.hover_close) { app.hover_close = false; app.dirty = true; }
        set_cursor(app, "default");
        return;
    }
    const bool hov_close = in_close(app, app.px, app.py);
    if (hov_close != app.hover_close) {
        app.hover_close = hov_close; app.dirty = true;
        if (!app.frame_pending) wl_commit_frame(app);
    }
    const uint32_t edge = edge_for(app, app.px, app.py);
    const char* name = hov_close ? "default" : cursor_name_for_edge(edge);
    set_cursor(app, name);
}

void pointer_enter(void* data, wl_pointer*, uint32_t serial,
                   wl_surface*, wl_fixed_t sx, wl_fixed_t sy) {
    auto& app = *static_cast<WaylandApp*>(data);
    app.last_enter_serial = serial;
    app.current_cursor = nullptr;
    const double o = double(wl_eff_shadow(app));
    app.px = wl_fixed_to_double(sx) - o;
    app.py = wl_fixed_to_double(sy) - o;
    g_app.mx = app.px;
    g_app.my = (app.py > TITLEBAR_H) ? app.py - TITLEBAR_H : app.py;
    update_hover(app);
}
void pointer_leave(void* data, wl_pointer*, uint32_t, wl_surface*) {
    auto& app = *static_cast<WaylandApp*>(data);
    if (app.hover_close) { app.hover_close = false; app.dirty = true;
        if (!app.frame_pending) wl_commit_frame(app);
    }
}
void pointer_motion(void* data, wl_pointer*, uint32_t, wl_fixed_t sx, wl_fixed_t sy) {
    auto& app = *static_cast<WaylandApp*>(data);
    const double o = double(wl_eff_shadow(app));
    app.px = wl_fixed_to_double(sx) - o;
    app.py = wl_fixed_to_double(sy) - o;
    // Pass content-relative coords to app (subtract titlebar when in content area)
    g_app.mx = app.px;
    g_app.my = (app.py > TITLEBAR_H) ? app.py - TITLEBAR_H : app.py;
    update_hover(app);
}
void pointer_button(void* data, wl_pointer*, uint32_t serial, uint32_t,
                    uint32_t button, uint32_t state) {
    auto& app = *static_cast<WaylandApp*>(data);
    constexpr uint32_t BTN_LEFT  = 0x110;
    constexpr uint32_t BTN_RIGHT = 0x111;

    const bool pressed = state == WL_POINTER_BUTTON_STATE_PRESSED;

    if (button == BTN_RIGHT && pressed) {
        if (!app.use_csd) return;
        // Right-click on title bar: ask compositor to show window menu
        if (app.py < TITLEBAR_H && !in_close(app, app.px, app.py)) {
            xdg_toplevel_show_window_menu(app.toplevel, app.seat, serial,
                                          (int32_t)app.px, (int32_t)app.py);
        }
        return;
    }

    if (button != BTN_LEFT) return;

    g_app.mouseDown = pressed;

    if (!app.use_csd) {
        if (pressed) g_app.OnClick(app.px, app.py);
        return;
    }
    if (pressed) {
        if (in_close(app, app.px, app.py)) {
            app.close_pressed = true; return;
        }
        const uint32_t edge = edge_for(app, app.px, app.py);
        if (edge != XDG_TOPLEVEL_RESIZE_EDGE_NONE) {
            xdg_toplevel_resize(app.toplevel, app.seat, serial, edge); return;
        }
        if (app.py < TITLEBAR_H) {
            xdg_toplevel_move(app.toplevel, app.seat, serial); return;
        }
        // Click in content area - subtract titlebar offset
        g_app.OnClick(app.px, app.py - TITLEBAR_H);
    } else {
        if (app.close_pressed) {
            app.close_pressed = false;
            if (in_close(app, app.px, app.py)) app.running = false;
        }
    }
}
void pointer_axis(void*, wl_pointer*, uint32_t, uint32_t, wl_fixed_t) {}
const wl_pointer_listener pointer_listener_impl = {
    .enter = pointer_enter, .leave = pointer_leave,
    .motion = pointer_motion, .button = pointer_button, .axis = pointer_axis,
};

// Keyboard handling
void keyboard_keymap(void* data, wl_keyboard*, uint32_t format, int fd, uint32_t size) {
    auto& app = *static_cast<WaylandApp*>(data);
    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) { close(fd); return; }
    char* map_str = (char*)mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map_str == MAP_FAILED) { close(fd); return; }

    if (!app.xkb_ctx) app.xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (app.xkb_km) xkb_keymap_unref(app.xkb_km);
    if (app.xkb_st) xkb_state_unref(app.xkb_st);

    app.xkb_km = xkb_keymap_new_from_string(app.xkb_ctx, map_str, XKB_KEYMAP_FORMAT_TEXT_V1,
                                            XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(map_str, size); close(fd);

    if (app.xkb_km) app.xkb_st = xkb_state_new(app.xkb_km);
}

void keyboard_enter(void* data, wl_keyboard*, uint32_t, wl_surface*, wl_array*) {
    auto& app = *static_cast<WaylandApp*>(data);
    app.keyboard_focus = true;
}
void keyboard_leave(void* data, wl_keyboard*, uint32_t, wl_surface*) {
    auto& app = *static_cast<WaylandApp*>(data);
    app.keyboard_focus = false;
    if (g_app.inputFocused) {
        g_app.inputFocused = false;
        app.dirty = true;
        if (!app.frame_pending) wl_commit_frame(app);
    }
}

void keyboard_key(void* data, wl_keyboard*, uint32_t, uint32_t, uint32_t key, uint32_t state) {
    auto& app = *static_cast<WaylandApp*>(data);
    if (state != WL_KEYBOARD_KEY_STATE_PRESSED) return;

    xkb_keysym_t sym = XKB_KEY_NoSymbol;
    if (app.xkb_st) sym = xkb_state_key_get_one_sym(app.xkb_st, key + 8);

    uint32_t keycode = 0;
    switch (sym) {
    case XKB_KEY_BackSpace: keycode = 0x08; break;
    case XKB_KEY_Return: case XKB_KEY_KP_Enter: keycode = 0x0D; break;
    case XKB_KEY_Escape: keycode = 0x1B; break;
    case XKB_KEY_Tab: keycode = 0x09; break;
    case XKB_KEY_Left: keycode = 0xFF51; break;
    case XKB_KEY_Right: keycode = 0xFF53; break;
    case XKB_KEY_Up: keycode = 0xFF52; break;
    case XKB_KEY_Down: keycode = 0xFF54; break;
    }
    if (keycode) {
        g_app.OnKey(keycode, 0);
    } else if (app.xkb_st) {
        uint32_t cp = xkb_state_key_get_utf32(app.xkb_st, key + 8);
        if (cp != 0 && cp != 0x08 && cp != 0x0D && cp != 0x1B && cp != 0x09) {
            g_app.OnChar(cp);
        }
    }
}

void keyboard_modifiers(void* data, wl_keyboard*, uint32_t, uint32_t mods_depressed,
                        uint32_t mods_latched, uint32_t mods_locked, uint32_t group) {
    auto& app = *static_cast<WaylandApp*>(data);
    app.mods_depressed = mods_depressed; app.mods_latched = mods_latched;
    app.mods_locked = mods_locked; app.group = group;
    if (app.xkb_st) xkb_state_update_mask(app.xkb_st, mods_depressed, mods_latched, mods_locked, 0, 0, group);
}

void keyboard_repeat_info(void*, wl_keyboard*, int32_t, int32_t) {}

const wl_keyboard_listener keyboard_listener_impl = {
    .keymap = keyboard_keymap, .enter = keyboard_enter, .leave = keyboard_leave,
    .key = keyboard_key, .modifiers = keyboard_modifiers, .repeat_info = keyboard_repeat_info,
};

void seat_capabilities(void* data, wl_seat* seat, uint32_t caps) {
    auto& app = *static_cast<WaylandApp*>(data);
    const bool has_ptr = caps & WL_SEAT_CAPABILITY_POINTER;
    if (has_ptr && !app.pointer) {
        app.pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(app.pointer, &pointer_listener_impl, &app);
    } else if (!has_ptr && app.pointer) {
        wl_pointer_destroy(app.pointer); app.pointer = nullptr;
    }
    const bool has_kbd = caps & WL_SEAT_CAPABILITY_KEYBOARD;
    if (has_kbd && !app.keyboard) {
        app.keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(app.keyboard, &keyboard_listener_impl, &app);
    } else if (!has_kbd && app.keyboard) {
        wl_keyboard_destroy(app.keyboard); app.keyboard = nullptr;
    }
}
void seat_name(void*, wl_seat*, const char*) {}
const wl_seat_listener seat_listener_impl = { .capabilities = seat_capabilities, .name = seat_name };

void wm_base_ping(void*, xdg_wm_base* wm, uint32_t serial) { xdg_wm_base_pong(wm, serial); }
const xdg_wm_base_listener wm_base_listener_impl = { .ping = wm_base_ping };

// Map wl_output* → scale. We pass WaylandApp* as listener user_data.
// The output_scale callback needs to know which wl_output got the event,
// but wl_output protocol <4 doesn't pass it explicitly. We track scale per-output
// by maintaining a linked list of OutputState inside WaylandApp.
struct OutputState {
    wl_output* output = nullptr;
    int scale = 1;
};

void wl_output_geometry(void* data, wl_output*, int32_t, int32_t, int32_t, int32_t, int32_t, const char*, const char*, int32_t) {}
void wl_output_mode(void* data, wl_output*, uint32_t, int32_t, int32_t, int32_t) {}
void wl_output_done(void* data, wl_output*) {
    auto& app = *static_cast<WaylandApp*>(data);
    int max_s = 1;
    for (auto& o : app.outputs) if (o.scale > max_s) max_s = o.scale;
    if (max_s > app.scale) {
        app.scale = max_s;
        app.compositor_max_scale = max_s;
        if (app.surface) wl_surface_set_buffer_scale(app.surface, max_s);
        app.applied_buffer_w = 0; app.applied_buffer_h = 0;
        app.dirty = true;
        if (app.egl_ready && !app.frame_pending) wl_commit_frame(app);
    }
}
void wl_output_scale(void* data, wl_output* output, int32_t factor) {
    auto& app = *static_cast<WaylandApp*>(data);
    if (factor < 1) factor = 1;
    for (auto& o : app.outputs) {
        if (o.output == output) { o.scale = factor; break; }
    }
}
void wl_output_name(void*, wl_output*, const char*) {}
void wl_output_description(void*, wl_output*, const char*) {}
const wl_output_listener wl_output_listener_impl = {
    .geometry = wl_output_geometry, .mode = wl_output_mode,
    .done = wl_output_done, .scale = wl_output_scale,
    .name = wl_output_name, .description = wl_output_description,
};

void registry_global(void* data, wl_registry* reg, uint32_t name,
                     const char* iface, uint32_t version) {
    auto& app = *static_cast<WaylandApp*>(data);
    if (std::strcmp(iface, wl_compositor_interface.name) == 0) {
        app.compositor = static_cast<wl_compositor*>(wl_registry_bind(reg, name, &wl_compositor_interface, 6));
    } else if (std::strcmp(iface, xdg_wm_base_interface.name) == 0) {
        app.wm_base = static_cast<xdg_wm_base*>(wl_registry_bind(reg, name, &xdg_wm_base_interface, 1));
        xdg_wm_base_add_listener(app.wm_base, &wm_base_listener_impl, &app);
    } else if (std::strcmp(iface, wl_seat_interface.name) == 0) {
        app.seat = static_cast<wl_seat*>(wl_registry_bind(reg, name, &wl_seat_interface, 1));
        wl_seat_add_listener(app.seat, &seat_listener_impl, &app);
    } else if (std::strcmp(iface, wl_shm_interface.name) == 0) {
        app.shm = static_cast<wl_shm*>(wl_registry_bind(reg, name, &wl_shm_interface, 1));
    } else if (std::strcmp(iface, zxdg_decoration_manager_v1_interface.name) == 0) {
        app.deco_mgr = static_cast<zxdg_decoration_manager_v1*>(wl_registry_bind(reg, name, &zxdg_decoration_manager_v1_interface, 1));
    } else if (std::strcmp(iface, wl_output_interface.name) == 0) {
        // Bind with version 2+ for scale event
        int ver = std::min<int>(version, 4);
        wl_output* output = static_cast<wl_output*>(wl_registry_bind(reg, name, &wl_output_interface, ver));
        app.outputs.push_back({output, 1});
        wl_output_add_listener(output, &wl_output_listener_impl, &app);
    }
}
const wl_registry_listener registry_listener_impl = {
    .global = registry_global, .global_remove = [](void*, wl_registry*, uint32_t) {},
};

bool wl_init_egl(WaylandApp& app) {
    app.egl_display = eglGetDisplay(reinterpret_cast<EGLNativeDisplayType>(app.display));
    if (app.egl_display == EGL_NO_DISPLAY) return false;
    if (!eglInitialize(app.egl_display, nullptr, nullptr)) return false;
    if (!eglBindAPI(EGL_OPENGL_API)) return false;

    const EGLint attrs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_STENCIL_SIZE, 8, EGL_NONE
    };
    EGLint n = 0;
    if (!eglChooseConfig(app.egl_display, attrs, &app.egl_config, 1, &n) || n < 1) return false;

    const EGLint ctx_attrs[] = {
        EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3,
        EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE
    };
    app.egl_context = eglCreateContext(app.egl_display, app.egl_config, EGL_NO_CONTEXT, ctx_attrs);
    if (app.egl_context == EGL_NO_CONTEXT) return false;

    app.egl_window = wl_egl_window_create(app.surface, app.width, app.height);
    app.egl_surface = eglCreateWindowSurface(app.egl_display, app.egl_config,
        reinterpret_cast<EGLNativeWindowType>(app.egl_window), nullptr);
    if (app.egl_surface == EGL_NO_SURFACE) return false;

    if (!eglMakeCurrent(app.egl_display, app.egl_surface, app.egl_surface, app.egl_context)) return false;
    eglSwapInterval(app.egl_display, 0);
    return true;
}

int run_wayland() {
    WaylandApp app;

    app.display = wl_display_connect(nullptr);
    if (!app.display) return -1;

    wl_registry* reg = wl_display_get_registry(app.display);
    wl_registry_add_listener(reg, &registry_listener_impl, &app);
    wl_display_roundtrip(app.display);
    if (!app.compositor || !app.wm_base) { wl_display_disconnect(app.display); return -1; }
    wl_display_roundtrip(app.display);

    std::fprintf(stderr, "Using Wayland backend\n");

    if (app.shm) {
        const char* size_env = std::getenv("XCURSOR_SIZE");
        int cursor_size = size_env ? std::atoi(size_env) : 24;
        if (cursor_size <= 0) cursor_size = 24;
        app.cursor_theme = wl_cursor_theme_load(std::getenv("XCURSOR_THEME"), cursor_size, app.shm);
        app.cursor_surface = wl_compositor_create_surface(app.compositor);
    }

    app.surface  = wl_compositor_create_surface(app.compositor);
    wl_surface_add_listener(app.surface, &surface_listener_impl, &app);

    // Use the best-known output scale as default
    for (auto& oi : app.outputs) {
        if (oi.scale > app.scale) app.scale = oi.scale;
    }
    wl_surface_set_buffer_scale(app.surface, app.scale);
    app.xsurface = xdg_wm_base_get_xdg_surface(app.wm_base, app.surface);
    xdg_surface_add_listener(app.xsurface, &xdg_surface_listener_impl, &app);
    app.toplevel = xdg_surface_get_toplevel(app.xsurface);
    xdg_toplevel_add_listener(app.toplevel, &toplevel_listener_impl, &app);
    xdg_toplevel_set_app_id(app.toplevel, "tt");
    xdg_toplevel_set_title(app.toplevel, "Time Tracker");
    xdg_toplevel_set_min_size(app.toplevel, app.min_width, app.min_height);

    if (app.deco_mgr) {
        app.deco = zxdg_decoration_manager_v1_get_toplevel_decoration(app.deco_mgr, app.toplevel);
        zxdg_toplevel_decoration_v1_add_listener(app.deco, &deco_listener_impl, &app);
        zxdg_toplevel_decoration_v1_set_mode(app.deco, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    }

    wl_surface_commit(app.surface);
    wl_display_roundtrip(app.display);

    if (!wl_init_egl(app)) { std::fprintf(stderr, "EGL init failed\n"); return 1; }
    if (!init_close_button(app)) { std::fprintf(stderr, "close-button shader init failed\n"); return 1; }
    if (!init_compose(app)) { std::fprintf(stderr, "compose shader init failed\n"); return 1; }

    g_app.scale = app.scale;
    g_app.Init();
    app.egl_ready = true;
    wl_commit_frame(app);

    while (app.running) {
        wl_display_dispatch_pending(app.display);
        if (app.dirty && app.egl_ready && !app.frame_pending)
            wl_commit_frame(app);
        wl_display_flush(app.display);

        int timeout_ms = wl_next_timeout_ms();
        if (app.frame_pending) timeout_ms = -1; // block until frame callback

        int fd = wl_display_get_fd(app.display);
        struct pollfd pfd = { fd, POLLIN, 0 };
        int ret = poll(&pfd, 1, timeout_ms);
        if (ret < 0) {
            if (errno != EINTR) break;
            continue;
        }

        if (ret == 0) {
            // Timeout: time-based update is due
            app.dirty = true;
            continue;
        }

        if (pfd.revents & POLLIN) {
            if (wl_display_prepare_read(app.display) == 0) {
                wl_display_read_events(app.display);
            }
        }
    }

    if (app.xkb_st) xkb_state_unref(app.xkb_st);
    if (app.xkb_km) xkb_keymap_unref(app.xkb_km);
    if (app.xkb_ctx) xkb_context_unref(app.xkb_ctx);
    if (app.deco) zxdg_toplevel_decoration_v1_destroy(app.deco);
    if (app.egl_surface != EGL_NO_SURFACE) eglDestroySurface(app.egl_display, app.egl_surface);
    if (app.egl_window) wl_egl_window_destroy(app.egl_window);
    if (app.egl_context != EGL_NO_CONTEXT) eglDestroyContext(app.egl_display, app.egl_context);
    if (app.egl_display != EGL_NO_DISPLAY) eglTerminate(app.egl_display);
    wl_display_disconnect(app.display);
    return 0;
}

#endif // HAVE_WAYLAND

// ============================================================================
// X11 backend
// ============================================================================

#ifdef HAVE_X11

int run_x11() {
    Display* dpy = XOpenDisplay(nullptr);
    if (!dpy) return -1;
    std::fprintf(stderr, "Using X11 backend\n");

    EGLDisplay egl_dpy = eglGetDisplay(reinterpret_cast<EGLNativeDisplayType>(dpy));
    if (egl_dpy == EGL_NO_DISPLAY || !eglInitialize(egl_dpy, nullptr, nullptr)) {
        XCloseDisplay(dpy); return -1;
    }
    if (!eglBindAPI(EGL_OPENGL_API)) { eglTerminate(egl_dpy); XCloseDisplay(dpy); return -1; }

    const EGLint cfg_attrs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE
    };
    EGLConfig egl_cfg; EGLint n = 0;
    if (!eglChooseConfig(egl_dpy, cfg_attrs, &egl_cfg, 1, &n) || n < 1) {
        eglTerminate(egl_dpy); XCloseDisplay(dpy); return 1;
    }

    EGLint visual_id; eglGetConfigAttrib(egl_dpy, egl_cfg, EGL_NATIVE_VISUAL_ID, &visual_id);
    XVisualInfo vi_template{}; vi_template.visualid = visual_id;
    int vi_count = 0;
    XVisualInfo* vi = XGetVisualInfo(dpy, VisualIDMask, &vi_template, &vi_count);
    if (!vi) { eglTerminate(egl_dpy); XCloseDisplay(dpy); return 1; }

    int screen = DefaultScreen(dpy);
    Colormap cmap = XCreateColormap(dpy, RootWindow(dpy, screen), vi->visual, AllocNone);
    XSetWindowAttributes swa{};
    swa.colormap = cmap;
    swa.event_mask = StructureNotifyMask | ExposureMask | ButtonPressMask | ButtonReleaseMask |
                     PointerMotionMask | KeyPressMask | KeyReleaseMask | FocusChangeMask;

    int width = 520, height = 640;
    Window win = XCreateWindow(dpy, RootWindow(dpy, screen), 0, 0, width, height, 0,
                               vi->depth, InputOutput, vi->visual, CWColormap | CWEventMask, &swa);
    XFree(vi);
    XStoreName(dpy, win, "TT");

    // Set WM_CLASS for desktop file matching
    XClassHint classHint{};
    classHint.res_name = (char*)"tt";
    classHint.res_class = (char*)"tt";
    XSetClassHint(dpy, win, &classHint);

    // Min size hints
    XSizeHints hints{};
    hints.flags = PMinSize;
    hints.min_width = 400;
    hints.min_height = 560;
    XSetWMNormalHints(dpy, win, &hints);

    Atom wm_protocols_atom = XInternAtom(dpy, "WM_PROTOCOLS", False);
    Atom wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    Atom wm_sync_request = XInternAtom(dpy, "_NET_WM_SYNC_REQUEST", False);
    Atom wm_sync_counter = XInternAtom(dpy, "_NET_WM_SYNC_REQUEST_COUNTER", False);

    int sync_major = 0, sync_minor = 0;
    bool have_sync = XSyncInitialize(dpy, &sync_major, &sync_minor);

    XSyncCounter basic_counter = 0, extended_counter = 0;
    XSyncValue zero_val, one_val, current_counter, sync_value;
    XSyncIntToValue(&zero_val, 0); XSyncIntToValue(&one_val, 1);
    current_counter = zero_val; sync_value = zero_val;

    if (have_sync) {
        basic_counter = XSyncCreateCounter(dpy, zero_val);
        extended_counter = XSyncCreateCounter(dpy, zero_val);
        XID counters[2] = { basic_counter, extended_counter };
        XChangeProperty(dpy, win, wm_sync_counter, XA_CARDINAL, 32, PropModeReplace,
                        reinterpret_cast<unsigned char*>(counters), 2);
        Atom protocols[2] = { wm_delete, wm_sync_request };
        XSetWMProtocols(dpy, win, protocols, 2);
    } else {
        XSetWMProtocols(dpy, win, &wm_delete, 1);
    }

    XMapWindow(dpy, win);

    const EGLint ctx_attrs[] = {
        EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3,
        EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE
    };
    EGLContext egl_ctx = eglCreateContext(egl_dpy, egl_cfg, EGL_NO_CONTEXT, ctx_attrs);
    EGLSurface egl_srf = eglCreateWindowSurface(egl_dpy, egl_cfg,
        reinterpret_cast<EGLNativeWindowType>(win), nullptr);
    if (egl_ctx == EGL_NO_CONTEXT || egl_srf == EGL_NO_SURFACE) {
        eglTerminate(egl_dpy); XDestroyWindow(dpy, win); XCloseDisplay(dpy); return 1;
    }
    eglMakeCurrent(egl_dpy, egl_srf, egl_srf, egl_ctx);
    eglSwapInterval(egl_dpy, 0);

    g_app.Init();

    auto freeze_counter = [&](XSyncValue target) {
        int overflow;
        if (XSyncValueLessThan(current_counter, target)) current_counter = target;
        if (!(XSyncValueLow32(current_counter) & 1u))
            XSyncValueAdd(&current_counter, current_counter, one_val, &overflow);
        XSyncSetCounter(dpy, extended_counter, current_counter);
        XFlush(dpy);
    };
    auto thaw_counter = [&]() {
        int overflow;
        if (XSyncValueLow32(current_counter) & 1u)
            XSyncValueAdd(&current_counter, current_counter, one_val, &overflow);
        XSyncSetCounter(dpy, extended_counter, current_counter);
        XSyncSetCounter(dpy, basic_counter, current_counter);
        XFlush(dpy);
    };

    const int x_fd = ConnectionNumber(dpy);
    bool running = true;
    bool x11_dirty = true;

    auto drain_events = [&](bool& alive) {
        while (XPending(dpy)) {
            XEvent ev; XNextEvent(dpy, &ev);
            switch (ev.type) {
            case ConfigureNotify:
                width = ev.xconfigure.width; height = ev.xconfigure.height;
                x11_dirty = true;
                break;
            case Expose:
                x11_dirty = true;
                break;
            case ButtonPress:
                g_app.mouseDown = true;
                g_app.OnClick((double)ev.xbutton.x, (double)ev.xbutton.y);
                x11_dirty = true;
                break;
            case ButtonRelease:
                g_app.mouseDown = false;
                x11_dirty = true;
                break;
            case MotionNotify:
                g_app.mx = (double)ev.xmotion.x; g_app.my = (double)ev.xmotion.y;
                x11_dirty = true;
                break;
            case KeyPress: {
                KeySym sym = XLookupKeysym(&ev.xkey, 0);
                uint32_t keycode = 0;
                switch (sym) {
                case XK_BackSpace: keycode = 0x08; break;
                case XK_Return: case XK_KP_Enter: keycode = 0x0D; break;
                case XK_Escape: keycode = 0x1B; break;
                case XK_Tab: keycode = 0x09; break;
                }
                if (keycode) g_app.OnKey(keycode, 0);
                // Character input (decode UTF-8 from XLookupString)
                char buf[32]; int len = XLookupString(&ev.xkey, buf, sizeof(buf), nullptr, nullptr);
                for (int i = 0; i < len; ) {
                    unsigned char c0 = (unsigned char)buf[i];
                    uint32_t cp = 0; int n = 0;
                    if ((c0 & 0x80) == 0) { cp = c0; n = 1; }
                    else if ((c0 & 0xE0) == 0xC0 && i + 1 < len) { cp = (c0 & 0x1F) << 6 | ((unsigned char)buf[i+1] & 0x3F); n = 2; }
                    else if ((c0 & 0xF0) == 0xE0 && i + 2 < len) { cp = (c0 & 0x0F) << 12 | ((unsigned char)buf[i+1] & 0x3F) << 6 | ((unsigned char)buf[i+2] & 0x3F); n = 3; }
                    else if ((c0 & 0xF8) == 0xF0 && i + 3 < len) { cp = (c0 & 0x07) << 18 | ((unsigned char)buf[i+1] & 0x3F) << 12 | ((unsigned char)buf[i+2] & 0x3F) << 6 | ((unsigned char)buf[i+3] & 0x3F); n = 4; }
                    else { i++; continue; }
                    if (cp >= 0x20 && cp != 0x7F) g_app.OnChar(cp);
                    i += n;
                }
                break;
            }
            case FocusOut:
                if (g_app.inputFocused) {
                    g_app.inputFocused = false;
                }
                break;
            case ClientMessage:
                if (ev.xclient.message_type == wm_protocols_atom) {
                    if (static_cast<Atom>(ev.xclient.data.l[0]) == wm_delete) alive = false;
                    else if (have_sync && static_cast<Atom>(ev.xclient.data.l[0]) == wm_sync_request) {
                        XSyncIntsToValue(&sync_value, ev.xclient.data.l[2], ev.xclient.data.l[3]);
                        freeze_counter(sync_value);
                    }
                }
                break;
            }
        }
    };

    while (running) {
        drain_events(running);
        if (!running) break;

        // Determine if we need to render
        bool needs_render = x11_dirty;
        if (!needs_render) {
            // Check if time-based updates are due
            auto now = std::chrono::steady_clock::now();
            if (g_app.activeTask >= 0) {
                auto elapsed = now - g_app.tasks[g_app.activeTask].startTime;
                auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
                if (ms % 1000 < 50) needs_render = true; // near second boundary
            }
            if (g_app.inputFocused) {
                auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
                if (ms % 500 < 50) needs_render = true; // near blink boundary
            }
        }

        if (needs_render) {
            if (have_sync) freeze_counter(current_counter);

            EGLint srf_w = width, srf_h = height;
            eglQuerySurface(egl_dpy, egl_srf, EGL_WIDTH, &srf_w);
            eglQuerySurface(egl_dpy, egl_srf, EGL_HEIGHT, &srf_h);

            g_app.winW = srf_w; g_app.winH = srf_h;
            g_app.bufW = srf_w; g_app.bufH = srf_h;
            g_app.scale = 1;
            g_app.Paint();

            glFinish();
            eglSwapBuffers(egl_dpy, egl_srf);
            if (have_sync) thaw_counter();
            x11_dirty = false;
        }

        // Compute sleep time
        int timeout_ms = -1;
        if (g_app.activeTask >= 0 || g_app.inputFocused) {
            auto now = std::chrono::steady_clock::now();
            auto next = now + std::chrono::hours(24);
            if (g_app.activeTask >= 0) {
                auto elapsed = now - g_app.tasks[g_app.activeTask].startTime;
                auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
                next = std::min(next, now + std::chrono::milliseconds(1000 - (ms % 1000)));
            }
            if (g_app.inputFocused) {
                auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
                next = std::min(next, now + std::chrono::milliseconds(500 - (ms % 500)));
            }
            timeout_ms = (int)std::chrono::duration_cast<std::chrono::milliseconds>(next - now).count();
            timeout_ms = std::max(0, timeout_ms);
        }

        struct pollfd pfd{ x_fd, POLLIN, 0 };
        poll(&pfd, 1, timeout_ms);
    }

    if (have_sync) {
        XSyncDestroyCounter(dpy, basic_counter);
        XSyncDestroyCounter(dpy, extended_counter);
    }
    eglDestroySurface(egl_dpy, egl_srf);
    eglDestroyContext(egl_dpy, egl_ctx);
    eglTerminate(egl_dpy);
    XDestroyWindow(dpy, win);
    XFreeColormap(dpy, cmap);
    XCloseDisplay(dpy);
    return 0;
}

#endif // HAVE_X11

} // namespace

int main() {
#ifdef HAVE_WAYLAND
    if (std::getenv("WAYLAND_DISPLAY")) {
        int rc = run_wayland();
        if (rc >= 0) {
            g_app.Save();
            return rc;
        }
        std::fprintf(stderr, "Wayland session detected but backend failed\n");
        return 1;
    }
#endif
#ifdef HAVE_X11
    {
        int rc = run_x11();
        if (rc >= 0) {
            g_app.Save();
            return rc;
        }
    }
    std::fprintf(stderr, "X11 not available\n");
#endif
    std::fprintf(stderr, "No display server found\n");
    return 1;
}
