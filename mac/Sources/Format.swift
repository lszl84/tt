import Foundation

// Durations the way the other tt frontends show them (see data.cpp and
// omarchy/Model.js), plus the large stopwatch of the window.
enum Format {
    private static func parts(_ seconds: Double) -> (h: Int, m: Int, s: Int) {
        let total = max(0, Int(seconds))
        return (total / 3600, total % 3600 / 60, total % 60)
    }

    private static func pad(_ n: Int) -> String {
        n < 10 ? "0\(n)" : "\(n)"
    }

    // "1h 02m 03s" / "2m 03s" / "12s".
    static func duration(_ seconds: Double) -> String {
        let (h, m, s) = parts(seconds)
        if h > 0 { return "\(h)h \(pad(m))m \(pad(s))s" }
        if m > 0 { return "\(m)m \(pad(s))s" }
        return "\(s)s"
    }

    // "1h 02m" / "12m".
    static func short(_ seconds: Double) -> String {
        let (h, m, _) = parts(seconds)
        return h > 0 ? "\(h)h \(pad(m))m" : "\(m)m"
    }

    // "1:02", next to the menu bar icon.
    static func clock(_ seconds: Double) -> String {
        let (h, m, _) = parts(seconds)
        return "\(h):\(pad(m))"
    }

    // "1:02:03" / "02:03".
    static func stopwatch(_ seconds: Double) -> String {
        let (h, m, s) = parts(seconds)
        return h > 0 ? "\(h):\(pad(m)):\(pad(s))" : "\(pad(m)):\(pad(s))"
    }

    // "14:02", in the user's time format.
    static func time(_ date: Date) -> String {
        date.formatted(date: .omitted, time: .shortened)
    }

    // The days a range covers: "Tuesday, Oct 27" or "Oct 19 – Nov 1".
    static func span(start: Date, end: Date) -> String {
        let last = end.addingTimeInterval(-1)
        if Calendar.current.isDate(start, inSameDayAs: last) {
            return start.formatted(.dateTime.weekday(.wide).month(.abbreviated).day())
        }
        let day = Date.FormatStyle.dateTime.month(.abbreviated).day()
        return "\(start.formatted(day)) – \(last.formatted(day))"
    }
}
