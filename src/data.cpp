#include "data.h"
#include "app.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <cstdlib>

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
        if (version != 1) return false;

        app.tasks.clear();
        for (const auto& t : j["tasks"]) {
            Task task;
            task.name = t.value("name", "Untitled");
            task.totalSeconds = t.value("totalSeconds", 0.0);
            task.active = false;
            app.tasks.push_back(std::move(task));
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
    j["version"] = 1;
    j["tasks"] = nlohmann::json::array();

    auto now = std::chrono::steady_clock::now();
    for (const auto& t : app.tasks) {
        double total = t.totalSeconds;
        if (t.active) {
            total += std::chrono::duration<double>(now - t.startTime).count();
        }
        nlohmann::json taskJson;
        taskJson["name"] = t.name;
        taskJson["totalSeconds"] = total;
        j["tasks"].push_back(taskJson);
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
