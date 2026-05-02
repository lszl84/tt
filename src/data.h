#pragma once

#include <chrono>
#include <ctime>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

struct Task {
    std::string id;            // stable UUID, persisted
    std::string name;
    bool active = false;
    std::chrono::steady_clock::time_point steadyStart;
    std::time_t wallStart = 0;
    std::string activeSessionId; // id of the open session driving this task
};

struct TimeSession {
    std::string id;            // stable UUID, persisted
    std::string taskId;        // foreign key to Task.id
    std::time_t start = 0;
    std::time_t end = 0;        // 0 == open / active session
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

struct TTState {
    std::vector<Task> tasks;
    std::vector<TimeSession> sessions;
    int activeTask = -1;
};

// On-disk path. Honors $TT_DATA_DIR.
std::filesystem::path GetDataPath();

// Load / save state.json (version 3). Returns false if no usable file exists.
bool LoadState(TTState& state);
bool SaveState(const TTState& state);

// Random RFC4122-ish UUID.
std::string GenerateUuid();

// Format a duration in seconds as "1h 02m 03s" / "2m 03s" / "12s".
std::string FormatDuration(double seconds);

// [start, end) wall-clock range covered by a SummaryRange option.
std::pair<std::time_t, std::time_t> GetRangeBounds(SummaryRange r);
const char* RangeLabel(SummaryRange r);
const char* RangeHeader(SummaryRange r);

// Total tracked time for one task within `range`. Counts open sessions up to
// `now` so the active row updates live.
double GetTaskTime(const TTState& state, int idx, SummaryRange range,
                   std::chrono::steady_clock::time_point now);

// Mutators. Caller is responsible for persisting after.
void StartTask(TTState& state, int idx);
void StopTask(TTState& state, int idx);

int FindTaskById(const TTState& state, const std::string& id);
