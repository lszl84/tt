#include "sync.h"
#include "app.h"
#include "data.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <unistd.h>
#endif

namespace {

constexpr const char* kSyncFolder = "tt-sync";
constexpr const char* kSyncFile = "data.json";

// =====================================================================
// FilesystemBackend — used on macOS (Drive desktop client) and as a
// $TT_SYNC_PATH fallback. The remote is just a path the OS keeps in sync.
// =====================================================================

class FilesystemBackend : public SyncBackend {
public:
    explicit FilesystemBackend(std::filesystem::path file, std::string desc)
        : file_(std::move(file)), desc_(std::move(desc)) {}

    std::optional<std::string> Read() override {
        std::error_code ec;
        if (!std::filesystem::exists(file_, ec)) {
            // Distinguish "remote reachable but file missing" from "unreachable".
            // For a plain filesystem this just means: parent must exist.
            std::filesystem::create_directories(file_.parent_path(), ec);
            if (ec) return std::nullopt;
            return std::string{};
        }
        std::ifstream f(file_, std::ios::binary);
        if (!f) return std::nullopt;
        std::stringstream ss;
        ss << f.rdbuf();
        return ss.str();
    }

    bool Write(const std::string& contents) override {
        std::error_code ec;
        std::filesystem::create_directories(file_.parent_path(), ec);
        if (ec) return false;
        return WriteFileAtomic(file_, contents);
    }

    std::string Description() const override { return desc_; }

private:
    std::filesystem::path file_;
    std::string desc_;
};

// =====================================================================
// GvfsBackend — Linux Google Drive via the gvfs FUSE mount that GNOME
// publishes at /run/user/<uid>/gvfs/google-drive:*. The gio CLI's `cat`/`copy`
// commands don't reliably support writing to Drive URIs (gvfs returns
// "Operation not supported" on at least gvfs 2.86), but plain POSIX I/O on
// the mount path works. The path is volatile across mounts but stable while
// the GVFS mount is up — re-discover at startup, treat lookup failure as
// "offline".
// =====================================================================

class GvfsBackend : public SyncBackend {
public:
    GvfsBackend(std::filesystem::path file, std::string desc)
        : file_(std::move(file)), desc_(std::move(desc)) {}

    std::optional<std::string> Read() override {
        std::error_code ec;
        // If the mount went away, parent_path won't exist.
        auto parent = file_.parent_path();
        if (!std::filesystem::exists(parent.parent_path(), ec)) {
            return std::nullopt; // Drive root missing → offline
        }
        if (!std::filesystem::exists(parent, ec)) {
            // tt-sync folder doesn't exist yet — try to create it. If that
            // works, treat as "remote reachable but empty".
            std::filesystem::create_directories(parent, ec);
            if (ec) return std::nullopt;
        }
        if (!std::filesystem::exists(file_, ec)) {
            return std::string{};
        }
        std::ifstream f(file_, std::ios::binary);
        if (!f) return std::nullopt;
        std::stringstream ss;
        ss << f.rdbuf();
        return ss.str();
    }

    bool Write(const std::string& contents) override {
        std::error_code ec;
        auto parent = file_.parent_path();
        std::filesystem::create_directories(parent, ec);
        // GVFS doesn't support rename across the mount in all cases, so write
        // directly. Atomicity is best-effort here — if the upload is
        // interrupted, the next pull-merge cycle reconciles.
        std::ofstream f(file_, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(contents.data(), (std::streamsize)contents.size());
        return f.good();
    }

    std::string Description() const override { return desc_; }

private:
    std::filesystem::path file_;
    std::string desc_;
};

// =====================================================================
// Backend discovery
// =====================================================================

#if !defined(__APPLE__)
// Locate the user's GVFS Google Drive mount and the "My Drive" folder under
// it. Returns the path to "My Drive" and a human-readable account label.
struct GvfsDrive {
    std::filesystem::path my_drive;
    std::string account; // e.g. "host=gmail.com,user=foo.bar"
};

std::optional<GvfsDrive> DiscoverGvfsGoogleDrive() {
    uid_t uid = getuid();
    std::filesystem::path gvfs_root =
        std::filesystem::path("/run/user") / std::to_string((unsigned)uid) / "gvfs";
    std::error_code ec;
    if (!std::filesystem::exists(gvfs_root, ec)) return std::nullopt;

    // Each Google account appears as a directory:
    //   google-drive:host=<host>,user=<user>
    for (auto& m : std::filesystem::directory_iterator(gvfs_root, ec)) {
        if (ec) break;
        std::string mname = m.path().filename().string();
        if (mname.rfind("google-drive:", 0) != 0) continue;

        // Inside each account, top-level entries are one folder per drive
        // (My Drive, Shared Drives) plus a "GVfsSharedWithMe" pseudo-folder.
        // The Drive ID format starts with "0A". Pick the first matching one.
        for (auto& d : std::filesystem::directory_iterator(m.path(), ec)) {
            if (ec) break;
            std::string dname = d.path().filename().string();
            if (dname.rfind("GVfs", 0) == 0) continue; // skip "GVfsSharedWithMe"
            // Heuristic: the user's primary drive root id begins with "0A".
            if (dname.size() < 2 || dname[0] != '0') continue;
            GvfsDrive gd;
            gd.my_drive = d.path();
            gd.account = mname.substr(std::strlen("google-drive:"));
            return gd;
        }
    }
    return std::nullopt;
}
#endif

#if defined(__APPLE__)
std::optional<std::filesystem::path> DiscoverMacGoogleDrive() {
    const char* home = std::getenv("HOME");
    if (!home || !home[0]) return std::nullopt;
    auto root = std::filesystem::path(home) / "Library" / "CloudStorage";
    std::error_code ec;
    if (!std::filesystem::exists(root, ec)) return std::nullopt;
    for (auto& e : std::filesystem::directory_iterator(root, ec)) {
        if (ec) break;
        std::string n = e.path().filename().string();
        if (n.rfind("GoogleDrive-", 0) == 0) {
            auto myDrive = e.path() / "My Drive";
            if (std::filesystem::exists(myDrive, ec)) return myDrive;
            return e.path();
        }
    }
    return std::nullopt;
}
#endif

} // namespace

std::unique_ptr<SyncBackend> DiscoverBackend() {
    // 1. Explicit override.
    if (const char* p = std::getenv("TT_SYNC_PATH")) {
        if (p[0]) {
            std::filesystem::path file(p);
            std::string desc = std::string("Path: ") + file.string();
            return std::make_unique<FilesystemBackend>(file, std::move(desc));
        }
    }

#if defined(__APPLE__)
    if (auto drive = DiscoverMacGoogleDrive()) {
        auto folder = *drive / kSyncFolder;
        auto file = folder / kSyncFile;
        std::string desc = std::string("Google Drive: ") + drive->filename().string();
        return std::make_unique<FilesystemBackend>(file, std::move(desc));
    }
#else
    if (auto drive = DiscoverGvfsGoogleDrive()) {
        auto file = drive->my_drive / kSyncFolder / kSyncFile;
        std::string desc = "Google Drive (" + drive->account + ")";
        return std::make_unique<GvfsBackend>(std::move(file), std::move(desc));
    }
#endif
    return nullptr;
}

// =====================================================================
// SyncManager
// =====================================================================

SyncManager& GetSyncManager() {
    static SyncManager mgr;
    return mgr;
}

SyncManager::~SyncManager() {
    Stop();
    if (wake_pipe_[0] >= 0) close(wake_pipe_[0]);
    if (wake_pipe_[1] >= 0) close(wake_pipe_[1]);
}

void SyncManager::Signal() {
    if (wake_pipe_[1] < 0) return;
    char b = 1;
    // Best-effort: pipe is non-blocking, so a full buffer just means the main
    // loop already has work pending.
    (void)write(wake_pipe_[1], &b, 1);
}

int SyncManager::WakeupFd() const {
    return wake_pipe_[0];
}

void SyncManager::Start(App& app) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (started_) return;
        started_ = true;
        backend_ = DiscoverBackend();
        if (backend_) {
            description_ = backend_->Description();
            app.syncBackendDescription = description_;
            status_.store(State::Syncing);
            // Self-pipe (non-blocking) for waking the UI main loop.
            if (pipe(wake_pipe_) == 0) {
                fcntl(wake_pipe_[0], F_SETFL, O_NONBLOCK);
                fcntl(wake_pipe_[1], F_SETFL, O_NONBLOCK);
            }
        } else {
            description_ = "No sync backend";
            app.syncBackendDescription = description_;
            status_.store(State::Idle);
            return; // no thread needed
        }
    }
    worker_ = std::thread([this] { Run(); });
}

void SyncManager::Stop() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!started_) return;
        stop_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
    started_ = false;
}

void SyncManager::RequestPush(std::string snapshot) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!backend_) return;
        pending_push_ = std::move(snapshot);
    }
    cv_.notify_all();
}

bool SyncManager::TryConsumePulled(std::string& out) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!pulled_) return false;
    out = std::move(*pulled_);
    pulled_.reset();
    return true;
}

std::string SyncManager::Description() const {
    std::lock_guard<std::mutex> lock(mu_);
    return description_;
}

void SyncManager::Run() {
    using clock = std::chrono::steady_clock;
    auto last_pull = clock::now() - std::chrono::seconds(120); // force initial pull
    constexpr auto kPullInterval = std::chrono::seconds(60);
    bool first_iteration = true;

    while (true) {
        std::optional<std::string> push_payload;
        bool do_pull = false;

        {
            std::unique_lock<std::mutex> lock(mu_);
            auto next_pull = last_pull + kPullInterval;
            cv_.wait_until(lock, next_pull, [this] {
                return stop_ || pending_push_.has_value() || force_pull_;
            });
            if (stop_) break;

            if (pending_push_) {
                push_payload = std::move(pending_push_);
                pending_push_.reset();
            }
            if (clock::now() >= next_pull || force_pull_) {
                do_pull = true;
                force_pull_ = false;
            }
        }

        bool any_op_ok = false;
        bool any_op_fail = false;
        bool delivered_pull = false;

        // Always pull before pushing, so main has a chance to ingest remote
        // changes and produce a merged push (rather than us blindly clobbering
        // remote with our un-merged local snapshot).
        if (do_pull) {
            auto data = backend_->Read();
            last_pull = clock::now();
            if (data) {
                any_op_ok = true;
                if (!data->empty()) {
                    std::lock_guard<std::mutex> lock(mu_);
                    pulled_ = std::move(*data);
                    delivered_pull = true;
                }
            } else {
                any_op_fail = true;
            }
        }

        // First-iteration safety: if we just delivered remote data to main,
        // defer the initial push. Main will merge on its next frame and call
        // RequestPush() with the merged state, which we'll handle on the next
        // worker iteration (woken by RequestPush's condvar notify).
        if (first_iteration && delivered_pull && push_payload) {
            std::lock_guard<std::mutex> lock(mu_);
            // Only restore if main hasn't already replaced it with a fresher one.
            if (!pending_push_) pending_push_ = std::move(push_payload);
            push_payload.reset();
        }

        if (push_payload) {
            bool ok = backend_->Write(*push_payload);
            if (ok) any_op_ok = true; else any_op_fail = true;
        }

        State new_status = status_.load();
        if (any_op_fail && !any_op_ok)      new_status = State::Offline;
        else if (any_op_ok && !any_op_fail) new_status = State::OK;
        status_.store(new_status);

        bool status_changed = last_signaled_status_.exchange(new_status) != new_status;
        if (delivered_pull || status_changed) Signal();

        first_iteration = false;
    }
}
