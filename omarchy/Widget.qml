import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Quickshell
import Quickshell.Io
import qs.Commons
import qs.Commons as Commons
import qs.Ui
import "Model.js" as Model

// tt in the bar: an icon (plus the running time while tracking) and a
// dropdown with what the window offers: add tasks, start/stop them, and page
// through the same summary ranges.
Panel {
  id: root
  moduleName: "com.devmindscape.tt"
  // `omarchy-shell shell summon com.devmindscape.tt` finds the widget by id;
  // an IPC target per instance would clash with one bar per monitor.
  manageIpc: false

  property string focusSection: "tasks"  // "input", "tasks" or "range"
  property int taskIndex: 0
  property bool cursorActive: false
  property int rangeIndex: 0

  readonly property string idleGlyph: String.fromCodePoint(0xF13AB)     // timer outline
  readonly property string runningGlyph: String.fromCodePoint(0xF051B)  // timer
  readonly property string startGlyph: String.fromCodePoint(0xF040A)
  readonly property string stopGlyph: String.fromCodePoint(0xF04DB)

  readonly property color foreground: bar ? bar.foreground : Commons.Color.foreground
  readonly property color urgent: bar ? bar.urgent : Commons.Color.urgent
  readonly property color dim: Qt.darker(foreground, 1.55)
  readonly property string fontFamily: bar ? bar.fontFamily : Style.font.family
  // `omarchy bar set` without --json stores "false" as a string.
  readonly property bool showElapsed: String(setting("showElapsed", true)) !== "false"
  readonly property bool labelMode: tt.running && showElapsed && !(bar && bar.vertical)
  readonly property Item barButton: labelMode ? labelButton : iconButton
  readonly property var range: tt.ranges[rangeIndex] || null
  // Task indices with time in the selected range, longest first. Ordered by
  // the last status read so rows don't reshuffle every second.
  readonly property var reportRows: buildReportRows()

  function buildReportRows() {
    var r = tt.ranges[rangeIndex]
    if (!r || !Array.isArray(r.seconds)) return []
    var readAt = Number(tt.status ? tt.status.now : 0)
    var live = readAt >= Number(r.start) && readAt < Number(r.end)
    var rows = []
    for (var i = 0; i < tt.tasks.length; i++) {
      var seconds = Number(r.seconds[i] || 0)
      if (seconds > 0 || (live && i === tt.activeIndex)) rows.push({ index: i, seconds: seconds })
    }
    rows.sort(function(a, b) { return b.seconds - a.seconds })
    return rows.map(function(row) { return row.index })
  }

  function rangeSpan(r) {
    if (!r) return ""
    var start = new Date(Number(r.start) * 1000)
    var last = new Date((Number(r.end) - 1) * 1000)
    if (start.toDateString() === last.toDateString()) return Qt.formatDate(start, "dddd, d MMM")
    return Qt.formatDate(start, "d MMM") + " – " + Qt.formatDate(last, "d MMM")
  }

  function stepRange(step) {
    var count = Math.max(1, tt.ranges.length)
    rangeIndex = (rangeIndex + step + count) % count
  }

  function toggleTask(index) {
    var task = tt.tasks[index]
    if (task) tt.toggle(task.id)
  }

  function focusNameField() {
    cursorActive = true
    focusSection = "input"
    if (panelFlick) panelFlick.contentY = 0
    nameField.forceActiveFocus()
  }

  function addTask() {
    var name = nameField.text.trim()
    if (name === "") return
    tt.add(name)
    nameField.text = ""
    releaseNameField()
  }

  // Hand the keys back to the panel.
  function releaseNameField() {
    nameField.focus = false
    keyCatcher.forceActiveFocus()
  }

  function ensureCursor() {
    if (focusSection === "tasks" && tt.tasks.length === 0) focusSection = "input"
    if (taskIndex >= tt.tasks.length) taskIndex = Math.max(0, tt.tasks.length - 1)
    if (taskIndex < 0) taskIndex = 0
  }

  function moveCursor(dx, dy) {
    ensureCursor()
    if (dx !== 0) {
      stepRange(dx)
      return
    }
    if (focusSection === "input") {
      if (dy > 0) setTaskCursor(0)
    } else if (focusSection === "tasks") {
      var next = taskIndex + dy
      if (next < 0) {
        focusSection = "input"
        if (panelFlick) panelFlick.contentY = 0
      } else if (next >= tt.tasks.length) {
        setRangeCursor()
      } else {
        setTaskCursor(next)
      }
    } else if (focusSection === "range" && dy < 0) {
      if (tt.tasks.length > 0) setTaskCursor(tt.tasks.length - 1)
      else focusSection = "input"
    }
  }

  function activateCursor() {
    ensureCursor()
    if (focusSection === "input") focusNameField()
    else if (focusSection === "tasks") toggleTask(taskIndex)
    else if (focusSection === "range") stepRange(1)
  }

  function setTaskCursor(index) {
    if (index < 0 || index >= tt.tasks.length) {
      if (tt.tasks.length === 0) focusSection = "input"
      return
    }
    cursorActive = true
    focusSection = "tasks"
    taskIndex = index
    if (index < taskColumn.children.length) scrollItemIntoView(taskColumn.children[index])
  }

  function setRangeCursor() {
    cursorActive = true
    focusSection = "range"
    scrollItemIntoView(rangeHeader)
  }

  function scrollItemIntoView(item) {
    if (!panelFlick || !item) return
    Qt.callLater(function() {
      if (!item) return
      var margin = Style.space(6)
      var top = item.mapToItem(panelFlick.contentItem, 0, 0).y
      var bottom = top + item.height
      var maxY = Math.max(0, panelFlick.contentHeight - panelFlick.height)
      if (top < panelFlick.contentY + margin) panelFlick.contentY = Math.max(0, top - margin)
      else if (bottom > panelFlick.contentY + panelFlick.height - margin)
        panelFlick.contentY = Math.min(maxY, bottom + margin - panelFlick.height)
    })
  }

  function handleBarPress(button) {
    if (button === Qt.RightButton) tt.refresh()
    else root.toggle()
  }

  implicitWidth: barButton.implicitWidth
  implicitHeight: barButton.implicitHeight

  onOpenedChanged: if (opened) {
    cursorActive = false
    focusSection = "tasks"
    taskIndex = tt.activeIndex >= 0 ? tt.activeIndex : 0
    if (panelFlick) panelFlick.contentY = 0
    tt.refresh()
    Qt.callLater(function() { keyCatcher.forceActiveFocus() })
  }

  Service {
    id: tt
  }

  // tt installs each version of this code in its own directory; after an
  // update it waits for the new code to settle in before opening the
  // dropdown. The token tells reloaded instances apart.
  readonly property string instanceToken: Date.now() + "-" + Math.random()

  IpcHandler {
    target: "com.devmindscape.tt"
    function revision(): string { return Qt.resolvedUrl(".") + " " + root.instanceToken }
  }

  Connections {
    target: tt
    function onTaskAdded(taskId) {
      for (var i = 0; i < tt.tasks.length; i++) {
        if (tt.tasks[i].id === taskId) {
          root.setTaskCursor(i)
          return
        }
      }
    }
  }

  BarIconButton {
    id: iconButton
    anchors.fill: parent
    visible: !root.labelMode
    bar: root.bar
    text: tt.running ? root.runningGlyph : root.idleGlyph
    foreground: tt.running ? root.barForeground : Qt.darker(root.barForeground, 1.55)
    onPressed: function(button) { root.handleBarPress(button) }
  }

  WidgetButton {
    id: labelButton
    anchors.fill: parent
    visible: root.labelMode
    bar: root.bar
    text: root.runningGlyph + " " + Model.formatClock(tt.activeElapsed)
    onPressed: function(button) { root.handleBarPress(button) }
  }

  KeyboardPanel {
    id: panel
    anchorItem: root.barButton
    owner: root
    bar: root.bar
    open: root.opened
    focusTarget: keyCatcher
    contentWidth: panel.fittedContentWidth(Style.space(360))
    contentHeight: panel.fittedContentHeight(column.implicitHeight, Style.space(640))

    PanelKeyCatcher {
      id: keyCatcher
      anchors.fill: parent
      blocked: nameField.activeFocus
      onMoveRequested: function(dx, dy) {
        if (!root.cursorActive) { root.cursorActive = true; root.ensureCursor(); return }
        root.moveCursor(dx, dy)
      }
      onActivateRequested: {
        if (!root.cursorActive) { root.cursorActive = true; root.ensureCursor(); return }
        root.activateCursor()
      }
      onCloseRequested: root.close()
      onTabRequested: function(direction) { root.switchPanel(direction) }
      onTextKey: function(t) {
        if (t === "n" || t === "N" || t === "a" || t === "A") root.focusNameField()
        else if (t === "s" || t === "S") tt.stop()
        else if (t === "r" || t === "R") tt.refresh()
      }

      Flickable {
        id: panelFlick
        anchors.fill: parent
        contentWidth: width
        contentHeight: column.implicitHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        flickableDirection: Flickable.VerticalFlick
        interactive: contentHeight > height
        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

        Column {
          id: column
          width: panelFlick.width
          spacing: Style.space(12)

          PanelHero {
            id: hero
            width: parent.width
            title: "Time Tracker"
            meta: tt.running
              ? tt.activeName + " · " + Model.formatDuration(tt.activeElapsed)
              : (tt.loaded ? "Not tracking" : "Loading")
            detail: tt.loaded ? Model.formatShort(tt.totalFor(0)) + " today" : ""
            foreground: root.foreground
            fontFamily: root.fontFamily
            // Components handed to PanelHero resolve `root` to the hero, so
            // they reach panel state through `hero` and `tt`.
            iconComponent: Component {
              Text {
                textFormat: Text.PlainText
                text: String.fromCodePoint(tt.running ? 0xF051B : 0xF13AB)
                color: hero.foreground
                opacity: tt.running ? 1.0 : 0.6
                font.family: hero.fontFamily
                font.pixelSize: Style.font.display
              }
            }
            trailingControl: Component {
              PanelActionButton {
                visible: tt.running
                iconText: String.fromCodePoint(0xF04DB)
                tooltipText: "Stop"
                foreground: hero.foreground
                fontFamily: hero.fontFamily
                onClicked: tt.stop()
              }
            }
          }

          Text {
            textFormat: Text.PlainText
            visible: tt.message !== ""
            width: parent.width
            text: tt.message
            color: tt.messageIsError ? root.urgent : root.dim
            font.family: root.fontFamily
            font.pixelSize: Style.font.bodySmall
            wrapMode: Text.WordWrap
          }

          Item {
            width: parent.width
            implicitHeight: nameField.implicitHeight

            TextField {
              id: nameField
              anchors.left: parent.left
              anchors.right: addButton.left
              anchors.rightMargin: Style.space(6)
              anchors.verticalCenter: parent.verticalCenter
              placeholderText: "New task name…"
              foreground: root.foreground
              font.family: root.fontFamily
              hasCursor: root.cursorActive && root.focusSection === "input"
              onHoveredChanged: if (hovered) { root.cursorActive = true; root.focusSection = "input" }
              // Accept the keys here: TextInput lets Return propagate after
              // `accepted`, and the panel would read it as "focus the field".
              Keys.onReturnPressed: function(event) { event.accepted = true; root.addTask() }
              Keys.onEnterPressed: function(event) { event.accepted = true; root.addTask() }
              Keys.onEscapePressed: function(event) { event.accepted = true; root.releaseNameField() }
            }

            PanelActionButton {
              id: addButton
              anchors.right: parent.right
              anchors.verticalCenter: parent.verticalCenter
              iconText: String.fromCodePoint(0xF0415)
              tooltipText: "Add task"
              foreground: root.foreground
              fontFamily: root.fontFamily
              enabled: nameField.text.trim() !== ""
              onClicked: root.addTask()
            }
          }

          Column {
            width: parent.width
            spacing: Style.space(6)

            PanelSectionHeader {
              text: "TASKS · TODAY"
              foreground: root.foreground
              fontFamily: root.fontFamily
            }

            Text {
              textFormat: Text.PlainText
              visible: tt.loaded && tt.tasks.length === 0
              width: parent.width
              text: "No tasks yet. Add one above."
              color: root.dim
              font.family: root.fontFamily
              font.pixelSize: Style.font.body
              horizontalAlignment: Text.AlignHCenter
            }

            Column {
              id: taskColumn
              width: parent.width
              spacing: Style.space(4)

              Repeater {
                model: tt.tasks
                TaskRow {
                  required property var modelData
                  required property int index
                  width: taskColumn.width
                  task: modelData
                  rowIndex: index
                }
              }
            }
          }

          PanelSeparator {
            foreground: root.foreground
          }

          Column {
            width: parent.width
            spacing: Style.space(6)

            CursorSurface {
              id: rangeHeader
              width: parent.width
              hasCursor: root.cursorActive && root.focusSection === "range"
              foreground: root.foreground
              implicitHeight: Math.max(prevButton.implicitHeight, rangeTitle.implicitHeight) + Style.space(6)

              MouseArea {
                anchors.fill: parent
                hoverEnabled: true
                onEntered: root.setRangeCursor()
              }

              PanelActionButton {
                id: prevButton
                anchors.left: parent.left
                anchors.leftMargin: Style.space(2)
                anchors.verticalCenter: parent.verticalCenter
                iconText: String.fromCodePoint(0xF0141)
                tooltipText: "Previous range"
                foreground: root.foreground
                fontFamily: root.fontFamily
                onClicked: root.stepRange(-1)
              }

              Column {
                id: rangeTitle
                anchors.centerIn: parent
                spacing: Style.space(1)

                PanelSectionHeader {
                  anchors.horizontalCenter: parent.horizontalCenter
                  text: root.range ? String(root.range.header).toUpperCase() : "SUMMARY"
                  foreground: root.foreground
                  fontFamily: root.fontFamily
                }

                Text {
                  textFormat: Text.PlainText
                  anchors.horizontalCenter: parent.horizontalCenter
                  text: root.rangeSpan(root.range)
                  color: root.dim
                  font.family: root.fontFamily
                  font.pixelSize: Style.font.caption
                }
              }

              PanelActionButton {
                anchors.right: parent.right
                anchors.rightMargin: Style.space(2)
                anchors.verticalCenter: parent.verticalCenter
                iconText: String.fromCodePoint(0xF0142)
                tooltipText: "Next range"
                foreground: root.foreground
                fontFamily: root.fontFamily
                onClicked: root.stepRange(1)
              }
            }

            Text {
              textFormat: Text.PlainText
              visible: tt.loaded && root.reportRows.length === 0
              width: parent.width
              text: "Nothing tracked in this range."
              color: root.dim
              font.family: root.fontFamily
              font.pixelSize: Style.font.body
              horizontalAlignment: Text.AlignHCenter
            }

            Repeater {
              model: root.reportRows
              ReportRow {
                required property var modelData
                width: parent.width
                label: tt.tasks[modelData] ? String(tt.tasks[modelData].name) : ""
                seconds: tt.secondsFor(root.rangeIndex, modelData)
                highlighted: modelData === tt.activeIndex
              }
            }

            ReportRow {
              visible: root.reportRows.length > 0
              width: parent.width
              label: "Total"
              seconds: tt.totalFor(root.rangeIndex)
              highlighted: true
              bold: true
            }
          }
        }
      }
    }
  }

  component TaskRow: CursorSurface {
    id: row
    property var task: null
    property int rowIndex: 0
    readonly property bool isActive: task ? task.active === true : false

    hasCursor: root.cursorActive && root.focusSection === "tasks" && root.taskIndex === rowIndex
    current: isActive
    foreground: root.foreground
    implicitHeight: Math.max(Style.spacing.popupRowHeight, rowContent.implicitHeight + Style.space(6))

    MouseArea {
      anchors.fill: parent
      hoverEnabled: true
      cursorShape: Qt.PointingHandCursor
      onEntered: root.setTaskCursor(row.rowIndex)
      onClicked: root.toggleTask(row.rowIndex)
    }

    RowLayout {
      id: rowContent
      anchors.left: parent.left
      anchors.right: parent.right
      anchors.verticalCenter: parent.verticalCenter
      anchors.leftMargin: Style.space(10)
      anchors.rightMargin: Style.space(3)
      spacing: Style.space(8)

      Text {
        textFormat: Text.PlainText
        Layout.fillWidth: true
        text: row.task ? String(row.task.name) : ""
        color: root.foreground
        font.family: root.fontFamily
        font.pixelSize: Style.font.body
        font.bold: row.isActive
        elide: Text.ElideRight
      }

      Text {
        textFormat: Text.PlainText
        text: Model.formatDuration(tt.secondsFor(0, row.rowIndex))
        color: row.isActive ? root.foreground : root.dim
        font.family: root.fontFamily
        font.pixelSize: Style.font.bodySmall
      }

      PanelActionButton {
        iconText: row.isActive ? root.stopGlyph : root.startGlyph
        tooltipText: row.isActive ? "Stop" : "Start"
        foreground: root.foreground
        fontFamily: root.fontFamily
        onClicked: root.toggleTask(row.rowIndex)
      }
    }
  }

  component ReportRow: Item {
    property string label: ""
    property double seconds: 0
    property bool highlighted: false
    property bool bold: false

    implicitHeight: Math.max(nameText.implicitHeight, timeText.implicitHeight)

    Text {
      id: nameText
      textFormat: Text.PlainText
      anchors.left: parent.left
      anchors.right: timeText.left
      anchors.leftMargin: Style.space(10)
      anchors.rightMargin: Style.space(8)
      text: parent.label
      color: parent.highlighted ? root.foreground : root.dim
      font.family: root.fontFamily
      font.pixelSize: Style.font.bodySmall
      font.bold: parent.bold
      elide: Text.ElideRight
    }

    Text {
      id: timeText
      textFormat: Text.PlainText
      anchors.right: parent.right
      anchors.rightMargin: Style.space(10)
      text: Model.formatDuration(parent.seconds)
      color: parent.highlighted ? root.foreground : root.dim
      font.family: root.fontFamily
      font.pixelSize: Style.font.bodySmall
      font.bold: parent.bold
    }
  }
}
