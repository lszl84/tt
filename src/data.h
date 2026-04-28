#pragma once

#include <filesystem>
#include <ctime>
#include <string>

class App;

// Returns the directory where state.json lives. Honors $TT_DATA_DIR if set.
std::filesystem::path GetDataDir();
std::filesystem::path GetDataPath();

// Disk I/O.
bool LoadState(App& app);
bool SaveState(const App& app);

// Pure serialize: produce v3 JSON string from current app state.
// Used both by SaveState (to disk) and by the sync layer (to push remote).
std::string SerializeStateToJson(const App& app);

// Atomically write a JSON string to a path (write tmp then rename).
bool WriteFileAtomic(const std::filesystem::path& path, const std::string& contents);

// Merge a JSON snapshot from another machine into the current app state.
// Returns true if anything actually changed locally (i.e., we have new info).
// Safe to call on main thread; mutates app.tasks / app.sessions / active state.
bool MergeRemoteJson(App& app, const std::string& json);

// Time helpers.
std::time_t TodayMidnight();
std::string FormatISO(std::time_t t);
std::time_t ParseISO(const std::string& s);

// Generate a random UUID-like string (8-4-4-4-12 hex).
std::string GenerateUuid();
