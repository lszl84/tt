#include "git_sync.h"
#include "data.h"
#include <array>
#include <cctype>
#include <cstdio>

bool IsGitRepo(const std::filesystem::path& dir) {
    std::error_code ec;
    return std::filesystem::exists(dir / ".git", ec);
}

GitSyncResult RunGitSync(const std::filesystem::path& dir) {
    // cd to the data dir, commit and push
    std::string cmd = "cd \"" + dir.string() + "\" && "
                      "git add -A && "
                      "git commit -m \"auto: updates\" && "
                      "git push 2>&1";

    GitSyncResult result;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        result.output = "failed to run git";
        return result;
    }
    std::array<char, 256> buffer;
    while (fgets(buffer.data(), (int)buffer.size(), pipe) != nullptr) {
        result.output += buffer.data();
    }
    result.ok = (pclose(pipe) == 0);
    return result;
}

std::string GitSyncMessage(const GitSyncResult& result, std::time_t now) {
    if (result.ok) return "Pushed at " + FormatClock(now);
    std::string detail = result.output;
    while (!detail.empty() && std::isspace((unsigned char)detail.back())) {
        detail.pop_back();
    }
    return "Git error: " + detail;
}
