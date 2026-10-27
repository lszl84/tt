import AppKit

// tt.app's window and menu bar item. The data comes from the tt command line
// tool next to this executable in the bundle.
MainActor.assumeIsolated {
    let cli = Bundle.main.url(forAuxiliaryExecutable: "tt")
        ?? URL(fileURLWithPath: CommandLine.arguments[0])
            .deletingLastPathComponent().appendingPathComponent("tt")
    let app = NSApplication.shared
    let delegate = AppDelegate(store: Store(cli: cli))
    app.delegate = delegate
    withExtendedLifetime(delegate) { app.run() }
}
