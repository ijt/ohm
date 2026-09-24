import QtQuick
import Quickshell.Io
import "Model.js" as Model

// Polls downloads_helper.py (which reads Ohm's downloads.sqlite and
// dials ohm-browser.sock) rather than touching SQLite or the socket from QML
// directly -- same shell-out-to-a-helper idiom as the Dropbox panel's
// Service.qml.
Item {
  id: root

  property string pluginDir: ""
  property bool active: false

  readonly property string helperPath: root.pluginDir !== "" ? root.pluginDir + "/downloads_helper.py" : ""

  property var downloads: []
  property bool lastReadFailed: false

  function refresh() {
    if (root.helperPath === "" || listProcess.running) return
    listProcess.command = ["python3", root.helperPath, "list"]
    listProcess.running = true
  }

  function applyList(raw) {
    var parsed = Model.parseList(raw)
    if (!parsed.ok) {
      // Keep the last known-good list rather than flashing empty -- the old
      // downloads-tui Go app had exactly this bug once (see its db_test.go).
      root.lastReadFailed = true
      return
    }
    root.downloads = parsed.rows
    root.lastReadFailed = false
  }

  function runAction(args) {
    if (root.helperPath === "" || actionProcess.running) return
    actionProcess.command = ["python3", root.helperPath].concat(args)
    actionProcess.running = true
  }

  function clearFinished() { runAction(["clear-finished"]) }
  function cancel(id) { runAction(["cancel", String(id)]) }
  function openFile(path) { runAction(["open", String(path)]) }
  function reveal(path) { runAction(["reveal", String(path)]) }

  Process {
    id: listProcess
    command: []
    stdout: StdioCollector {
      id: listStdout
      waitForEnd: true
      onStreamFinished: root.applyList(text)
    }
    onExited: function(exitCode) {
      if (exitCode !== 0) root.lastReadFailed = true
    }
  }

  Process {
    id: actionProcess
    command: []
    onExited: settleTimer.restart()
  }

  Timer {
    id: pollTimer
    interval: 500
    repeat: true
    running: root.active
    triggeredOnStart: true
    onTriggered: root.refresh()
  }

  Timer {
    // Re-poll a few times right after a mutating action (cancel/clear) so
    // the panel reflects the daemon's new state without waiting for the
    // next periodic tick -- same idiom as the Dropbox panel's settleTimer.
    id: settleTimer
    property int ticks: 0
    interval: 300
    repeat: true
    running: false
    onTriggered: {
      settleTimer.ticks += 1
      root.refresh()
      if (settleTimer.ticks >= 3) {
        settleTimer.ticks = 0
        settleTimer.running = false
      }
    }
  }
}
