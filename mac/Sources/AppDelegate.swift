import AppKit
import SwiftUI

// tt lives in the menu bar: a timer icon with the running task's time. A
// click hides the window if it is the one in use and otherwise brings it to
// the front on the current Space; a right-click offers Open Window, Stop and
// Quit. Closing the window leaves tt in the menu bar, and it has no Dock icon.
@MainActor
final class AppDelegate: NSObject, NSApplicationDelegate, NSWindowDelegate {
    // Shared with src/mac_main.cpp: `tt`, `tt --window` and `tt --popup`
    // send these to a running app.
    static let portName = "com.devmindscape.tt.app" as CFString
    static let showWindowMessage: Int32 = 1
    static let toggleWindowMessage: Int32 = 2

    private let store: Store
    private var window: NSWindow!
    private var statusItem: NSStatusItem!
    private var port: CFMessagePort?
    private var ticker: Timer?

    init(store: Store) {
        self.store = store
    }

    func applicationDidFinishLaunching(_ notification: Notification) {
        guard listenForCommands() else {
            // Another copy owns the menu bar item: bring its window instead.
            Self.send(Self.showWindowMessage)
            NSApp.terminate(nil)
            return
        }
        NSApp.setActivationPolicy(.accessory)
        NSApp.mainMenu = makeMainMenu()
        makeWindow()
        makeStatusItem()
        store.start()
        showWindow()
    }

    func applicationShouldHandleReopen(_ sender: NSApplication, hasVisibleWindows: Bool) -> Bool {
        showWindow()
        return false
    }

    func windowShouldClose(_ sender: NSWindow) -> Bool {
        sender.orderOut(nil)
        return false
    }

    // MARK: Window

    private func makeWindow() {
        let window = NSWindow(
            contentRect: NSRect(x: 0, y: 0, width: 380, height: 600),
            styleMask: [.titled, .closable, .miniaturizable, .resizable, .fullSizeContentView],
            backing: .buffered, defer: false)
        window.toolbarStyle = .unified
        window.isMovableByWindowBackground = true
        window.isOpaque = false
        window.backgroundColor = .clear
        window.isReleasedWhenClosed = false
        window.delegate = self
        // Showing the window brings it to the current Space instead of
        // switching to the Space it was last on.
        window.collectionBehavior.insert(.moveToActiveSpace)
        // The hosting controller sizes the window (its height follows the
        // content, its width is free above the minimum) and turns the view's
        // title and toolbar into the window's.
        let content = NSHostingController(rootView: ContentView().environment(store))
        content.sceneBridgingOptions = [.title, .toolbars]
        window.contentViewController = content
        if !window.setFrameUsingName("TimeTracker") {
            window.center()
        }
        window.setFrameAutosaveName("TimeTracker")
        self.window = window
    }

    private func showWindow() {
        // A window still showing on another Space stays there when ordered
        // front; hiding it first makes it reappear on this one.
        if window.isVisible && !window.isOnActiveSpace {
            window.orderOut(nil)
        }
        if window.isMiniaturized { window.deminiaturize(nil) }
        // Plain activate() is only a request since macOS 14, and it is
        // refused while another app is active: the window came up behind that
        // app's windows, unfocused. The older call still takes focus for a
        // menu bar app. Raising the window above normal windows and back puts
        // it in front even if that ever stops working.
        NSApp.activate(ignoringOtherApps: true)
        window.makeKeyAndOrderFront(nil)
        window.level = .floating
        window.orderFrontRegardless()
        window.level = .normal
    }

    // Hide the window if it is in front of every other window on this
    // Space. Otherwise (hidden, behind other windows, on another Space)
    // bring it to the front here.
    private func toggleWindow() {
        let inFront = window.isVisible && !window.isMiniaturized && window.isOnActiveSpace
            && isFrontmostWindow()
        if inFront {
            window.orderOut(nil)
        } else {
            showWindow()
        }
    }

    // Whether the window is the frontmost normal window on screen, of any
    // app. tt need not be the active app for that (see showWindow).
    private func isFrontmostWindow() -> Bool {
        let options: CGWindowListOption = [.optionOnScreenOnly, .excludeDesktopElements]
        guard let windows = CGWindowListCopyWindowInfo(options, kCGNullWindowID)
                as? [[String: Any]] else { return false }
        let front = windows.first { ($0[kCGWindowLayer as String] as? Int) == 0 }
        return (front?[kCGWindowNumber as String] as? Int) == window.windowNumber
    }

    // An accessory app shows no menu bar, but its menus still carry the
    // standard key equivalents: ⌘Q, ⌘W and editing in the text field.
    private func makeMainMenu() -> NSMenu {
        let main = NSMenu()

        let app = NSMenu()
        app.addItem(withTitle: "Quit tt", action: #selector(NSApplication.terminate(_:)),
                    keyEquivalent: "q")
        main.addItem(withTitle: "tt", action: nil, keyEquivalent: "").submenu = app

        let edit = NSMenu(title: "Edit")
        edit.addItem(withTitle: "Undo", action: Selector(("undo:")), keyEquivalent: "z")
        edit.addItem(withTitle: "Redo", action: Selector(("redo:")), keyEquivalent: "Z")
        edit.addItem(.separator())
        edit.addItem(withTitle: "Cut", action: #selector(NSText.cut(_:)), keyEquivalent: "x")
        edit.addItem(withTitle: "Copy", action: #selector(NSText.copy(_:)), keyEquivalent: "c")
        edit.addItem(withTitle: "Paste", action: #selector(NSText.paste(_:)), keyEquivalent: "v")
        edit.addItem(withTitle: "Select All", action: #selector(NSText.selectAll(_:)),
                     keyEquivalent: "a")
        main.addItem(withTitle: "Edit", action: nil, keyEquivalent: "").submenu = edit

        let windowMenu = NSMenu(title: "Window")
        windowMenu.addItem(withTitle: "Close", action: #selector(NSWindow.performClose(_:)),
                           keyEquivalent: "w")
        windowMenu.addItem(withTitle: "Minimize", action: #selector(NSWindow.miniaturize(_:)),
                           keyEquivalent: "m")
        main.addItem(withTitle: "Window", action: nil, keyEquivalent: "").submenu = windowMenu
        return main
    }

    // MARK: Menu bar item

    private func makeStatusItem() {
        statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
        guard let button = statusItem.button else { return }
        button.imagePosition = .imageLeading
        button.font = .monospacedDigitSystemFont(ofSize: NSFont.systemFontSize, weight: .regular)
        button.target = self
        button.action = #selector(statusItemClicked(_:))
        button.sendAction(on: [.leftMouseUp, .rightMouseUp])
        updateStatusItem()
        ticker = Timer.scheduledTimer(withTimeInterval: 1, repeats: true) { [weak self] _ in
            MainActor.assumeIsolated { self?.updateStatusItem() }
        }
    }

    // The icon and the time are template content, so the menu bar colors
    // them like its other items in light and dark mode.
    private func updateStatusItem() {
        guard let button = statusItem.button else { return }
        let now = Date()
        let task = store.activeTask
        let symbol = task != nil ? "stopwatch.fill" : "stopwatch"
        if button.image?.name() != symbol {
            let image = NSImage(systemSymbolName: symbol, accessibilityDescription: "Time Tracker")
            image?.setName(symbol)
            image?.isTemplate = true
            button.image = image
        }
        let title = task != nil ? Format.clock(store.elapsed(at: now)) : ""
        if button.title != title {
            button.title = title
            button.imagePosition = title.isEmpty ? .imageOnly : .imageLeading
        }

        var tooltip = task.map { "\($0.name) · \(Format.duration(store.elapsed(at: now)))" }
            ?? "Not tracking"
        tooltip += "\n\(Format.short(store.total(range: 0, at: now))) today"
        button.toolTip = tooltip
        button.setAccessibilityLabel(task.map {
            "Time Tracker, \($0.name), \(Format.clock(store.elapsed(at: now)))"
        } ?? "Time Tracker, not tracking")
    }

    @objc private func statusItemClicked(_ sender: NSStatusBarButton) {
        let event = NSApp.currentEvent
        if event?.type == .rightMouseUp || event?.modifierFlags.contains(.control) == true {
            showContextMenu()
        } else {
            toggleWindow()
        }
    }

    // Without a Dock icon this is the way to quit while the window is hidden.
    private func showContextMenu() {
        let menu = NSMenu()
        menu.addItem(withTitle: "Open Window", action: #selector(openWindow), keyEquivalent: "")
            .target = self
        if let task = store.activeTask {
            menu.addItem(withTitle: "Stop \(task.name)", action: #selector(stopTask),
                         keyEquivalent: "").target = self
        }
        menu.addItem(.separator())
        menu.addItem(withTitle: "Quit tt", action: #selector(NSApplication.terminate(_:)),
                     keyEquivalent: "q")
        statusItem.menu = menu
        statusItem.button?.performClick(nil)
        statusItem.menu = nil  // a left click toggles the window again
    }

    @objc private func openWindow() { showWindow() }
    @objc private func stopTask() { store.stop() }

    // MARK: Commands from `tt`

    // Owns the port that `tt` sends to. False if another copy already does.
    private func listenForCommands() -> Bool {
        var context = CFMessagePortContext(version: 0, info: nil, retain: nil, release: nil,
                                           copyDescription: nil)
        let callback: CFMessagePortCallBack = { _, message, _, _ in
            MainActor.assumeIsolated {
                (NSApp.delegate as? AppDelegate)?.handle(message)
            }
            return nil
        }
        guard let port = CFMessagePortCreateLocal(nil, Self.portName, callback, &context, nil)
        else { return false }
        let source = CFMessagePortCreateRunLoopSource(nil, port, 0)
        CFRunLoopAddSource(CFRunLoopGetMain(), source, .commonModes)
        self.port = port
        return true
    }

    private func handle(_ message: Int32) {
        switch message {
        case Self.toggleWindowMessage: toggleWindow()
        default: showWindow()
        }
    }

    private static func send(_ message: Int32) {
        guard let remote = CFMessagePortCreateRemote(nil, portName) else { return }
        CFMessagePortSendRequest(remote, message, nil, 2, 0, nil, nil)
    }
}
