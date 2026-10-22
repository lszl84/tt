#pragma once

#include <ctime>
#include <filesystem>
#include <string>

struct GitSyncResult {
    bool ok = false;
    std::string output;  // git's output, used as the error detail
};

// True if `dir` is the root of a git work tree.
bool IsGitRepo(const std::filesystem::path& dir);

// git add -A && git commit && git push inside `dir`. Blocks until git exits.
GitSyncResult RunGitSync(const std::filesystem::path& dir);

// Status line for a sync result: "Pushed at HH:MM" / "Git error: ...".
std::string GitSyncMessage(const GitSyncResult& result, std::time_t now);
