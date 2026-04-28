#include "data.h"
#include "app.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <random>
#include <mutex>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>

namespace {

std::filesystem::path DefaultDataDir() {
#if defined(__APPLE__)
    const char* home = std::getenv("HOME");
    if (home && home[0]) {
        return std::filesystem::path(home) / "Library" / "Application Support" / "tt";
    }
#else
    const char* xdg_data = std::getenv("XDG_DATA_HOME");
    if (xdg_data && xdg_data[0]) {
        return std::filesystem::path(xdg_data) / "tt";
    }
    const char* home = std::getenv("HOME");
    if (home && home[0]) {
        return std::filesystem::path(home) / ".local" / "share" / "tt";
    }
#endif
    return std::filesystem::current_path() / ".tt";
}

std::string IsoOrNull(std::time_t t) {
    return t == 0 ? std::string{} : FormatISO(t);
}

// Read entire file into a string. Returns empty string on error / missing file.
std::string ReadFile(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Convert a parsed v3 JSON object into Task/TimeSession vectors.
// Throws nlohmann::json exceptions on malformed structure.
void ParseV3(const nlohmann::json& j,
             std::vector<Task>& tasks,
             std::vector<TimeSession>& sessions) {
    tasks.clear();
    sessions.clear();
    for (const auto& t : j.value("tasks", nlohmann::json::array())) {
        Task task;
        task.id = t.value("id", std::string{});
        task.name = t.value("name", std::string("Untitled"));
        if (task.id.empty()) task.id = GenerateUuid();
        tasks.push_back(std::move(task));
    }
    for (const auto& s : j.value("sessions", nlohmann::json::array())) {
        TimeSession sess;
        sess.id = s.value("id", std::string{});
        sess.taskId = s.value("task_id", std::string{});
        std::string startStr = s.value("start", std::string{});
        // "end" may be null (active session) — value<>() doesn't handle nulls
        // cleanly so check explicitly.
        std::string endStr;
        if (s.contains("end") && !s["end"].is_null()) {
            endStr = s["end"].get<std::string>();
        }
        if (sess.id.empty()) sess.id = GenerateUuid();
        if (!startStr.empty()) sess.start = ParseISO(startStr);
        if (!endStr.empty()) sess.end = ParseISO(endStr);
        sess.seconds = (sess.end == 0) ? 0.0
                                       : std::max(0.0, std::difftime(sess.end, sess.start));
        sessions.push_back(std::move(sess));
    }
}

// v2 → v3 in-memory conversion. v2 referenced tasks by name and stored active
// sessions in a separate "active" array.
void MigrateV2(const nlohmann::json& j,
               std::vector<Task>& tasks,
               std::vector<TimeSession>& sessions) {
    tasks.clear();
    sessions.clear();

    std::unordered_map<std::string, std::string> nameToId;
    for (const auto& t : j.value("tasks", nlohmann::json::array())) {
        Task task;
        task.id = GenerateUuid();
        task.name = t.value("name", std::string("Untitled"));
        nameToId[task.name] = task.id;
        tasks.push_back(std::move(task));
    }

    auto resolveTaskId = [&](const std::string& name) -> std::string {
        auto it = nameToId.find(name);
        if (it != nameToId.end()) return it->second;
        // Orphan session — synthesize a task.
        Task t;
        t.id = GenerateUuid();
        t.name = name.empty() ? std::string("Untitled") : name;
        nameToId[t.name] = t.id;
        tasks.push_back(std::move(t));
        return nameToId[name];
    };

    for (const auto& s : j.value("sessions", nlohmann::json::array())) {
        TimeSession sess;
        sess.id = GenerateUuid();
        sess.taskId = resolveTaskId(s.value("task", std::string{}));
        std::string startStr = s.value("start", std::string{});
        std::string endStr   = s.value("end", std::string{});
        if (!startStr.empty()) sess.start = ParseISO(startStr);
        if (!endStr.empty())   sess.end   = ParseISO(endStr);
        sess.seconds = (sess.end == 0) ? 0.0
                                       : std::max(0.0, std::difftime(sess.end, sess.start));
        sessions.push_back(std::move(sess));
    }

    // v2 tracked active sessions in a separate "active" array — promote each to
    // a session entry with end=0.
    for (const auto& a : j.value("active", nlohmann::json::array())) {
        TimeSession sess;
        sess.id = GenerateUuid();
        sess.taskId = resolveTaskId(a.value("task", std::string{}));
        std::string startStr = a.value("start", std::string{});
        if (!startStr.empty()) sess.start = ParseISO(startStr);
        sess.end = 0;
        sess.seconds = 0;
        sessions.push_back(std::move(sess));
    }
}

// Set runtime "active" fields on tasks based on which sessions are still open.
// If multiple open sessions exist for one task, the earliest-started one wins
// (it's the canonical active one; others were dropped/duplicated).
void ApplyActiveFromSessions(App& app) {
    for (auto& t : app.tasks) {
        t.active = false;
        t.activeSessionId.clear();
        t.wallStart = 0;
    }
    app.activeTask = -1;

    for (const auto& s : app.sessions) {
        if (s.end != 0) continue;
        int idx = app.FindTaskById(s.taskId);
        if (idx < 0) continue;
        Task& tt = app.tasks[idx];
        if (tt.active && tt.wallStart <= s.start) continue; // earlier session wins
        tt.active = true;
        tt.activeSessionId = s.id;
        tt.wallStart = s.start;
        std::time_t now_t = std::time(nullptr);
        double elapsed = std::max(0.0, std::difftime(now_t, s.start));
        auto offset = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(elapsed));
        tt.steadyStart = std::chrono::steady_clock::now() - offset;
        app.activeTask = idx;
    }
}

} // namespace

std::time_t TodayMidnight() {
    auto now = std::time(nullptr);
    auto tm = *std::localtime(&now);
    tm.tm_hour = 0; tm.tm_min = 0; tm.tm_sec = 0;
    return std::mktime(&tm);
}

std::string FormatISO(std::time_t t) {
    if (t == 0) return "";
    auto tm = *std::localtime(&t);
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

std::string GenerateUuid() {
    // Thread-safe RFC 4122 v4-ish UUID.
    static std::mutex mu;
    static std::mt19937_64 rng{std::random_device{}()};
    std::lock_guard<std::mutex> lock(mu);
    uint64_t a = rng();
    uint64_t b = rng();
    // Force version 4 + variant bits.
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

std::filesystem::path GetDataDir() {
    if (const char* env = std::getenv("TT_DATA_DIR")) {
        if (env[0]) return std::filesystem::path(env);
    }
    return DefaultDataDir();
}

std::filesystem::path GetDataPath() {
    return GetDataDir() / "state.json";
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
    if (ec) return false;
    return true;
}

std::string SerializeStateToJson(const App& app) {
    nlohmann::json j;
    j["version"] = 3;

    j["tasks"] = nlohmann::json::array();
    for (const auto& t : app.tasks) {
        nlohmann::json tj;
        tj["id"] = t.id;
        tj["name"] = t.name;
        j["tasks"].push_back(std::move(tj));
    }

    j["sessions"] = nlohmann::json::array();
    for (const auto& s : app.sessions) {
        nlohmann::json sj;
        sj["id"] = s.id;
        sj["task_id"] = s.taskId;
        sj["start"] = IsoOrNull(s.start);
        if (s.end == 0) sj["end"] = nullptr;
        else            sj["end"] = IsoOrNull(s.end);
        j["sessions"].push_back(std::move(sj));
    }

    return j.dump(2);
}

bool LoadState(App& app) {
    auto path = GetDataPath();
    std::string text = ReadFile(path);
    if (text.empty()) return false;

    try {
        auto j = nlohmann::json::parse(text);
        int version = j.value("version", 0);
        if (version == 3) {
            ParseV3(j, app.tasks, app.sessions);
        } else if (version == 2) {
            MigrateV2(j, app.tasks, app.sessions);
            // Keep a one-time .v2.bak so the old file isn't silently lost.
            auto bak = path;
            bak += ".v2.bak";
            std::error_code ec;
            std::filesystem::copy_file(path, bak,
                std::filesystem::copy_options::overwrite_existing, ec);
            std::fprintf(stderr, "Migrated v2 → v3 (backup at %s)\n", bak.c_str());
        } else if (version == 1) {
            // v1 had no per-session timestamps. Best we can do is keep names.
            app.tasks.clear();
            app.sessions.clear();
            for (const auto& t : j.value("tasks", nlohmann::json::array())) {
                Task task;
                task.id = GenerateUuid();
                task.name = t.value("name", std::string("Untitled"));
                app.tasks.push_back(std::move(task));
            }
            std::fprintf(stderr, "Migrated v1 → v3 (sessions discarded)\n");
        } else {
            return false;
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Failed to parse %s: %s\n", path.c_str(), e.what());
        return false;
    }

    ApplyActiveFromSessions(app);
    if (!app.tasks.empty() && app.selectedTask < 0) app.selectedTask = 0;
    return true;
}

bool SaveState(const App& app) {
    return WriteFileAtomic(GetDataPath(), SerializeStateToJson(app));
}

bool MergeRemoteJson(App& app, const std::string& json) {
    if (json.empty()) return false;

    std::vector<Task> rTasks;
    std::vector<TimeSession> rSessions;
    try {
        auto j = nlohmann::json::parse(json);
        int v = j.value("version", 0);
        if (v == 3) {
            ParseV3(j, rTasks, rSessions);
        } else if (v == 2) {
            MigrateV2(j, rTasks, rSessions);
        } else {
            return false; // unknown version, skip
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Sync: bad remote JSON: %s\n", e.what());
        return false;
    }

    bool changed = false;

    // ---- Merge tasks: union by id. Remote name wins iff local task is unnamed.
    std::unordered_map<std::string, int> localTaskIdx;
    for (size_t i = 0; i < app.tasks.size(); ++i)
        localTaskIdx[app.tasks[i].id] = (int)i;

    for (const auto& rt : rTasks) {
        if (rt.id.empty()) continue;
        auto it = localTaskIdx.find(rt.id);
        if (it == localTaskIdx.end()) {
            app.tasks.push_back(rt);
            localTaskIdx[rt.id] = (int)app.tasks.size() - 1;
            changed = true;
        } else {
            // Keep local name; tasks rarely change.
            (void)0;
        }
    }

    // ---- Merge sessions: union by id; for same id, completed wins, then later end.
    std::unordered_map<std::string, int> localSessIdx;
    for (size_t i = 0; i < app.sessions.size(); ++i)
        localSessIdx[app.sessions[i].id] = (int)i;

    for (const auto& rs : rSessions) {
        if (rs.id.empty()) continue;
        auto it = localSessIdx.find(rs.id);
        if (it == localSessIdx.end()) {
            app.sessions.push_back(rs);
            changed = true;
        } else {
            TimeSession& cur = app.sessions[it->second];
            // completed (end != 0) wins over open (end == 0)
            if (cur.end == 0 && rs.end != 0) {
                cur = rs;
                changed = true;
            } else if (cur.end != 0 && rs.end != 0 && rs.end > cur.end) {
                // Both completed: later end wins (assumes loosely-synced clocks).
                cur = rs;
                changed = true;
            }
            // Other cases: keep local.
        }
    }

    // After merge, re-derive active state from sessions.
    ApplyActiveFromSessions(app);
    return changed;
}
