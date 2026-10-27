#include "cli.h"
#include "data.h"
#include "git_sync.h"
#include <nlohmann/json.hpp>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

using nlohmann::json;

constexpr const char* kUsage =
    "Usage: tt [--window | --popup]\n"
    "       tt <command> [args]\n"
    "\n"
    "With no command, tt opens its window. On macOS tt also lives in the menu\n"
    "bar: its icon shows or hides the window, and --popup toggles it (for a\n"
    "keyboard shortcut). On Omarchy it lives in the bar instead; --window\n"
    "forces the window there too, and --popup shows or hides the bar widget's\n"
    "dropdown.\n"
    "\n"
    "Commands (each prints one JSON object):\n"
    "  status           tasks, the running task and per-range totals\n"
    "  toggle <task>    start <task> (stopping any other), or stop it\n"
    "  start <task>     start <task>, stopping any other running task\n"
    "  stop             stop the running task\n"
    "  add <name>       add a task\n"
    "  sync             commit and push the data dir if it is a git repo\n"
    "\n"
    "<task> is a task id or its exact name.\n";

void Print(const json& j) {
    std::printf("%s\n", j.dump().c_str());
}

int Fail(const std::string& error) {
    Print({{"ok", false}, {"error", error}});
    return 1;
}

json StatusJson(const TTState& state) {
    std::time_t now = std::time(nullptr);
    std::filesystem::path path = GetDataPath();
    const Task* active =
        state.activeTask >= 0 ? &state.tasks[state.activeTask] : nullptr;

    json tasks = json::array();
    for (const auto& t : state.tasks) {
        tasks.push_back({{"id", t.id}, {"name", t.name}, {"active", t.active}});
    }

    // One entry per SummaryRange; `seconds` is parallel to `tasks` and
    // already includes the running session up to `now`.
    json ranges = json::array();
    for (int r = 0; r < (int)SummaryRange::Count; ++r) {
        auto range = static_cast<SummaryRange>(r);
        auto [start, end] = GetRangeBounds(range);
        json seconds = json::array();
        for (size_t i = 0; i < state.tasks.size(); ++i) {
            seconds.push_back((long long)GetTaskTime(state, (int)i, range, now));
        }
        ranges.push_back({{"label", RangeLabel(range)},
                          {"header", RangeHeader(range)},
                          {"start", (long long)start},
                          {"end", (long long)end},
                          {"seconds", std::move(seconds)}});
    }

    return {{"now", (long long)now},
            {"dataPath", path.string()},
            {"git", IsGitRepo(path.parent_path())},
            {"active", active ? json(active->id) : json(nullptr)},
            {"activeSince", active ? (long long)active->wallStart : 0LL},
            {"tasks", std::move(tasks)},
            {"ranges", std::move(ranges)}};
}

// A missing state file is an empty state; an unreadable one must not be
// overwritten, so commands that save refuse to continue.
bool LoadForWrite(TTState& state, std::string& error) {
    if (LoadState(state)) return true;
    std::error_code ec;
    if (std::filesystem::exists(GetDataPath(), ec)) {
        error = "Could not read " + GetDataPath().string();
        return false;
    }
    return true;
}

int ResolveTask(const TTState& state, const std::string& key) {
    int idx = FindTaskById(state, key);
    if (idx >= 0) return idx;
    for (size_t i = 0; i < state.tasks.size(); ++i) {
        if (state.tasks[i].name == key) return (int)i;
    }
    return -1;
}

std::string JoinArgs(int argc, char** argv, int from) {
    std::string out;
    for (int i = from; i < argc; ++i) {
        if (!out.empty()) out += ' ';
        out += argv[i];
    }
    return out;
}

std::string Trim(std::string s) {
    const char* ws = " \t\r\n";
    s.erase(0, s.find_first_not_of(ws));
    s.erase(s.find_last_not_of(ws) + 1);
    return s;
}

int Reply(const TTState& state, const std::string& message) {
    if (!SaveState(state)) return Fail("Failed to save state");
    Print({{"ok", true},
           {"changed", true},
           {"message", message},
           {"status", StatusJson(state)}});
    return 0;
}

int CmdStatus() {
    TTState state;
    LoadState(state);
    Print({{"ok", true}, {"status", StatusJson(state)}});
    return 0;
}

int CmdToggle(const std::string& key, bool startOnly) {
    TTState state;
    std::string error;
    if (!LoadForWrite(state, error)) return Fail(error);
    int idx = ResolveTask(state, key);
    if (idx < 0) return Fail("No task '" + key + "'");

    const std::string& name = state.tasks[idx].name;
    const std::string time = FormatClock(std::time(nullptr));
    if (startOnly && state.tasks[idx].active) {
        Print({{"ok", true},
               {"changed", false},
               {"message", "'" + name + "' is already running"},
               {"status", StatusJson(state)}});
        return 0;
    }
    const bool started = ToggleTask(state, idx);
    return Reply(state, (started ? "Started '" : "Stopped '") + name +
                            "' at " + time);
}

int CmdStop() {
    TTState state;
    std::string error;
    if (!LoadForWrite(state, error)) return Fail(error);
    if (state.activeTask < 0) {
        Print({{"ok", true},
               {"changed", false},
               {"message", "No task running"},
               {"status", StatusJson(state)}});
        return 0;
    }
    const std::string name = state.tasks[state.activeTask].name;
    StopTask(state, state.activeTask);
    return Reply(state, "Stopped '" + name + "' at " +
                            FormatClock(std::time(nullptr)));
}

int CmdAdd(const std::string& rawName) {
    std::string name = Trim(rawName);
    if (name.empty()) return Fail("Task name is empty");
    TTState state;
    std::string error;
    if (!LoadForWrite(state, error)) return Fail(error);
    int idx = AddTask(state, name);
    if (!SaveState(state)) return Fail("Failed to save state");
    Print({{"ok", true},
           {"changed", true},
           {"message", "Added '" + name + "'"},
           {"taskId", state.tasks[idx].id},
           {"status", StatusJson(state)}});
    return 0;
}

int CmdSync() {
    std::filesystem::path dir = GetDataPath().parent_path();
    if (!IsGitRepo(dir)) {
        Print({{"ok", true}, {"skipped", true}, {"message", ""}});
        return 0;
    }
    GitSyncResult result = RunGitSync(dir);
    Print({{"ok", result.ok},
           {"skipped", false},
           {"message", GitSyncMessage(result, std::time(nullptr))}});
    return result.ok ? 0 : 1;
}

}  // namespace

int RunCli(int argc, char** argv) {
    if (argc < 2) return -1;
    const std::string cmd = argv[1];

    if (cmd == "-h" || cmd == "--help" || cmd == "help") {
        std::fputs(kUsage, stdout);
        return 0;
    }
    if (cmd == "status") return CmdStatus();
    if (cmd == "stop") return CmdStop();
    if (cmd == "sync") return CmdSync();
    if (cmd == "toggle" || cmd == "start") {
        if (argc < 3) return Fail("Usage: tt " + cmd + " <task>");
        return CmdToggle(JoinArgs(argc, argv, 2), cmd == "start");
    }
    if (cmd == "add") {
        if (argc < 3) return Fail("Usage: tt add <name>");
        return CmdAdd(JoinArgs(argc, argv, 2));
    }

    // Options (--window, toolkit flags) belong to the app.
    if (!cmd.empty() && cmd[0] == '-') return -1;
    std::fprintf(stderr, "tt: unknown command '%s'\n\n%s", cmd.c_str(), kUsage);
    return 2;
}
