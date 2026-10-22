import QtQuick
import Quickshell
import Quickshell.Io
import "Backend.js" as Backend
import "Model.js" as Model

// Drives the tt executable; the widget never touches state.json itself.
// `tt status` reads the state, toggle/stop/add change it and print the new
// state, and `tt sync` pushes the data repo after a start or stop, like the
// window does. Commands run one at a time so replies land in order.
Item {
  id: root

  property string command: Backend.command
  property var status: null
  property double now: Date.now() / 1000
  property string message: ""
  property bool messageIsError: false

  readonly property bool loaded: status !== null
  readonly property var tasks: status && Array.isArray(status.tasks) ? status.tasks : []
  readonly property var ranges: status && Array.isArray(status.ranges) ? status.ranges : []
  readonly property int activeIndex: Model.activeIndex(tasks)
  readonly property bool running: activeIndex >= 0
  readonly property string activeName: running ? String(tasks[activeIndex].name || "") : ""
  readonly property double activeElapsed: running ? Math.max(0, now - Number(status.activeSince || 0)) : 0

  signal taskAdded(string taskId)

  property var _queue: []
  property var _job: null
  property string _output: ""
  property string _syncOutput: ""
  property bool _syncAgain: false

  // Seconds tracked on a task within a range, counting the running session
  // up to now.
  function secondsFor(rangeIndex, taskIndex) {
    var range = ranges[rangeIndex]
    if (!range || !Array.isArray(range.seconds)) return 0
    var seconds = Number(range.seconds[taskIndex] || 0)
    if (taskIndex !== activeIndex) return seconds
    var until = Math.min(now, Number(range.end || 0))
    return seconds + Math.max(0, until - Number(status.now || 0))
  }

  function totalFor(rangeIndex) {
    var total = 0
    for (var i = 0; i < tasks.length; i++) total += secondsFor(rangeIndex, i)
    return total
  }

  function refresh() { enqueue(["status"]) }
  function toggle(taskId) { if (taskId) enqueue(["toggle", String(taskId)]) }
  function stop() { if (running) enqueue(["stop"]) }

  function add(name) {
    var trimmed = String(name || "").trim()
    if (trimmed !== "") enqueue(["add", trimmed])
  }

  function setMessage(text, isError) {
    message = text
    messageIsError = isError
  }

  function enqueue(args) {
    // One read answers every refresh still waiting in the queue.
    if (args[0] === "status") {
      for (var i = 0; i < _queue.length; i++) if (_queue[i][0] === "status") return
    }
    _queue = _queue.concat([args])
    pump()
  }

  function pump() {
    if (commandProcess.running || _job !== null || _queue.length === 0) return
    _job = _queue[0]
    _queue = _queue.slice(1)
    _output = ""
    commandProcess.command = [command].concat(_job)
    commandProcess.running = true
  }

  function finish(raw) {
    var job = _job
    if (job === null) return
    _job = null

    var reply = Model.parseReply(raw)
    if (reply.status) {
      status = reply.status
      now = Date.now() / 1000
    }
    if (reply.ok === false) {
      setMessage(String(reply.error || "tt failed"), true)
    } else if (job[0] !== "status" && reply.message) {
      setMessage(String(reply.message), false)
      if (job[0] === "add" && reply.taskId) taskAdded(String(reply.taskId))
      if (reply.changed === true && (job[0] === "toggle" || job[0] === "stop") && status && status.git) sync()
    }
    Qt.callLater(pump)
  }

  function sync() {
    if (syncProcess.running) {
      _syncAgain = true
      return
    }
    _syncOutput = ""
    syncProcess.command = [command, "sync"]
    syncProcess.running = true
  }

  Process {
    id: commandProcess
    running: false
    command: []
    stdout: StdioCollector { id: commandStdout; waitForEnd: true; onStreamFinished: root._output = text }
    onExited: function(exitCode) { root.finish(String(commandStdout.text || root._output || "")) }
    onRunningChanged: if (!running) startWatchdog.restart()
  }

  // A command that cannot start never reports `exited`; don't let it wedge
  // the queue.
  Timer {
    id: startWatchdog
    interval: 1000
    onTriggered: if (!commandProcess.running && root._job !== null) root.finish("")
  }

  Process {
    id: syncProcess
    running: false
    command: []
    stdout: StdioCollector { id: syncStdout; waitForEnd: true; onStreamFinished: root._syncOutput = text }
    onExited: function(exitCode) {
      var reply = Model.parseReply(String(syncStdout.text || root._syncOutput || ""))
      if (reply.skipped !== true) root.setMessage(String(reply.message || reply.error || ""), reply.ok === false)
      if (root._syncAgain) {
        root._syncAgain = false
        Qt.callLater(root.sync)
      }
    }
  }

  // Picks up changes from anywhere: the widget on another monitor, the
  // window, or tt run in a terminal.
  FileView {
    path: root.status && root.status.dataPath ? String(root.status.dataPath) : ""
    watchChanges: true
    printErrors: false
    onFileChanged: {
      reload()
      changeDebounce.restart()
    }
  }

  Timer {
    id: changeDebounce
    interval: 250
    onTriggered: root.refresh()
  }

  Timer {
    // Running times tick every second.
    interval: 1000
    repeat: true
    running: root.running
    onTriggered: root.now = Date.now() / 1000
  }

  Timer {
    // Range boundaries (midnight, Monday) move without any file change.
    interval: 60000
    repeat: true
    running: true
    triggeredOnStart: true
    onTriggered: root.refresh()
  }
}
