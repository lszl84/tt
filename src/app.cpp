#include "app.h"
#include "data.h"
#include <algorithm>
#include <ctime>
#include <cstdio>
#include <cmath>
#include <utility>

namespace {

std::time_t StartOfDay(std::time_t t) {
    std::tm tm = *std::localtime(&t);
    tm.tm_hour = 0; tm.tm_min = 0; tm.tm_sec = 0;
    tm.tm_isdst = -1;
    return std::mktime(&tm);
}

std::time_t AddDays(std::time_t t, int days) {
    std::tm tm = *std::localtime(&t);
    tm.tm_mday += days;
    tm.tm_isdst = -1;
    return std::mktime(&tm);
}

std::time_t StartOfWeekMonday(std::time_t t) {
    std::time_t day = StartOfDay(t);
    std::tm tm = *std::localtime(&day);
    int daysFromMonday = (tm.tm_wday + 6) % 7; // Mon=0, Sun=6
    return AddDays(day, -daysFromMonday);
}

std::pair<std::time_t, std::time_t> GetRangeBounds(SummaryRange r) {
    std::time_t now = std::time(nullptr);
    std::time_t today = StartOfDay(now);
    std::time_t thisWeek = StartOfWeekMonday(now);
    switch (r) {
    case SummaryRange::Today:
        return {today, AddDays(today, 1)};
    case SummaryRange::ThisWeek:
        return {thisWeek, AddDays(thisWeek, 7)};
    case SummaryRange::TwoWeeks:
        return {AddDays(thisWeek, -7), AddDays(thisWeek, 7)};
    case SummaryRange::PrevWeek:
        return {AddDays(thisWeek, -7), thisWeek};
    case SummaryRange::PrevTwoWeeks:
        return {AddDays(thisWeek, -14), thisWeek};
    default:
        return {today, AddDays(today, 1)};
    }
}

const char* RangeLabel(SummaryRange r) {
    switch (r) {
    case SummaryRange::Today:        return "Today";
    case SummaryRange::ThisWeek:     return "This Week";
    case SummaryRange::TwoWeeks:     return "Two Weeks";
    case SummaryRange::PrevWeek:     return "Prev Week";
    case SummaryRange::PrevTwoWeeks: return "Prev Two Weeks";
    default: return "Today";
    }
}

const char* RangeHeader(SummaryRange r) {
    switch (r) {
    case SummaryRange::Today:        return "Today's Summary";
    case SummaryRange::ThisWeek:     return "This Week Summary";
    case SummaryRange::TwoWeeks:     return "Two Weeks Summary";
    case SummaryRange::PrevWeek:     return "Prev Week Summary";
    case SummaryRange::PrevTwoWeeks: return "Prev Two Weeks Summary";
    default: return "Summary";
    }
}

} // namespace

void App::Init() {
    lastFontScale = scale;
    if (!fontManager.Init(16 * scale)) {
        std::fprintf(stderr, "Font init failed\n");
        return;
    }
    if (!renderer.Init()) {
        std::fprintf(stderr, "Renderer init failed\n");
        return;
    }
    glReady = true;

    if (!LoadState(*this)) {
        // Fallback defaults
        tasks.push_back({"Development", false, {}, 0});
        tasks.push_back({"Code Review", false, {}, 0});
        tasks.push_back({"Meetings", false, {}, 0});
        tasks.push_back({"Planning", false, {}, 0});
    }
    if (!tasks.empty() && selectedTask < 0) selectedTask = 0;
}

void App::Save() {
    if (!SaveState(*this)) {
        std::fprintf(stderr, "Failed to save state\n");
    }
}

std::string App::FormatTime(double seconds) const {
    int h = (int)(seconds / 3600);
    int m = (int)((seconds - h * 3600) / 60);
    int s = (int)(seconds - h * 3600 - m * 60);
    char buf[32];
    if (h > 0)
        std::snprintf(buf, sizeof(buf), "%dh %02dm %02ds", h, m, s);
    else if (m > 0)
        std::snprintf(buf, sizeof(buf), "%dm %02ds", m, s);
    else
        std::snprintf(buf, sizeof(buf), "%ds", s);
    return buf;
}

double App::GetTaskTime(int idx) const {
    if (idx < 0 || idx >= (int)tasks.size()) return 0;
    auto [rangeStart, rangeEnd] = GetRangeBounds(summaryRange);
    double total = 0;
    for (const auto& s : sessions) {
        if (s.taskName == tasks[idx].name && s.start >= rangeStart && s.start < rangeEnd)
            total += s.seconds;
    }
    if (tasks[idx].active) {
        std::time_t now = std::time(nullptr);
        if (now >= rangeStart && now < rangeEnd) {
            double fullElapsed = std::chrono::duration<double>(frameNow - tasks[idx].steadyStart).count();
            if (tasks[idx].wallStart >= rangeStart) {
                total += fullElapsed;
            } else {
                // Session began before this range; clip to range start
                double beforeRange = std::difftime(rangeStart, tasks[idx].wallStart);
                total += std::max(0.0, fullElapsed - beforeRange);
            }
        }
    }
    return total;
}

void App::AddTask(const std::string& name) {
    if (name.empty()) return;
    tasks.push_back({name, false, {}, 0});
    if (selectedTask < 0) selectedTask = (int)tasks.size() - 1;
}

void App::ToggleTimer() {
    if (selectedTask < 0 || selectedTask >= (int)tasks.size()) return;

    // If another task is active, stop it first
    if (activeTask >= 0 && activeTask != selectedTask && tasks[activeTask].active) {
        auto now_steady = std::chrono::steady_clock::now();
        auto now_wall = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        double elapsed = std::chrono::duration<double>(now_steady - tasks[activeTask].steadyStart).count();
        sessions.push_back({tasks[activeTask].name, tasks[activeTask].wallStart, now_wall, elapsed});
        tasks[activeTask].active = false;
    }

    if (tasks[selectedTask].active) {
        // Stop
        auto now_steady = std::chrono::steady_clock::now();
        auto now_wall = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        double elapsed = std::chrono::duration<double>(now_steady - tasks[selectedTask].steadyStart).count();
        sessions.push_back({tasks[selectedTask].name, tasks[selectedTask].wallStart, now_wall, elapsed});
        tasks[selectedTask].active = false;
        activeTask = -1;
    } else {
        // Start
        tasks[selectedTask].active = true;
        tasks[selectedTask].steadyStart = std::chrono::steady_clock::now();
        tasks[selectedTask].wallStart = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        activeTask = selectedTask;
    }
}

void App::Paint() {
    if (!glReady) return;

    // Snapshot time once per frame so every timer display is perfectly in sync
    frameNow = std::chrono::steady_clock::now();

    // Auto-save every 30 seconds
    if (std::chrono::duration_cast<std::chrono::seconds>(frameNow - lastSaveTime).count() >= 30) {
        Save();
        lastSaveTime = frameNow;
    }

    CheckFontScale();
    renderer.BeginFrame(bufW, bufH, winW, winH, fontManager);

    float W = (float)winW;
    float H = (float)winH;
    float P = PADDING;

    // ---- Background ----
    renderer.DrawRect(0, 0, W, H, BG);

    // Title is drawn in the CSD title bar, not here
    float titleBottom = P + 4;

    // ---- Input + Add button row ----
    float inputRowY = titleBottom;
    float inputH = INPUT_HEIGHT;
    float btnW = 80.0f;
    float inputW = W - 2 * P - btnW - 8;

    // Input field background
    renderer.DrawRoundedRect(P, inputRowY, inputW, inputH, 6.0f, INPUT_BG);
    // Border
    Color borderCol = inputFocused ? ACCENT : INPUT_BORDER;
    // Draw border as 4 thin rects (simple approach)
    float bw = 1.5f;
    renderer.DrawRect(P, inputRowY, inputW, bw, borderCol);                   // top
    renderer.DrawRect(P, inputRowY + inputH - bw, inputW, bw, borderCol);     // bottom
    renderer.DrawRect(P, inputRowY, bw, inputH, borderCol);                   // left
    renderer.DrawRect(P + inputW - bw, inputRowY, bw, inputH, borderCol);     // right

    // Input text
    {
        float textY = inputRowY + (inputH - LineH()) / 2.0f;
        std::string displayText = inputText;
        if (displayText.empty()) {
            renderer.DrawText("New task name...", P + 10, textY, TEXT_DIM);
        } else {
            renderer.DrawText(displayText, P + 10, textY, TEXT_COLOR);
        }
        // Cursor blink
        if (inputFocused) {
            float cursorX = P + 10 + renderer.MeasureText(displayText);
            float blink = std::fmod(std::chrono::duration<float>(frameNow.time_since_epoch()).count(), 1.0f);
            if (blink < 0.5f) {
                renderer.DrawRect(cursorX, textY + 2, 2, LineH() - 4, TEXT_COLOR);
            }
        }
    }

    // Add button
    {
        float btnX = P + inputW + 8;
        renderer.DrawRoundedRect(btnX, inputRowY, btnW, inputH, 6.0f, ACCENT);
        float textW = renderer.MeasureText("+ Add");
        float textY = inputRowY + (inputH - LineH()) / 2.0f;
        renderer.DrawText("+ Add", btnX + (btnW - textW) / 2, textY, Color(1,1,1,1));
    }

    // ---- Task List ----
    float listY = inputRowY + inputH + P;

    // Animate summary expansion
    float targetAnim = summaryExpanded ? 1.0f : 0.0f;
    summaryExpandAnim += (targetAnim - summaryExpandAnim) * 0.15f;
    if (std::abs(targetAnim - summaryExpandAnim) < 0.001f) summaryExpandAnim = targetAnim;

    float collapsedListH = H - listY - SUMMARY_HEIGHT - P - BUTTON_HEIGHT - 8;
    float expandedListH = ROW_HEIGHT * 2 + 8; // show ~2 rows when expanded
    float listH = collapsedListH + (expandedListH - collapsedListH) * summaryExpandAnim;
    if (listH < expandedListH) listH = expandedListH;

    // Task list background panel
    renderer.DrawRoundedRect(P, listY, W - 2*P, listH, CORNER_RADIUS, PANEL_BG);

    // Clip to list area
    renderer.PushClip(P + 2, listY + 2, W - 2*P - 4, listH - 4);

    float rowY = listY + 4;
    float rowW = W - 2*P - 8;
    for (int i = 0; i < (int)tasks.size(); i++) {
        if (rowY + ROW_HEIGHT > listY + listH) break;

        float rowX = P + 4;
        Color rowBg = TASK_BG;
        if (i == selectedTask) rowBg = TASK_SELECTED;
        if (tasks[i].active) rowBg = TASK_ACTIVE;

        renderer.DrawRoundedRect(rowX, rowY, rowW, ROW_HEIGHT - 4, 6.0f, rowBg);

        // Task name
        float textY = rowY + (ROW_HEIGHT - 4 - LineH()) / 2.0f;
        renderer.DrawText(tasks[i].name, rowX + 12, textY, TEXT_COLOR);

        // Time
        std::string timeStr = FormatTime(GetTaskTime(i));
        float timeW = renderer.MeasureText(timeStr);
        renderer.DrawText(timeStr, rowX + rowW - timeW - 12, textY,
                          tasks[i].active ? GREEN : TEXT_DIM);

        rowY += ROW_HEIGHT;
    }
    renderer.PopClip();

    // ---- Start/Stop Button ----
    float btnAreaY = listY + listH + 8;
    float startBtnW = 160.0f;
    float startBtnX = P;

    bool canToggle = (selectedTask >= 0 && selectedTask < (int)tasks.size());
    bool isRunning = canToggle && tasks[selectedTask].active;
    Color startBtnColor = isRunning ? RED : GREEN;
    std::string startLabel = isRunning ? "Stop" : "Start";

    renderer.DrawRoundedRect(startBtnX, btnAreaY, startBtnW, BUTTON_HEIGHT, 6.0f, startBtnColor);
    {
        float textW = renderer.MeasureText(startLabel);
        float textY = btnAreaY + (BUTTON_HEIGHT - LineH()) / 2.0f;
        float iconW = isRunning ? 12.0f : 14.0f;
        float iconH = 12.0f;
        float spacing = 8.0f;
        float totalW = iconW + spacing + textW;
        float contentX = startBtnX + (startBtnW - totalW) / 2.0f;

        if (isRunning) {
            // Stop: white square
            float ix = contentX + (iconW - 10.0f) / 2.0f;
            float iy = btnAreaY + (BUTTON_HEIGHT - 10.0f) / 2.0f;
            renderer.DrawRect(ix, iy, 10.0f, 10.0f, Color(1,1,1,1));
        } else {
            // Play: white triangle pointing right
            float ix = contentX;
            float iy = btnAreaY + (BUTTON_HEIGHT - iconH) / 2.0f;
            renderer.DrawTriangle(ix, iy, ix, iy + iconH, ix + iconW, iy + iconH / 2.0f, Color(1,1,1,1));
        }

        renderer.DrawText(startLabel, contentX + iconW + spacing, textY, Color(1,1,1,1));
    }

    // ---- Daily Summary Panel ----
    float summaryY = btnAreaY + BUTTON_HEIGHT + P;
    float summaryH = H - summaryY - P;
    if (summaryH < 40) summaryH = 40;

    renderer.DrawRoundedRect(P, summaryY, W - 2*P, summaryH, CORNER_RADIUS, SUMMARY_BG);

    // Header with hover highlight
    float headerPad = 10.0f;
    float headerTextY = summaryY + headerPad;
    float headerH = LineH() + headerPad * 2;
    summaryHeaderX = P;
    summaryHeaderY = summaryY;
    summaryHeaderW = W - 2*P;
    summaryHeaderH = headerH;

    summaryHeaderHovered = (mx >= summaryHeaderX && mx <= summaryHeaderX + summaryHeaderW &&
                            my >= summaryHeaderY && my <= summaryHeaderY + summaryHeaderH);

    if (summaryHeaderHovered) {
        renderer.DrawRoundedRect(summaryHeaderX + 2, summaryHeaderY + 2,
                                  summaryHeaderW - 4, headerH - 4, 4.0f,
                                  Color(0.18f, 0.20f, 0.26f, 1.0f));
    }

    std::string headerText = RangeHeader(summaryRange);
    renderer.DrawText(headerText, P + 14, headerTextY, ACCENT);

    if (summaryHeaderHovered) {
        std::string hint = summaryExpanded ? "(click to collapse)" : "(click to expand)";
        float titleW = renderer.MeasureText(headerText);
        float hintX = P + 14 + titleW + 12;
        float titleCenter = headerTextY + LineH() * 0.5f;
        int oldSize = fontManager.CurrentSize();
        fontManager.SetSize(12 * scale);
        float hintY = titleCenter - (fontManager.LineHeight() / scale) * 0.5f;
        renderer.DrawText(hint, hintX, hintY, Color(0.35f, 0.38f, 0.45f, 1.0f));
        fontManager.SetSize(oldSize);
    }

    float sumTextY = summaryY + headerH;

    // Compute total
    double totalSec = 0;
    for (int i = 0; i < (int)tasks.size(); i++)
        totalSec += GetTaskTime(i);

    renderer.DrawText("Total: " + FormatTime(totalSec), P + 14, sumTextY, TEXT_COLOR);
    sumTextY += LineH() + 2;

    // Reserve space at the bottom for the range selector (grows with expand animation)
    const float selectorBtnSize = 28.0f;
    const float selectorPad = 10.0f;
    const float selectorFullH = selectorBtnSize + selectorPad * 2;
    float selectorReserve = selectorFullH * summaryExpandAnim;
    float breakdownBottom = summaryY + summaryH - 4 - selectorReserve;

    // Per-task breakdown
    renderer.PushClip(P + 2, sumTextY, W - 2*P - 4, std::max(0.0f, breakdownBottom - sumTextY));
    int shown = 0;
    for (int i = 0; i < (int)tasks.size(); i++) {
        double t = GetTaskTime(i);
        float textY2 = sumTextY + (LineH() + 4) * shown;
        if (textY2 + LineH() > breakdownBottom) break;

        // Name
        renderer.DrawText(tasks[i].name, P + 14, textY2, TEXT_DIM);
        // Time
        std::string ts = FormatTime(t);
        float tw = renderer.MeasureText(ts);
        renderer.DrawText(ts, W - P - 14 - tw, textY2, TEXT_DIM);

        shown++;
    }
    renderer.PopClip();

    // Range selector (only while expanded)
    if (summaryExpandAnim > 0.01f) {
        float selY = summaryY + summaryH - selectorBtnSize - selectorPad;
        float btnW = selectorBtnSize;
        float btnH = selectorBtnSize;

        // Left button
        rangeLeftX = P + selectorPad;
        rangeLeftY = selY;
        rangeLeftW = btnW;
        rangeLeftH = btnH;
        bool leftHover = (mx >= rangeLeftX && mx <= rangeLeftX + rangeLeftW &&
                          my >= rangeLeftY && my <= rangeLeftY + rangeLeftH);
        Color leftBg = leftHover ? Color(0.22f, 0.24f, 0.30f, 1.0f)
                                 : Color(0.17f, 0.18f, 0.22f, 1.0f);
        renderer.DrawRoundedRect(rangeLeftX, rangeLeftY, rangeLeftW, rangeLeftH, 6.0f, leftBg);
        {
            float tPad = 9.0f;
            float tx_point = rangeLeftX + tPad;
            float tx_base  = rangeLeftX + rangeLeftW - tPad;
            float ty_top   = rangeLeftY + tPad;
            float ty_bot   = rangeLeftY + rangeLeftH - tPad;
            float ty_mid   = rangeLeftY + rangeLeftH * 0.5f;
            renderer.DrawTriangle(tx_point, ty_mid, tx_base, ty_top, tx_base, ty_bot, TEXT_COLOR);
        }

        // Right button
        rangeRightX = W - P - selectorPad - btnW;
        rangeRightY = selY;
        rangeRightW = btnW;
        rangeRightH = btnH;
        bool rightHover = (mx >= rangeRightX && mx <= rangeRightX + rangeRightW &&
                           my >= rangeRightY && my <= rangeRightY + rangeRightH);
        Color rightBg = rightHover ? Color(0.22f, 0.24f, 0.30f, 1.0f)
                                   : Color(0.17f, 0.18f, 0.22f, 1.0f);
        renderer.DrawRoundedRect(rangeRightX, rangeRightY, rangeRightW, rangeRightH, 6.0f, rightBg);
        {
            float tPad = 9.0f;
            float tx_base  = rangeRightX + tPad;
            float tx_point = rangeRightX + rangeRightW - tPad;
            float ty_top   = rangeRightY + tPad;
            float ty_bot   = rangeRightY + rangeRightH - tPad;
            float ty_mid   = rangeRightY + rangeRightH * 0.5f;
            renderer.DrawTriangle(tx_base, ty_top, tx_base, ty_bot, tx_point, ty_mid, TEXT_COLOR);
        }

        // Label centered between buttons
        std::string label = RangeLabel(summaryRange);
        float labelW = renderer.MeasureText(label);
        float midX = P + (W - 2 * P) * 0.5f;
        float labelY = selY + (btnH - LineH()) * 0.5f;
        renderer.DrawText(label, midX - labelW * 0.5f, labelY, TEXT_COLOR);
    } else {
        // Selector not visible — reset hitboxes so clicks don't accidentally trigger
        rangeLeftW = rangeLeftH = 0;
        rangeRightW = rangeRightH = 0;
    }

    renderer.EndFrame();
}

void App::OnClick(double x, double y) {
    float W = (float)winW;
    float H = (float)winH;
    float P = PADDING;

    float titleBottom = P + 4;
    float inputRowY = titleBottom;
    float inputH = INPUT_HEIGHT;
    float btnW = 80.0f;
    float inputW = W - 2 * P - btnW - 8;

    // Click on input field?
    if (x >= P && x <= P + inputW && y >= inputRowY && y <= inputRowY + inputH) {
        inputFocused = true;
        return;
    }

    // Click on Add button?
    float btnX = P + inputW + 8;
    if (x >= btnX && x <= btnX + btnW && y >= inputRowY && y <= inputRowY + inputH) {
        AddTask(inputText);
        inputText.clear();
        return;
    }

    // Click on task list
    float listY = inputRowY + inputH + P;
    float collapsedListH = H - listY - SUMMARY_HEIGHT - P - BUTTON_HEIGHT - 8;
    float expandedListH = ROW_HEIGHT * 2 + 8;
    float listH = collapsedListH + (expandedListH - collapsedListH) * summaryExpandAnim;
    if (listH < expandedListH) listH = expandedListH;

    if (x >= P && x <= W - P && y >= listY && y <= listY + listH) {
        float rowY = listY + 4;
        for (int i = 0; i < (int)tasks.size(); i++) {
            if (rowY + ROW_HEIGHT > listY + listH) break;
            if (y >= rowY && y < rowY + ROW_HEIGHT - 4) {
                selectedTask = i;
                return;
            }
            rowY += ROW_HEIGHT;
        }
    }

    // Click on Start/Stop button
    float btnAreaY = listY + listH + 8;
    float startBtnW = 160.0f;
    if (x >= P && x <= P + startBtnW && y >= btnAreaY && y <= btnAreaY + BUTTON_HEIGHT) {
        ToggleTimer();
        return;
    }

    // Click on range selector buttons (only while expanded)
    if (summaryExpandAnim > 0.5f) {
        int count = static_cast<int>(SummaryRange::Count);
        if (rangeLeftW > 0 && rangeLeftH > 0 &&
            x >= rangeLeftX && x <= rangeLeftX + rangeLeftW &&
            y >= rangeLeftY && y <= rangeLeftY + rangeLeftH) {
            int cur = static_cast<int>(summaryRange);
            summaryRange = static_cast<SummaryRange>((cur - 1 + count) % count);
            return;
        }
        if (rangeRightW > 0 && rangeRightH > 0 &&
            x >= rangeRightX && x <= rangeRightX + rangeRightW &&
            y >= rangeRightY && y <= rangeRightY + rangeRightH) {
            int cur = static_cast<int>(summaryRange);
            summaryRange = static_cast<SummaryRange>((cur + 1) % count);
            return;
        }
    }

    // Click on summary header?
    if (x >= summaryHeaderX && x <= summaryHeaderX + summaryHeaderW &&
        y >= summaryHeaderY && y <= summaryHeaderY + summaryHeaderH) {
        summaryExpanded = !summaryExpanded;
        return;
    }

    // Click elsewhere: unfocus input
    inputFocused = false;
}

void App::OnKey(uint32_t key, uint32_t mods) {
    if (!inputFocused) {
        // Global shortcuts when input is not focused
        switch (key) {
        case 0xFF52: // Up
            if (!tasks.empty()) {
                selectedTask--;
                if (selectedTask < 0) selectedTask = (int)tasks.size() - 1;
            }
            return;
        case 0xFF54: // Down
            if (!tasks.empty()) {
                selectedTask++;
                if (selectedTask >= (int)tasks.size()) selectedTask = 0;
            }
            return;
        case 0x20: // Space
            ToggleTimer();
            return;
        }
        return;
    }

    // Input field keys
    if (key == 0x08 || key == 0xFF08) { // Backspace
        if (!inputText.empty()) {
            // Remove last UTF-8 codepoint
            size_t pos = inputText.size();
            while (pos > 0 && (inputText[pos-1] & 0xC0) == 0x80) pos--;
            if (pos > 0) pos--;
            inputText.erase(pos);
        }
    } else if (key == 0x0D || key == 0xFF0D) { // Enter/Return
        AddTask(inputText);
        inputText.clear();
    } else if (key == 0x1B || key == 0xFF1B) { // Escape
        inputFocused = false;
    }
}

void App::OnChar(uint32_t codepoint) {
    if (!inputFocused) return;
    if (codepoint < 0x20 && codepoint != 0x0D) return;
    if (codepoint == 0x0D) return; // handled in OnKey

    char buf[4];
    int len = 0;
    if (codepoint < 0x80) {
        buf[0] = (char)codepoint; len = 1;
    } else if (codepoint < 0x800) {
        buf[0] = (char)(0xC0 | (codepoint >> 6));
        buf[1] = (char)(0x80 | (codepoint & 0x3F)); len = 2;
    } else if (codepoint < 0x10000) {
        buf[0] = (char)(0xE0 | (codepoint >> 12));
        buf[1] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
        buf[2] = (char)(0x80 | (codepoint & 0x3F)); len = 3;
    } else {
        buf[0] = (char)(0xF0 | (codepoint >> 18));
        buf[1] = (char)(0x80 | ((codepoint >> 12) & 0x3F));
        buf[2] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
        buf[3] = (char)(0x80 | (codepoint & 0x3F)); len = 4;
    }
    inputText.append(buf, len);
}
