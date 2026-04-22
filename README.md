# tt — Time Tracker

A minimal, native time-tracking application with custom OpenGL rendering, client-side decorations, and HiDPI support.

## Features

- **Native Wayland backend** with client-side decorations (CSD): custom title bar, close button, resize handles, shadow, and rounded corners
- **X11 fallback backend** with `_NET_WM_SYNC_REQUEST` for smooth resizing
- **HiDPI support** — queries compositor output scale (Wayland) and renders at full resolution
- **Custom OpenGL 3.3 renderer** — batched quads, rounded-rect SDF, font atlas with HarfBuzz shaping
- **Time tracking** — add tasks, start/stop timer, daily summary with per-task breakdown

## Dependencies

| Library | Purpose |
|---------|---------|
| EGL / OpenGL 3.3 | Rendering |
| FreeType + HarfBuzz | Font rendering & text shaping |
| Fontconfig | Font discovery |
| wayland-client, wayland-egl, wayland-cursor | Wayland backend |
| xkbcommon | Keyboard input (Wayland) |
| X11, XSync | X11 backend (fallback) |

## Build

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

The Wayland backend is auto-detected. The X11 backend is used as a fallback if `WAYLAND_DISPLAY` is not set.

## Usage

```bash
./tt
```

- Type a task name and press **Enter** or click **+ Add**
- Click a task to select it
- Click **▶ Start** to begin tracking (click **■ Stop** to stop)
- The daily summary panel shows time breakdowns

## Architecture

```
src/
  main.cpp      — Platform layer (Wayland + X11 backends, EGL, event loop)
  app.h/cpp     — Application logic, layout, input handling
  renderer.h/cpp — OpenGL batch renderer (quads, rounded rects, text)
  font.h/cpp    — Font management (FreeType + HarfBuzz, glyph atlas)
```

The platform layer creates the window, manages the GL context, and drives the frame loop. The app layer is backend-agnostic — it only sees logical-pixel coordinates and calls `Paint()` / `OnClick()` / `OnKey()`.

## License

GPLv3
