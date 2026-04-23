#include "data.h"
#include "app.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <chrono>
#include <cstdio>

namespace {

std::filesystem::path GetDataDir() {
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
    return std::mktime(&tm);
}

std::filesystem::path GetDataPath() {
    return GetDataDir() / "state.json";
}

bool LoadState(App& app) {
    std::filesystem::path path = GetDataPath();
    if (!std::filesystem::exists(path)) {
        return false;
    }
    std::ifstream f(path);
    if (!f) return false;

    try {
        nlohmann::json j;
        f >> j;

        int version = j.value("version", 0);
        if (version == 1) {
            // Migrate v1 -> v2: keep task names, discard accumulated time (no dates)
            app.tasks.clear();
            app.sessions.clear();
            for (const auto& t : j["tasks"]) {
                Task task;
                task.name = t.value("name", "Untitled");
                task.active = false;
                app.tasks.push_back(std::move(task));
            }
            std::fprintf(stderr, "Migrated v1 data to v2 (session-based tracking)\n");
            return true;
        }
        if (version != 2) return false;

        app.tasks.clear();
        for (const auto& t : j["tasks"]) {
            Task task;
            task.name = t.value("name", "Untitled");
            task.active = false;
            app.tasks.push_back(std::move(task));
        }

        app.sessions.clear();
        for (const auto& s : j.value("sessions", nlohmann::json::array())) {
            TimeSession sess;
            sess.taskName = s.value("task", "");
            std::string startStr = s.value("start", "");
            std::string endStr = s.value("end", "");
            if (!startStr.empty()) sess.start = ParseISO(startStr);
            if (!endStr.empty()) sess.end = ParseISO(endStr);
            sess.seconds = s.value("seconds", 0.0);
            app.sessions.push_back(std::move(sess));
        }

        // Restore active sessions
        for (const auto& a : j.value("active", nlohmann::json::array())) {
            std::string name = a.value("task", "");
            for (size_t i = 0; i < app.tasks.size(); ++i) {
                if (app.tasks[i].name == name) {
                    app.tasks[i].active = true;
                    std::string startStr = a.value("start", "");
                    if (!startStr.empty()) app.tasks[i].wallStart = ParseISO(startStr);
                    double elapsed = a.value("elapsed", 0.0);
                    auto offset = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                        std::chrono::duration<double>(elapsed));
                    app.tasks[i].steadyStart = std::chrono::steady_clock::now() - offset;
                    app.activeTask = (int)i;
                    app.selectedTask = (int)i;
                    break;
                }
            }
        }
    } catch (...) {
        return false;
    }
    return true;
}

bool SaveState(const App& app) {
    std::filesystem::path dir = GetDataDir();
    std::filesystem::path finalPath = GetDataPath();
    std::filesystem::path tmpPath = finalPath;
    tmpPath += ".tmp";

    std::filesystem::create_directories(dir);

    nlohmann::json j;
    j["version"] = 2;
    j["tasks"] = nlohmann::json::array();

    for (const auto& t : app.tasks) {
        nlohmann::json taskJson;
        taskJson["name"] = t.name;
        j["tasks"].push_back(taskJson);
    }

    j["sessions"] = nlohmann::json::array();
    for (const auto& s : app.sessions) {
        nlohmann::json sess;
        sess["task"] = s.taskName;
        sess["start"] = FormatISO(s.start);
        sess["end"] = FormatISO(s.end);
        sess["seconds"] = s.seconds;
        j["sessions"].push_back(sess);
    }

    j["active"] = nlohmann::json::array();
    auto now_steady = std::chrono::steady_clock::now();
    for (const auto& t : app.tasks) {
        if (t.active) {
            double elapsed = std::chrono::duration<double>(now_steady - t.steadyStart).count();
            nlohmann::json a;
            a["task"] = t.name;
            a["start"] = FormatISO(t.wallStart);
            a["elapsed"] = elapsed;
            j["active"].push_back(a);
        }
    }

    {
        std::ofstream f(tmpPath);
        if (!f) return false;
        f << j.dump(2);
        if (!f.good()) return false;
    }
    std::filesystem::rename(tmpPath, finalPath);
    return true;
}
