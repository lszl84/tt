#pragma once
#include "renderer.h"
#include "font.h"
#include <string>
#include <vector>
#include <chrono>
#include <ctime>

// ============================================================================
// Time Tracker Data
// ============================================================================

struct Task {
    std::string name;
    bool active = false;
    std::chrono::steady_clock::time_point steadyStart;
    std::time_t wallStart = 0;
};

struct TimeSession {
    std::string taskName;
    std::time_t start = 0;
    std::time_t end = 0;
    double seconds = 0;
};

enum class SummaryRange : int {
    Today = 0,
    ThisWeek,
    TwoWeeks,
    PrevWeek,
    PrevTwoWeeks,
    Count
};

struct App {
    // Fonts & rendering
    FontManager fontManager;
    Renderer renderer;
    bool glReady = false;

    // Window dimensions (in logical pixels, before scale)
    int winW = 720, winH = 560;
    int bufW = 720, bufH = 560;
    int scale = 1;

    // Mouse state (in logical coords, relative to content area)
    double mx = 0, my = 0;
    bool mouseDown = false;

    // Text input for new task name
    std::string inputText;
    bool inputFocused = true;

    // Tasks
    std::vector<Task> tasks;
    int selectedTask = -1; // which task is selected in the list
    int activeTask = -1;   // which task is currently tracking

    // Session log (completed tracking sessions with wall-clock dates)
    std::vector<TimeSession> sessions;

    // Auto-save
    std::chrono::steady_clock::time_point lastSaveTime = std::chrono::steady_clock::now();

    // Frame-consistent time sampling (so task times and totals never drift mid-frame)
    std::chrono::steady_clock::time_point frameNow = std::chrono::steady_clock::now();

    // Summary panel expand
    bool summaryExpanded = false;
    float summaryExpandAnim = 0.0f;
    bool summaryHeaderHovered = false;
    float summaryHeaderX = 0, summaryHeaderY = 0, summaryHeaderW = 0, summaryHeaderH = 0;

    // Summary range selector (visible when expanded)
    SummaryRange summaryRange = SummaryRange::Today;
    float rangeLeftX = 0, rangeLeftY = 0, rangeLeftW = 0, rangeLeftH = 0;
    float rangeRightX = 0, rangeRightY = 0, rangeRightW = 0, rangeRightH = 0;

    // UI layout constants
    static constexpr float PADDING = 16.0f;
    static constexpr float ROW_HEIGHT = 40.0f;
    static constexpr float BUTTON_HEIGHT = 36.0f;
    static constexpr float INPUT_HEIGHT = 36.0f;
    static constexpr float CORNER_RADIUS = 8.0f;
    static constexpr float SUMMARY_HEIGHT = 260.0f;

    // Colors
    static constexpr Color BG           = {0.11f, 0.12f, 0.15f, 1.0f};
    static constexpr Color PANEL_BG     = {0.15f, 0.16f, 0.20f, 1.0f};
    static constexpr Color TASK_BG      = {0.18f, 0.19f, 0.24f, 1.0f};
    static constexpr Color TASK_SELECTED= {0.28f, 0.32f, 0.48f, 1.0f};
    static constexpr Color TASK_ACTIVE  = {0.14f, 0.24f, 0.20f, 1.0f};
    static constexpr Color TEXT_COLOR   = {0.85f, 0.87f, 0.90f, 1.0f};
    static constexpr Color TEXT_DIM     = {0.55f, 0.58f, 0.63f, 1.0f};
    static constexpr Color ACCENT       = {0.30f, 0.65f, 0.95f, 1.0f};
    static constexpr Color GREEN        = {0.30f, 0.78f, 0.50f, 1.0f};
    static constexpr Color RED          = {0.85f, 0.35f, 0.35f, 1.0f};
    static constexpr Color INPUT_BG     = {0.13f, 0.14f, 0.17f, 1.0f};
    static constexpr Color INPUT_BORDER = {0.25f, 0.27f, 0.32f, 1.0f};
    static constexpr Color DIVIDER      = {0.22f, 0.24f, 0.28f, 1.0f};
    static constexpr Color SUMMARY_BG   = {0.13f, 0.14f, 0.18f, 1.0f};

    // Methods
    void Init();
    void Paint();
    void OnClick(double x, double y);
    void OnKey(uint32_t key, uint32_t mods);
    void OnChar(uint32_t codepoint);
    void ToggleTimer();
    void AddTask(const std::string& name);
    double GetTaskTime(int idx) const;
    std::string FormatTime(double seconds) const;
    void Save();

    bool IsAnimating() const {
        float target = summaryExpanded ? 1.0f : 0.0f;
        return std::abs(target - summaryExpandAnim) >= 0.001f;
    }

    // Logical-pixel font metrics (divide physical by scale)
    float LineH() const { return fontManager.LineHeight() / scale; }
    float AscH() const { return fontManager.Ascent() / scale; }

    // Re-initialize font when scale changes
    int lastFontScale = 0;
    void CheckFontScale() {
        if (lastFontScale != scale) {
            lastFontScale = scale;
            fontManager.Init(18 * scale);
        }
    }
};

// ============================================================================
// Platform interface — implemented in main.cpp per backend
// ============================================================================

// Called by platform code:
//   App::Init(), App::Paint(), App::OnClick(), etc.
// Platform provides the EGL/GL context and calls these.
