#include "omarchy.h"
#include "data.h"

#if defined(__linux__)

#include <nlohmann/json.hpp>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr const char* kPluginId = "com.devmindscape.tt";

// Run `args` (argv[0] is looked up in PATH) and capture its stdout. Returns
// the exit status, or -1 if it could not be run.
int Run(const std::vector<std::string>& args, std::string* out,
        bool quiet = false) {
    int fds[2];
    if (pipe(fds) != 0) return -1;
    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return -1;
    }
    if (pid == 0) {
        dup2(fds[1], STDOUT_FILENO);
        close(fds[0]);
        close(fds[1]);
        if (quiet) {
            int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0) dup2(devnull, STDERR_FILENO);
        }
        std::vector<char*> argv;
        for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(127);
    }
    close(fds[1]);
    std::string data;
    char buf[4096];
    for (;;) {
        ssize_t n = read(fds[0], buf, sizeof(buf));
        if (n > 0) data.append(buf, (size_t)n);
        else if (n < 0 && errno == EINTR) continue;
        else break;
    }
    close(fds[0]);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) return -1;
    }
    if (out) *out = std::move(data);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

std::string Trim(std::string s) {
    const char* ws = " \t\r\n";
    s.erase(0, s.find_first_not_of(ws));
    s.erase(s.find_last_not_of(ws) + 1);
    return s;
}

// IPC into the running shell; returns its trimmed reply.
std::string ShellCall(std::vector<std::string> args, bool quiet = false) {
    std::string tool = "omarchy-shell";
    if (const char* root = std::getenv("OMARCHY_PATH"); root && root[0]) {
        std::error_code ec;
        fs::path candidate = fs::path(root) / "bin" / "omarchy-shell";
        if (fs::exists(candidate, ec)) tool = candidate.string();
    }
    args.insert(args.begin(), tool);
    std::string out;
    Run(args, &out, quiet);
    return Trim(out);
}

template <class Pred>
bool WaitFor(Pred pred, int timeoutMs = 5000) {
    using namespace std::chrono;
    auto deadline = steady_clock::now() + milliseconds(timeoutMs);
    for (;;) {
        if (pred()) return true;
        if (steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(milliseconds(150));
    }
}

fs::path ExePath() {
    std::error_code ec;
    fs::path p = fs::read_symlink("/proc/self/exe", ec);
    return ec ? fs::path() : p;
}

// Plugin files ship next to an installed binary; a binary run straight from
// a build directory falls back to the source tree.
fs::path PluginSourceDir() {
    std::vector<fs::path> candidates;
    if (fs::path exe = ExePath(); !exe.empty()) {
        candidates.push_back(
            (exe.parent_path() / TT_OMARCHY_PLUGIN_REL).lexically_normal());
    }
    candidates.push_back(TT_OMARCHY_PLUGIN_SOURCE_DIR);
    for (const auto& dir : candidates) {
        std::error_code ec;
        if (fs::exists(dir / "manifest.json", ec)) return dir;
    }
    return {};
}

fs::path PluginInstallDir() {
    const char* home = std::getenv("HOME");
    if (!home || !home[0]) return {};
    return fs::path(home) / ".config" / "omarchy" / "plugins" / kPluginId;
}

std::string ReadFile(const fs::path& path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// The shell hot-reloads the plugin on every write, so leave identical files
// alone.
bool WriteIfChanged(const fs::path& path, const std::string& contents,
                    bool& changed) {
    std::error_code ec;
    if (fs::exists(path, ec) && ReadFile(path) == contents) return true;
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(contents.data(), (std::streamsize)contents.size());
    changed = true;
    return f.good();
}

// Points the widget at this executable.
std::string BackendJs() {
    fs::path exe = ExePath();
    return ".pragma library\n"
           "\n"
           "// The tt executable this widget drives. tt rewrites this file with its own\n"
           "// path whenever it installs the widget.\n"
           "var command = " +
           nlohmann::json(exe.empty() ? std::string("tt") : exe.string()).dump() +
           "\n";
}

// Plugin files keyed by their path relative to the plugin dir.
std::map<std::string, std::string> PluginFiles(const fs::path& src) {
    std::map<std::string, std::string> files;
    for (const auto& entry : fs::recursive_directory_iterator(src)) {
        if (!entry.is_regular_file()) continue;
        std::string rel = fs::relative(entry.path(), src).generic_string();
        files[rel] = rel == "Backend.js" ? BackendJs() : ReadFile(entry.path());
    }
    return files;
}

// Short content hash (FNV-1a) naming one version of the widget code.
std::string Revision(const std::map<std::string, std::string>& files) {
    uint64_t h = 1469598103934665603ULL;
    auto mix = [&h](const std::string& s) {
        for (unsigned char c : s) {
            h ^= c;
            h *= 1099511628211ULL;
        }
        h ^= 0xff;
        h *= 1099511628211ULL;
    };
    for (const auto& [rel, contents] : files) {
        mix(rel);
        mix(contents);
    }
    char buf[16];
    std::snprintf(buf, sizeof(buf), "r%012llx",
                  (unsigned long long)(h & 0xffffffffffffULL));
    return buf;
}

// The shell keeps compiled QML for its whole lifetime, keyed by URL, so an
// updated widget would keep running the old code. Install the code under a
// directory named after its contents and point the manifest there: a new
// version gets a new URL and loads fresh.
bool InstallPlugin(bool& changed, std::string& rev) {
    fs::path src = PluginSourceDir();
    fs::path dst = PluginInstallDir();
    if (src.empty() || dst.empty()) return false;

    auto files = PluginFiles(src);
    auto manifestIt = files.find("manifest.json");
    if (manifestIt == files.end()) return false;
    auto manifest = nlohmann::json::parse(manifestIt->second);
    files.erase(manifestIt);

    rev = Revision(files);
    for (auto& [kind, entry] : manifest["entryPoints"].items()) {
        entry = rev + "/" + entry.get<std::string>();
    }

    std::error_code ec;
    for (const auto& [rel, contents] : files) {
        fs::path target = dst / rev / rel;
        fs::create_directories(target.parent_path(), ec);
        if (!WriteIfChanged(target, contents, changed)) return false;
    }
    if (!WriteIfChanged(dst / "manifest.json", manifest.dump(2) + "\n", changed)) {
        return false;
    }

    // The plugin dir belongs to tt: drop superseded revisions and files from
    // older installs.
    for (const auto& entry : fs::directory_iterator(dst)) {
        std::string name = entry.path().filename().string();
        if (name == rev || name == "manifest.json") continue;
        fs::remove_all(entry.path(), ec);
        changed = true;
    }
    return true;
}

// -1: the shell doesn't know the plugin, 0: known but off, 1: in the bar.
int PluginState() {
    try {
        auto plugins = nlohmann::json::parse(ShellCall({"shell", "listPlugins"}));
        for (const auto& p : plugins) {
            if (p.value("id", std::string{}) == kPluginId) {
                return p.value("enabled", false) ? 1 : 0;
            }
        }
    } catch (const std::exception&) {
    }
    return -1;
}

// Wait until the widget runs the code installed as `rev`. After an update
// the shell reloads the plugin by itself, sometimes twice in a row, so the
// new code must answer from the same instance twice before it counts.
bool WaitForWidget(const std::string& rev, bool changed) {
    using namespace std::chrono;
    auto start = steady_clock::now();
    std::string last;
    bool rescanned = false;
    while (steady_clock::now() - start < seconds(8)) {
        std::string reply = ShellCall({kPluginId, "revision"}, /*quiet=*/true);
        if (reply.find(rev) != std::string::npos) {
            if (!changed || reply == last) return true;
            last = reply;
            std::this_thread::sleep_for(milliseconds(400));
            continue;
        }
        last.clear();
        // The shell's file watcher normally does this.
        if (!rescanned && steady_clock::now() - start > seconds(2)) {
            ShellCall({"shell", "rescanPlugins"});
            rescanned = true;
        }
        std::this_thread::sleep_for(milliseconds(150));
    }
    return false;
}

// Same first-run starter tasks the window creates.
void SeedStateIfNew() {
    TTState state;
    std::error_code ec;
    bool loaded = LoadState(state);
    if (!loaded && fs::exists(GetDataPath(), ec)) return;  // unreadable: hands off
    if (SeedDefaultTasks(state)) SaveState(state);
}

}  // namespace

bool OmarchyShellAvailable() {
    const char* root = std::getenv("OMARCHY_PATH");
    if (!root || !root[0]) return false;
    // Not omarchy-shell -q: that hides the reply as well as the errors.
    return ShellCall({"shell", "ping"}, /*quiet=*/true) == "ok";
}

bool RunOmarchyLauncher(bool togglePopup) {
    try {
        bool changed = false;
        std::string rev;
        if (!InstallPlugin(changed, rev)) {
            std::fprintf(stderr, "tt: could not install the Omarchy bar widget\n");
            return false;
        }
        if (PluginState() < 0) ShellCall({"shell", "rescanPlugins"});

        int state = -1;
        if (!WaitFor([&] { state = PluginState(); return state >= 0; })) {
            std::fprintf(stderr, "tt: the Omarchy shell did not load the bar widget\n");
            return false;
        }
        SeedStateIfNew();

        if (state == 0) {
            std::string reply = ShellCall({"shell", "enablePlugin", kPluginId, "{}"});
            if (reply != "ok") {
                std::fprintf(stderr, "tt: could not add the widget to the bar: %s\n",
                             reply.c_str());
                return false;
            }
            if (!togglePopup) {
                // The new icon in the bar is the feedback; leave the dropdown shut.
                std::printf("tt is now in the Omarchy bar\n");
                return true;
            }
            changed = true;  // the widget is still being created
        }

        // Already in the bar: open the dropdown so launching tt does something
        // visible. After an update, wait for the new code first, or the
        // dropdown opens on the old widget just before a reload replaces it.
        if (!WaitForWidget(rev, changed)) {
            std::fprintf(stderr, "tt: could not open the bar widget\n");
            return false;
        }
        if (togglePopup) {
            ShellCall({"shell", "toggle", kPluginId, "{}"});  // replies with nothing
            return true;
        }
        if (ShellCall({"shell", "summon", kPluginId, "{}"}) != "ok") {
            std::fprintf(stderr, "tt: could not open the bar widget\n");
            return false;
        }
        return true;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "tt: Omarchy setup failed: %s\n", e.what());
        return false;
    }
}

#else

bool OmarchyShellAvailable() { return false; }
bool RunOmarchyLauncher(bool) { return false; }

#endif
