import Foundation
import Observation

// What `tt status` prints. Seconds already include the running session up
// to `now`.
struct TTTask: Decodable, Identifiable, Equatable {
    let id: String
    let name: String
    let active: Bool
}

struct TTRange: Decodable, Equatable {
    let label: String
    let header: String
    let start: Double
    let end: Double
    let seconds: [Double]
}

struct TTStatus: Decodable, Equatable {
    let now: Double
    let dataPath: String
    let git: Bool
    let active: String?
    let activeSince: Double
    let tasks: [TTTask]
    let ranges: [TTRange]
}

private struct TTReply: Decodable {
    let ok: Bool
    let error: String?
    let message: String?
    let skipped: Bool?
    let status: TTStatus?
}

// The app's state: the last `tt status`, kept fresh by watching state.json.
// Like the Omarchy widget and the Xfce plugin, every change goes through the
// tt command line tool, so the time logic lives in data.cpp alone.
@MainActor
@Observable
final class Store {
    private(set) var status: TTStatus?
    var rangeIndex = 0
    private(set) var notice = ""
    private(set) var noticeIsError = false
    private(set) var syncing = false

    @ObservationIgnored private let cli: URL
    @ObservationIgnored private var modified: Date?
    @ObservationIgnored private var poll: Timer?

    init(cli: URL) {
        self.cli = cli
    }

    func start() {
        Task { await refresh() }
        // state.json is replaced on every save, so poll its date rather than
        // watch the file. This also catches `git pull`s and other tt copies.
        poll = Timer.scheduledTimer(withTimeInterval: 2, repeats: true) { [weak self] _ in
            MainActor.assumeIsolated { self?.checkForChanges() }
        }
    }

    // MARK: Derived state

    var tasks: [TTTask] { status?.tasks ?? [] }

    var activeIndex: Int? {
        guard let id = status?.active else { return nil }
        return tasks.firstIndex { $0.id == id }
    }

    var activeTask: TTTask? { activeIndex.map { tasks[$0] } }

    var activeSince: Date? {
        guard activeTask != nil, let since = status?.activeSince, since > 0 else { return nil }
        return Date(timeIntervalSince1970: since)
    }

    var range: TTRange? {
        guard let ranges = status?.ranges, ranges.indices.contains(rangeIndex) else { return nil }
        return ranges[rangeIndex]
    }

    var rangeCount: Int { status?.ranges.count ?? 0 }

    func elapsed(at date: Date) -> Double {
        guard let since = activeSince else { return 0 }
        return max(0, date.timeIntervalSince(since))
    }

    // Time for task `index` in range `rangeIndex`, the running session
    // counted up to `date`.
    func seconds(task index: Int, range rangeIndex: Int? = nil, at date: Date) -> Double {
        let r = rangeIndex ?? self.rangeIndex
        guard let status, status.ranges.indices.contains(r) else { return 0 }
        let range = status.ranges[r]
        guard range.seconds.indices.contains(index) else { return 0 }
        var seconds = range.seconds[index]
        if index == activeIndex, status.now >= range.start, status.now < range.end {
            seconds += max(0, date.timeIntervalSince1970 - status.now)
        }
        return seconds
    }

    func total(range rangeIndex: Int? = nil, at date: Date) -> Double {
        tasks.indices.reduce(0) { $0 + seconds(task: $1, range: rangeIndex, at: date) }
    }

    // MARK: Actions

    func toggle(_ task: TTTask) {
        Task { await change(["toggle", task.id]) }
    }

    func stop() {
        Task { await change(["stop"]) }
    }

    func add(_ name: String) {
        let name = name.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !name.isEmpty else { return }
        Task { await change(["add", name]) }
    }

    func refresh() async {
        if let reply = await run(["status"]), let status = reply.status {
            apply(status)
        }
    }

    // MARK: Plumbing

    private func change(_ args: [String]) async {
        guard let reply = await run(args) else { return }
        if let status = reply.status { apply(status) }
        if !reply.ok {
            show(reply.error ?? "tt \(args[0]) failed", error: true)
            return
        }
        await sync()
    }

    func syncNow() {
        Task { await sync() }
    }

    // Commit and push the data dir, as the window does after every change.
    private func sync() async {
        guard status?.git == true, !syncing else { return }
        syncing = true
        defer { syncing = false }
        guard let reply = await run(["sync"]) else { return }
        if reply.skipped != true {
            show(reply.ok ? (reply.message ?? "") : (reply.message ?? reply.error ?? "Sync failed"),
                 error: !reply.ok)
        }
    }

    private func apply(_ status: TTStatus) {
        if self.status != status { self.status = status }
        if rangeIndex >= status.ranges.count { rangeIndex = 0 }
        modified = Self.modificationDate(status.dataPath)
    }

    private func show(_ message: String, error: Bool) {
        notice = message
        noticeIsError = error
    }

    private func checkForChanges() {
        guard let status else {
            Task { await refresh() }
            return
        }
        // A new day (or week) moves the ranges.
        let rolledOver = status.ranges.first.map { Date().timeIntervalSince1970 >= $0.end } ?? false
        if rolledOver || Self.modificationDate(status.dataPath) != modified {
            Task { await refresh() }
        }
    }

    private static func modificationDate(_ path: String) -> Date? {
        (try? FileManager.default.attributesOfItem(atPath: path))?[.modificationDate] as? Date
    }

    private func run(_ args: [String]) async -> TTReply? {
        let cli = self.cli
        let result: Result<Data, Error> = await Task.detached {
            let process = Process()
            process.executableURL = cli
            process.arguments = args
            let output = Pipe()
            process.standardOutput = output
            process.standardError = FileHandle.nullDevice
            do {
                try process.run()
                let data = output.fileHandleForReading.readDataToEndOfFile()
                process.waitUntilExit()
                return .success(data)
            } catch {
                return .failure(error)
            }
        }.value

        switch result {
        case .failure(let error):
            show("Could not run tt: \(error.localizedDescription)", error: true)
            return nil
        case .success(let data):
            guard let reply = try? JSONDecoder().decode(TTReply.self, from: data) else {
                show("Unexpected output from tt", error: true)
                return nil
            }
            return reply
        }
    }
}
