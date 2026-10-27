import AppKit
import SwiftUI

// Layout grid: cards sit 16pt from the window edge and their content 18pt
// inside them, so every leading and trailing edge in the window lines up.
private enum Grid {
    static let margin: CGFloat = 16
    static let inset: CGFloat = 18
    static let gap: CGFloat = 12
    static let cardRadius: CGFloat = 24
    static let listPadding: CGFloat = 6  // list card to its rows
    static let maxListHeight: CGFloat = 540
}

struct ContentView: View {
    @Environment(Store.self) private var store
    @State private var addingTask = false

    var body: some View {
        // One tick a second drives every running clock in the window.
        TimelineView(.periodic(from: .now, by: 1)) { context in
            VStack(spacing: Grid.gap) {
                NowCard(now: context.date)
                RangeHeader(now: context.date)
                TaskList(now: context.date)
            }
            .padding([.horizontal, .bottom], Grid.margin)
            .padding(.top, 4)
        }
        // The window is as tall as its content.
        .fixedSize(horizontal: false, vertical: true)
        // The narrowest the toolbar fits its title and both buttons.
        .frame(minWidth: 360)
        .background(WindowGlass().ignoresSafeArea())
        .animation(.smooth(duration: 0.35), value: store.status?.active)
        .navigationTitle("Time Tracker")
        .toolbar {
            ToolbarItemGroup(placement: .primaryAction) {
                if store.status?.git == true {
                    SyncButton()
                }
                Button {
                    addingTask = true
                } label: {
                    Label("New Task", systemImage: "plus")
                }
                .keyboardShortcut("n", modifiers: .command)
                .help("New task (⌘N)")
                .popover(isPresented: $addingTask, arrowEdge: .bottom) {
                    NewTaskPopover()
                }
            }
        }
        .labelStyle(.iconOnly)
    }
}

// Commits and pushes the data dir now; shows the last result on hover.
private struct SyncButton: View {
    @Environment(Store.self) private var store

    var body: some View {
        Button {
            store.syncNow()
        } label: {
            if store.syncing {
                ProgressView().controlSize(.small)
            } else {
                Label("Sync", systemImage: store.noticeIsError ? "exclamationmark.icloud"
                                                               : "arrow.clockwise.icloud")
            }
        }
        .disabled(store.syncing)
        .help(store.notice.isEmpty ? "Sync with git" : store.notice)
    }
}

// MARK: - Running task

private struct NowCard: View {
    @Environment(Store.self) private var store
    let now: Date

    var body: some View {
        let color = store.activeIndex.map(TaskColor.at)
        VStack(alignment: .leading, spacing: 10) {
            HStack(spacing: 7) {
                if let color {
                    PulseDot(color: color)
                }
                Text(color != nil ? "TRACKING" : "NOT TRACKING")
                    .font(.caption2.weight(.bold))
                    .tracking(1.4)
                Spacer()
                if let since = store.activeSince {
                    Text("since \(Format.time(since))")
                        .font(.caption)
                } else {
                    Text("today")
                        .font(.caption)
                }
            }
            .foregroundStyle(.secondary)

            HStack(alignment: .center, spacing: 12) {
                VStack(alignment: .leading, spacing: 2) {
                    Text(store.activeTask?.name
                         ?? (store.tasks.isEmpty ? "Add a task to start" : "Pick a task to start"))
                        .font(.title3.weight(.semibold))
                        .foregroundStyle(store.activeTask != nil ? .primary : .secondary)
                        .lineLimit(1)
                    Text(store.activeTask != nil
                         ? Format.stopwatch(store.elapsed(at: now))
                         : Format.short(store.total(range: 0, at: now)))
                        .font(.system(size: 48, weight: .semibold, design: .rounded))
                        .monospacedDigit()
                        .contentTransition(.numericText())
                        .animation(.snappy, value: Int(store.elapsed(at: now)))
                        .lineLimit(1)
                        .minimumScaleFactor(0.6)
                        // Trim the font's side bearing and line spacing so the
                        // digits line up with the text above and the card's
                        // padding looks even.
                        .padding(.leading, -1)
                        .padding(.vertical, -6)
                }
                Spacer(minLength: 0)
                if let task = store.activeTask {
                    Button {
                        store.stop()
                    } label: {
                        Image(systemName: "stop.fill")
                            .font(.system(size: 17, weight: .semibold))
                            .frame(width: 40, height: 40)
                    }
                    .buttonStyle(.glass)
                    .buttonBorderShape(.circle)
                    .keyboardShortcut(".", modifiers: .command)
                    .help("Stop \(task.name) (⌘.)")
                    .transition(.scale.combined(with: .opacity))
                }
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(Grid.inset)
        .background {
            // The running task's colour, washing in from the top.
            if let color {
                LinearGradient(colors: [color.opacity(0.42), color.opacity(0.06)],
                               startPoint: .topLeading, endPoint: .bottomTrailing)
                    .clipShape(.rect(cornerRadius: Grid.cardRadius))
            }
        }
        .glassEffect(.regular, in: .rect(cornerRadius: Grid.cardRadius))
    }
}

private struct PulseDot: View {
    let color: Color
    @State private var on = false

    var body: some View {
        Circle()
            .fill(color)
            .frame(width: 7, height: 7)
            .shadow(color: color.opacity(0.9), radius: on ? 4 : 0)
            .opacity(on ? 1 : 0.5)
            .onAppear {
                withAnimation(.easeInOut(duration: 1).repeatForever()) { on = true }
            }
    }
}

// MARK: - Range

private struct RangeHeader: View {
    @Environment(Store.self) private var store
    let now: Date

    var body: some View {
        HStack(alignment: .center, spacing: 12) {
            if let range = store.range {
                VStack(alignment: .leading, spacing: 1) {
                    Text(range.label)
                        .font(.headline)
                    Text(Format.span(start: Date(timeIntervalSince1970: range.start),
                                     end: Date(timeIntervalSince1970: range.end)))
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
                Spacer()
                Text(Format.short(store.total(at: now)))
                    .font(.headline)
                    .monospacedDigit()
                    .contentTransition(.numericText())
                GlassEffectContainer(spacing: 6) {
                    HStack(spacing: 6) {
                        step("chevron.left", by: -1, key: "[")
                        step("chevron.right", by: 1, key: "]")
                    }
                }
            }
        }
        .padding(.horizontal, Grid.inset)
        .padding(.vertical, 2)
    }

    private func step(_ symbol: String, by delta: Int, key: KeyEquivalent) -> some View {
        let target = store.rangeIndex + delta
        return Button {
            withAnimation(.snappy) { store.rangeIndex = target }
        } label: {
            Image(systemName: symbol)
                .font(.system(size: 11, weight: .bold))
                .frame(width: 16, height: 16)
        }
        .buttonStyle(.glass)
        .buttonBorderShape(.circle)
        .disabled(target < 0 || target >= store.rangeCount)
        .keyboardShortcut(key, modifiers: .command)
    }
}

// MARK: - Tasks

private struct TaskList: View {
    @Environment(Store.self) private var store
    let now: Date
    @State private var contentHeight: CGFloat = 0

    var body: some View {
        let seconds = store.tasks.indices.map { store.seconds(task: $0, at: now) }
        let longest = max(seconds.max() ?? 0, 1)

        Group {
            if store.status != nil && store.tasks.isEmpty {
                ContentUnavailableView("No tasks yet", systemImage: "timer",
                                       description: Text("Add one with + in the toolbar."))
                    .frame(height: 160)
            } else {
                // As tall as the rows, scrolling past a dozen or so.
                ScrollView {
                    VStack(spacing: 2) {
                        ForEach(Array(store.tasks.enumerated()), id: \.element.id) { index, task in
                            TaskRow(task: task,
                                    color: TaskColor.at(index),
                                    seconds: seconds[index],
                                    fraction: seconds[index] / longest,
                                    active: index == store.activeIndex) {
                                store.toggle(task)
                            }
                        }
                    }
                    .padding(Grid.listPadding)
                    .onGeometryChange(for: CGFloat.self) { $0.size.height } action: {
                        contentHeight = $0
                    }
                }
                .scrollBounceBehavior(.basedOnSize)
                .frame(height: min(contentHeight, Grid.maxListHeight))
            }
        }
        .glassEffect(.regular, in: .rect(cornerRadius: Grid.cardRadius))
    }
}

private struct TaskRow: View {
    let task: TTTask
    let color: Color
    let seconds: Double
    let fraction: Double
    let active: Bool
    let toggle: () -> Void

    @State private var hovering = false

    private static let markerWidth: CGFloat = 22
    private static let spacing: CGFloat = 10
    // Rows sit `listPadding` inside the card; this brings their content to
    // the grid inset.
    private static let padding = Grid.inset - Grid.listPadding

    var body: some View {
        Button(action: toggle) {
            VStack(alignment: .leading, spacing: 4) {
                HStack(spacing: Self.spacing) {
                    Marker(color: color, active: active, hovering: hovering)
                    Text(task.name)
                        .fontWeight(active ? .semibold : .regular)
                        .lineLimit(1)
                    Spacer(minLength: 8)
                    Text(seconds >= 1 ? Format.duration(seconds) : "–")
                        .monospacedDigit()
                        .foregroundStyle(seconds >= 1 ? .primary : .tertiary)
                        .contentTransition(.numericText())
                }
                Bar(fraction: fraction, color: color)
                    .padding(.leading, Self.markerWidth + Self.spacing)
            }
            .padding(.horizontal, Self.padding)
            .padding(.top, 5)
            .padding(.bottom, 7)
            .background {
                RoundedRectangle(cornerRadius: Grid.cardRadius - Grid.listPadding,
                                 style: .continuous)
                    .fill(active ? AnyShapeStyle(color.opacity(0.16))
                                 : hovering ? AnyShapeStyle(.quaternary.opacity(0.7))
                                            : AnyShapeStyle(Color.clear))
            }
            .contentShape(.rect(cornerRadius: Grid.cardRadius - Grid.listPadding))
        }
        .buttonStyle(.plain)
        .onHover { hovering = $0 }
        .animation(.easeOut(duration: 0.15), value: hovering)
        .help(active ? "Stop \(task.name)" : "Start \(task.name)")
    }
}

// The task's colour dot, a play button under the pointer and a stop button
// while the task runs.
private struct Marker: View {
    let color: Color
    let active: Bool
    let hovering: Bool

    var body: some View {
        let button = active || hovering
        // The dot starts at the column's leading edge, in line with the
        // pulsing dot of the card above, and grows into the button.
        Circle()
            .fill(active ? AnyShapeStyle(color.gradient)
                         : button ? AnyShapeStyle(color.opacity(0.22))
                                  : AnyShapeStyle(color.gradient))
            .frame(width: button ? 22 : 8, height: button ? 22 : 8)
            .overlay {
                Image(systemName: active ? "stop.fill" : "play.fill")
                    .font(.system(size: 8, weight: .heavy))
                    .foregroundStyle(active ? AnyShapeStyle(.black.opacity(0.75))
                                            : AnyShapeStyle(color))
                    .opacity(button ? 1 : 0)
            }
            .frame(width: 22, height: 22, alignment: .leading)
        .animation(.snappy(duration: 0.2), value: button)
    }
}

// The task's share of the range, relative to the longest task, on a faint
// full-width track.
private struct Bar: View {
    let fraction: Double
    let color: Color

    var body: some View {
        GeometryReader { geometry in
            ZStack(alignment: .leading) {
                Capsule().fill(.primary.opacity(0.06))
                Capsule()
                    .fill(color.gradient)
                    .frame(width: fraction > 0 ? max(4, geometry.size.width * fraction) : 0)
            }
        }
        .frame(height: 4)
        .animation(.smooth, value: fraction)
    }
}

// A colour per task, by its place in the list: tasks are only ever added at
// the end, so a task keeps its colour. One saturation and brightness for all
// hues, ordered so neighbours differ; a little deeper in light mode.
enum TaskColor {
    private static let hues: [Double] = [
        0.58, 0.07, 0.44, 0.94, 0.74, 0.12, 0.51, 0.66, 0.33, 0.86, 0.54, 0.02,
    ]

    private static let palette: [Color] = hues.map { hue in
        Color(nsColor: NSColor(name: nil) { appearance in
            let dark = appearance.bestMatch(from: [.darkAqua, .aqua]) == .darkAqua
            return NSColor(hue: hue, saturation: dark ? 0.52 : 0.62,
                           brightness: dark ? 0.94 : 0.80, alpha: 1)
        })
    }

    static func at(_ index: Int) -> Color {
        palette[index % palette.count]
    }
}

// MARK: - Adding

// Opened from + in the toolbar. Return adds the task, Escape cancels.
private struct NewTaskPopover: View {
    @Environment(Store.self) private var store
    @Environment(\.dismiss) private var dismiss
    @State private var name = ""
    @FocusState private var focused: Bool

    var body: some View {
        let valid = !name.trimmingCharacters(in: .whitespaces).isEmpty
        VStack(alignment: .leading, spacing: 12) {
            Text("New Task")
                .font(.headline)
            TextField("Name", text: $name)
                .textFieldStyle(.roundedBorder)
                .focused($focused)
                .onSubmit(add)
            HStack {
                Spacer()
                Button("Cancel", role: .cancel) { dismiss() }
                    .keyboardShortcut(.cancelAction)
                Button("Add", action: add)
                    .buttonStyle(.glassProminent)
                    .disabled(!valid)
            }
        }
        .padding(Grid.margin)
        .frame(width: 260)
        .onAppear { focused = true }
    }

    private func add() {
        guard !name.trimmingCharacters(in: .whitespaces).isEmpty else { return }
        store.add(name)
        dismiss()
    }
}

// MARK: - Window background

// The desktop shows through the window, blurred; the cards on top are glass.
private struct WindowGlass: NSViewRepresentable {
    func makeNSView(context: Context) -> NSVisualEffectView {
        let view = NSVisualEffectView()
        view.material = .underWindowBackground
        view.blendingMode = .behindWindow
        view.state = .active
        return view
    }

    func updateNSView(_ view: NSVisualEffectView, context: Context) {}
}
