#include "data.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <random>
#include <sstream>

namespace {

std::filesystem::path DefaultDataDir() {
#if defined(__APPLE__)
    if (const char* home = std::getenv("HOME"); home && home[0]) {
        return std::filesystem::path(home) / "Library" / "Application Support" / "tt";
    }
#else
    if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg && xdg[0]) {
        return std::filesystem::path(xdg) / "tt";
    }
    if (const char* home = std::getenv("HOME"); home && home[0]) {
        return std::filesystem::path(home) / ".local" / "share" / "tt";
    }
#endif
    return std::filesystem::current_path() / ".tt";
}

std::string FormatISO(std::time_t t) {
    if (t == 0) return {};
    std::tm tm = *std::localtime(&t);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
    return buf;
}

std::time_t ParseISO(const std::string& s) {
    if (s.empty()) return 0;
    std::tm tm = {};
    std::istringstream ss(s);
    ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
    tm.tm_isdst = -1;
    return std::mktime(&tm);
}

std::string ReadFile(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool WriteFileAtomic(const std::filesystem::path& path, const std::string& contents) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    auto tmp = path;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary);
        if (!f) return false;
        f.write(contents.data(), (std::streamsize)contents.size());
        if (!f.good()) return false;
    }
    std::filesystem::rename(tmp, path, ec);
    return !ec;
}

// Set runtime "active" fields on tasks based on which sessions are still open.
// If multiple open sessions exist for one task (shouldn't happen) the earliest
// wins.
void ApplyActiveFromSessions(TTState& state) {
    for (auto& t : state.tasks) {
        t.active = false;
        t.activeSessionId.clear();
        t.wallStart = 0;
    }
    state.activeTask = -1;

    for (const auto& s : state.sessions) {
        if (s.end != 0) continue;
        int idx = FindTaskById(state, s.taskId);
        if (idx < 0) continue;
        Task& tt = state.tasks[idx];
        if (tt.active && tt.wallStart <= s.start) continue;
        tt.active = true;
        tt.activeSessionId = s.id;
        tt.wallStart = s.start;
        std::time_t now_t = std::time(nullptr);
        double elapsed = std::max(0.0, std::difftime(now_t, s.start));
        auto offset = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(elapsed));
        tt.steadyStart = std::chrono::steady_clock::now() - offset;
        state.activeTask = idx;
    }
}

std::time_t StartOfDay(std::time_t t) {
    std::tm tm = *std::localtime(&t);
    tm.tm_hour = tm.tm_min = tm.tm_sec = 0;
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
    int daysFromMonday = (tm.tm_wday + 6) % 7;
    return AddDays(day, -daysFromMonday);
}

}  // namespace

std::filesystem::path GetDataPath() {
    std::filesystem::path dir;
    if (const char* env = std::getenv("TT_DATA_DIR"); env && env[0]) {
        dir = env;
    } else {
        dir = DefaultDataDir();
    }
    return dir / "state.json";
}

std::string GenerateUuid() {
    static std::mutex mu;
    static std::mt19937_64 rng{std::random_device{}()};
    std::lock_guard<std::mutex> lock(mu);
    uint64_t a = rng();
    uint64_t b = rng();
    a = (a & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;
    b = (b & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;
    char buf[37];
    std::snprintf(buf, sizeof(buf),
                  "%08x-%04x-%04x-%04x-%012llx",
                  (unsigned)(a >> 32),
                  (unsigned)((a >> 16) & 0xFFFF),
                  (unsigned)(a & 0xFFFF),
                  (unsigned)(b >> 48),
                  (unsigned long long)(b & 0xFFFFFFFFFFFFULL));
    return std::string(buf);
}

std::string FormatDuration(double seconds) {
    int total = (int)seconds;
    int h = total / 3600;
    int m = (total % 3600) / 60;
    int s = total % 60;
    char buf[32];
    if (h > 0)
        std::snprintf(buf, sizeof(buf), "%dh %02dm %02ds", h, m, s);
    else if (m > 0)
        std::snprintf(buf, sizeof(buf), "%dm %02ds", m, s);
    else
        std::snprintf(buf, sizeof(buf), "%ds", s);
    return buf;
}

std::pair<std::time_t, std::time_t> GetRangeBounds(SummaryRange r) {
    std::time_t now = std::time(nullptr);
    std::time_t today = StartOfDay(now);
    std::time_t thisWeek = StartOfWeekMonday(now);
    switch (r) {
    case SummaryRange::Today:        return {today, AddDays(today, 1)};
    case SummaryRange::ThisWeek:     return {thisWeek, AddDays(thisWeek, 7)};
    case SummaryRange::TwoWeeks:     return {AddDays(thisWeek, -7), AddDays(thisWeek, 7)};
    case SummaryRange::PrevWeek:     return {AddDays(thisWeek, -7), thisWeek};
    case SummaryRange::PrevTwoWeeks: return {AddDays(thisWeek, -14), thisWeek};
    default:                         return {today, AddDays(today, 1)};
    }
}

const char* RangeLabel(SummaryRange r) {
    switch (r) {
    case SummaryRange::Today:        return "Today";
    case SummaryRange::ThisWeek:     return "This Week";
    case SummaryRange::TwoWeeks:     return "Two Weeks";
    case SummaryRange::PrevWeek:     return "Prev Week";
    case SummaryRange::PrevTwoWeeks: return "Prev Two Weeks";
    default:                         return "Today";
    }
}

const char* RangeHeader(SummaryRange r) {
    switch (r) {
    case SummaryRange::Today:        return "Today's Summary";
    case SummaryRange::ThisWeek:     return "This Week Summary";
    case SummaryRange::TwoWeeks:     return "Two Weeks Summary";
    case SummaryRange::PrevWeek:     return "Prev Week Summary";
    case SummaryRange::PrevTwoWeeks: return "Prev Two Weeks Summary";
    default:                         return "Summary";
    }
}

int FindTaskById(const TTState& state, const std::string& id) {
    for (size_t i = 0; i < state.tasks.size(); ++i)
        if (state.tasks[i].id == id) return (int)i;
    return -1;
}

double GetTaskTime(const TTState& state, int idx, SummaryRange range,
                   std::chrono::steady_clock::time_point now) {
    if (idx < 0 || idx >= (int)state.tasks.size()) return 0;
    auto [rangeStart, rangeEnd] = GetRangeBounds(range);
    double total = 0;
    const std::string& tid = state.tasks[idx].id;
    for (const auto& s : state.sessions) {
        if (s.end == 0) continue;  // active sessions handled below
        if (s.taskId == tid && s.start >= rangeStart && s.start < rangeEnd)
            total += s.seconds;
    }
    const Task& t = state.tasks[idx];
    if (t.active) {
        std::time_t wnow = std::time(nullptr);
        if (wnow >= rangeStart && wnow < rangeEnd) {
            double fullElapsed = std::chrono::duration<double>(now - t.steadyStart).count();
            if (t.wallStart >= rangeStart) {
                total += fullElapsed;
            } else {
                double beforeRange = std::difftime(rangeStart, t.wallStart);
                total += std::max(0.0, fullElapsed - beforeRange);
            }
        }
    }
    return total;
}

void StartTask(TTState& state, int idx) {
    if (idx < 0 || idx >= (int)state.tasks.size()) return;
    Task& t = state.tasks[idx];
    if (t.active) return;

    std::time_t now_wall = std::time(nullptr);
    TimeSession s;
    s.id = GenerateUuid();
    s.taskId = t.id;
    s.start = now_wall;
    s.end = 0;
    s.seconds = 0;
    state.sessions.push_back(std::move(s));

    t.active = true;
    t.steadyStart = std::chrono::steady_clock::now();
    t.wallStart = now_wall;
    t.activeSessionId = state.sessions.back().id;
    state.activeTask = idx;
}

void StopTask(TTState& state, int idx) {
    if (idx < 0 || idx >= (int)state.tasks.size()) return;
    Task& t = state.tasks[idx];
    if (!t.active) return;

    std::time_t now_wall = std::time(nullptr);
    bool found = false;
    if (!t.activeSessionId.empty()) {
        for (auto& s : state.sessions) {
            if (s.id == t.activeSessionId) {
                s.end = now_wall;
                s.seconds = std::max(0.0, std::difftime(s.end, s.start));
                found = true;
                break;
            }
        }
    }
    if (!found) {
        // Defensive: synthesize a session from the task's wallStart so time
        // isn't lost if activeSessionId was stripped somehow.
        TimeSession s;
        s.id = GenerateUuid();
        s.taskId = t.id;
        s.start = t.wallStart != 0 ? t.wallStart : now_wall;
        s.end = now_wall;
        s.seconds = std::max(0.0, std::difftime(s.end, s.start));
        state.sessions.push_back(std::move(s));
    }
    t.active = false;
    t.activeSessionId.clear();
    t.wallStart = 0;
    if (state.activeTask == idx) state.activeTask = -1;
}

bool LoadState(TTState& state) {
    auto path = GetDataPath();
    std::string text = ReadFile(path);
    if (text.empty()) return false;

    try {
        auto j = nlohmann::json::parse(text);
        if (j.value("version", 0) != 3) return false;

        state.tasks.clear();
        state.sessions.clear();
        for (const auto& t : j.value("tasks", nlohmann::json::array())) {
            Task task;
            task.id = t.value("id", std::string{});
            task.name = t.value("name", std::string("Untitled"));
            if (task.id.empty()) task.id = GenerateUuid();
            state.tasks.push_back(std::move(task));
        }
        for (const auto& s : j.value("sessions", nlohmann::json::array())) {
            TimeSession sess;
            sess.id = s.value("id", std::string{});
            sess.taskId = s.value("task_id", std::string{});
            std::string startStr = s.value("start", std::string{});
            std::string endStr;
            if (s.contains("end") && !s["end"].is_null())
                endStr = s["end"].get<std::string>();
            if (sess.id.empty()) sess.id = GenerateUuid();
            if (!startStr.empty()) sess.start = ParseISO(startStr);
            if (!endStr.empty()) sess.end = ParseISO(endStr);
            sess.seconds = (sess.end == 0) ? 0.0
                : std::max(0.0, std::difftime(sess.end, sess.start));
            state.sessions.push_back(std::move(sess));
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Failed to parse %s: %s\n", path.c_str(), e.what());
        return false;
    }

    ApplyActiveFromSessions(state);
    return true;
}

bool SaveState(const TTState& state) {
    nlohmann::json j;
    j["version"] = 3;
    j["tasks"] = nlohmann::json::array();
    for (const auto& t : state.tasks) {
        j["tasks"].push_back({{"id", t.id}, {"name", t.name}});
    }
    j["sessions"] = nlohmann::json::array();
    for (const auto& s : state.sessions) {
        nlohmann::json sj;
        sj["id"] = s.id;
        sj["task_id"] = s.taskId;
        sj["start"] = FormatISO(s.start);
        if (s.end == 0) sj["end"] = nullptr;
        else            sj["end"] = FormatISO(s.end);
        j["sessions"].push_back(std::move(sj));
    }
    return WriteFileAtomic(GetDataPath(), j.dump(2));
}
