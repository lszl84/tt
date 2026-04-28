#pragma once

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

class App;

// Backend-agnostic remote storage interface. All methods are called from the
// sync worker thread; implementations must be thread-safe with respect to
// themselves but not to other backends.
class SyncBackend {
public:
    virtual ~SyncBackend() = default;
    // Attempt a one-shot read of the remote file. Returns:
    //   - the JSON contents if the file exists
    //   - empty string if the remote is reachable but the file isn't there yet
    //   - std::nullopt if the remote is unreachable / errored
    virtual std::optional<std::string> Read() = 0;
    // Write the JSON contents to the remote. Returns true on success.
    virtual bool Write(const std::string& contents) = 0;
    // Human-readable description for the titlebar / logs.
    virtual std::string Description() const = 0;
};

// Probe the environment for an available backend. Order:
//   1. $TT_SYNC_PATH (any platform) — direct filesystem path to a target file.
//   2. macOS: ~/Library/CloudStorage/GoogleDrive-* / My Drive / tt-sync / data.json
//   3. Linux: gio-discovered Google Drive mount (lukasz@gmail → google-drive://...)
// Returns nullptr if nothing is configured/discoverable.
std::unique_ptr<SyncBackend> DiscoverBackend();

// Owns the worker thread. Pulls every 60s, pushes on demand. Single instance
// process-wide.
class SyncManager {
public:
    enum class State { Idle, Syncing, OK, Offline };

    SyncManager() = default;
    ~SyncManager();

    // Detect a backend and start the worker thread. Safe to call once after
    // App is constructed; idempotent.
    void Start(App& app);
    void Stop();

    // Main thread → worker: queue a snapshot to push (latest-wins).
    void RequestPush(std::string snapshot);

    // Worker → main thread: drain pulled remote JSON, if any. Returns true and
    // writes to `out` if a result was waiting; otherwise false.
    bool TryConsumePulled(std::string& out);

    State GetStatus() const { return status_.load(); }
    std::string Description() const;

    // Returns a POSIX file descriptor that becomes readable when sync state
    // changes (new pulled data or status update). Main loops can include this
    // in poll() so the UI wakes for remote changes during idle. -1 if disabled.
    int WakeupFd() const;

private:
    void Run();

    std::thread worker_;
    mutable std::mutex mu_;
    std::condition_variable cv_;
    bool started_ = false;
    bool stop_ = false;
    bool force_pull_ = true; // pull on first iteration

    std::unique_ptr<SyncBackend> backend_;
    std::string description_;

    // Push: latest snapshot wins (multiple rapid changes coalesce).
    std::optional<std::string> pending_push_;

    // Pull: latest read result. Drained by main thread.
    std::optional<std::string> pulled_;

    std::atomic<State> status_{State::Idle};
    std::atomic<State> last_signaled_status_{State::Idle};

    // Self-pipe used to wake the UI main loop when state changes.
    int wake_pipe_[2] = {-1, -1};
    void Signal();
};

// Process-wide singleton accessor.
SyncManager& GetSyncManager();
