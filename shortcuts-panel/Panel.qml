import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Quickshell
import Quickshell.Wayland
import qs.Commons
import qs.Ui

// Standalone summonable panel -- same "panel" kind contract as
// ohm.downloads / omarchy.disk-speedtest: a plain Item exposing
// open(payloadJson)/close()/dismiss(), building its own PanelWindow overlay.
// A cheatsheet that doubles as a command palette: typing filters the rows,
// and Enter (or a click) runs the selected row's `command` via
// `ohm --command <name>`, which the daemon runs in the last-focused
// Ohm window (BrowserWindow::runCommand). Rows without a command, like
// the Hyprland ones, are reference only. Ctrl+? / F1 in a Ohm window
// toggles it (and the same keys dismiss it while focused here, since
// Exclusive keyboard focus would otherwise eat the window-level shortcut).
Item {
  id: root

  property var shell: null
  property var manifest: null
  property bool opened: false

  readonly property string pluginId: manifest && manifest.id ? String(manifest.id) : "ohm.shortcuts"
  readonly property string fontFamily: Style.font.family
  // Typed filter. A leading ":" (vim habit) is ignored.
  readonly property string query: filterField.text.replace(/^:+/, "").trim().toLowerCase()
  // Index into runnableRows of the row Enter would run; -1 for none.
  property int selectedIndex: -1

  readonly property var sections: [
    {
      title: "This window",
      rows: [
        { keys: "Ctrl+T", action: "New page in this Hyprland group", command: "new-tab" },
        { keys: "Ctrl+Shift+T", action: "Reopen the last closed page", command: "reopen" },
        { keys: "Ctrl+N", action: "New page as a standalone window", command: "new-window" },
        { keys: "Ctrl+L / Ctrl+K", action: "Edit this window's address", command: "edit-address" },
        { keys: "Alt+Left", action: "Back (config.lua: back_shortcut)", command: "back" },
        { keys: "Ctrl+F", action: "Find in page", command: "find" },
        { keys: "Ctrl+R", action: "Reload", command: "reload" },
        { keys: "Ctrl+=", action: "Zoom in", command: "zoom-in" },
        { keys: "Ctrl+-", action: "Zoom out", command: "zoom-out" },
        { keys: "Ctrl+P", action: "Print", command: "print" },
        { keys: "Download bar", action: "Show downloads", command: "downloads" },
        { keys: "F12 / Ctrl+Shift+I", action: "Developer tools (right-click: Inspect)", command: "devtools" },
        { keys: "Ctrl+W / Super+Q", action: "Close this page", command: "close" },
        { keys: "Ctrl+? / F1", action: "This list" }
      ]
    },
    {
      title: "Hyprland groups",
      rows: [
        { keys: "Super+G", action: "Toggle group (these are the tabs)" },
        { keys: "Super+Ctrl+Left/Right", action: "Cycle pages in the group" },
        { keys: "Super+Alt+1/2/3/4", action: "Jump to grouped window N" },
        { keys: "Super+Alt+G", action: "Pull this window out of the group" }
      ]
    }
  ]

  // How well `q` matches a row, lower is better, -1 for no match: at the
  // start of a word, anywhere, or as letters in order ("rop" -> reopen).
  function matchScore(row, q) {
    if (q === "") return 0
    const hay = (row.action + " " + (row.command || "") + " " + row.keys).toLowerCase()
    const at = hay.indexOf(q)
    if (at === 0 || (at > 0 && /[\s\-\/(+]/.test(hay[at - 1]))) return 0
    if (at > 0) return 1
    let j = 0
    for (let i = 0; i < hay.length && j < q.length; i++)
      if (hay[i] === q[j]) j++
    return j === q.length ? 2 : -1
  }

  // Sections with only the matching rows, best matches first; sections
  // with no matches drop out.
  readonly property var visibleSections: {
    const q = root.query
    const out = []
    for (const section of root.sections) {
      const scored = []
      section.rows.forEach(function(row, i) {
        const score = root.matchScore(row, q)
        if (score >= 0) scored.push({ row: row, score: score, i: i })
      })
      scored.sort(function(a, b) { return a.score - b.score || a.i - b.i })
      if (scored.length > 0)
        out.push({ title: section.title, rows: scored.map(function(s) { return s.row }) })
    }
    return out
  }

  readonly property var runnableRows: {
    const out = []
    for (const section of root.visibleSections)
      for (const row of section.rows)
        if (row.command) out.push(row)
    return out
  }

  // Typing selects the best match; an empty filter selects nothing, so a
  // stray Enter on the plain cheatsheet does nothing.
  onQueryChanged: root.selectedIndex = root.query === "" || root.runnableRows.length === 0 ? -1 : 0

  // Palette keys, wherever focus is in the panel. Returns whether the key
  // was used. Up/Down (and vim-ish Ctrl+J/K, readline Ctrl+N/P) move the
  // selection; Enter runs it; Escape and the toggle keys close the panel.
  function handleNavKey(event) {
    const ctrl = (event.modifiers & Qt.ControlModifier) !== 0
    if (root.isToggleKey(event) || event.key === Qt.Key_Escape) {
      root.dismiss()
    } else if (event.key === Qt.Key_Down || (ctrl && (event.key === Qt.Key_J || event.key === Qt.Key_N))) {
      root.moveSelection(1)
    } else if (event.key === Qt.Key_Up || (ctrl && (event.key === Qt.Key_K || event.key === Qt.Key_P))) {
      root.moveSelection(-1)
    } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
      if (root.selectedIndex >= 0) root.run(root.runnableRows[root.selectedIndex])
    } else {
      return false
    }
    return true
  }

  function moveSelection(delta) {
    const n = root.runnableRows.length
    if (n === 0) return
    root.selectedIndex = root.selectedIndex < 0
      ? (delta > 0 ? 0 : n - 1)
      : (root.selectedIndex + delta + n) % n
  }

  function run(row) {
    if (!row || !row.command) return
    // Detached, not a Process child of this panel: hiding the panel
    // unloads it, which would take a still-starting Process with it (the
    // command then silently never reached the daemon). Launched before
    // the hide for the same reason; the daemon handles it after focus is
    // back on the Ohm window either way.
    Util.execArgv(["ohm", "--command", row.command])
    root.dismiss()
  }

  function open(payloadJson) {
    filterField.text = ""
    root.selectedIndex = -1
    root.opened = true
    Qt.callLater(function() { if (root.opened) filterField.forceActiveFocus() })
  }

  function close() {
    root.opened = false
  }

  function dismiss() {
    if (root.shell && typeof root.shell.hide === "function") root.shell.hide(root.pluginId)
    else root.close()
  }

  function isToggleKey(event) {
    if (event.key === Qt.Key_F1)
      return (event.modifiers & (Qt.ControlModifier | Qt.AltModifier | Qt.MetaModifier)) === 0
    if (!(event.modifiers & Qt.ControlModifier))
      return false
    return event.key === Qt.Key_Slash || event.key === Qt.Key_Question
  }

  PanelWindow {
    id: win
    visible: root.opened
    anchors { top: true; bottom: true; left: true; right: true }
    color: "transparent"
    exclusionMode: ExclusionMode.Ignore
    WlrLayershell.namespace: "ohm-shortcuts"
    WlrLayershell.layer: WlrLayer.Overlay
    WlrLayershell.keyboardFocus: root.opened ? WlrKeyboardFocus.Exclusive : WlrKeyboardFocus.None

    Rectangle {
      anchors.fill: parent
      color: Qt.rgba(0, 0, 0, 0.5)

      MouseArea {
        anchors.fill: parent
        onClicked: root.dismiss()
      }
    }

    // A FocusScope whose focused child is the filter field, so the field
    // (not this catcher) gets the keyboard whenever the window does.
    // Keys the field doesn't take (Up/Down, Enter, Escape, the toggle
    // keys) bubble up here, which also covers focus landing anywhere else
    // in the panel.
    FocusScope {
      id: keyCatcher
      anchors.fill: parent
      focus: true

      Keys.onPressed: function(event) {
        if (root.handleNavKey(event)) {
          event.accepted = true
        } else if (event.text !== "" && !(event.modifiers & Qt.ControlModifier)) {
          // Typing with focus elsewhere still goes to the filter.
          filterField.forceActiveFocus()
          filterField.insert(filterField.cursorPosition, event.text)
          event.accepted = true
        }
      }

      Item {
        id: cardWrap
        anchors.centerIn: parent
        readonly property real maxWidth: Style.space(520)
        readonly property real maxHeight: Style.space(640)
        readonly property real minHeight: Style.space(160)
        width: Math.min(cardWrap.maxWidth, keyCatcher.width - Style.space(48))
        height: Math.min(
          Math.min(cardWrap.maxHeight, keyCatcher.height - Style.space(48)),
          Math.max(cardWrap.minHeight, contentColumn.implicitHeight + card.contentTopInset + card.contentBottomInset))

        MouseArea { anchors.fill: parent; onClicked: {} }

        BorderSurface {
          id: card
          anchors.fill: parent
          color: Color.popups.background
          radius: Style.cornerRadius
          padding: Style.space(16)
          borderSpec: Border.localOrSurfaceSpec("popups", "border", Color.popups.border, Color.popups.border, Math.max(1, Style.space(2)))

          Flickable {
            id: cardFlick
            anchors.fill: parent
            anchors.topMargin: card.contentTopInset
            anchors.rightMargin: card.contentRightInset
            anchors.bottomMargin: card.contentBottomInset
            anchors.leftMargin: card.contentLeftInset
            contentWidth: width
            contentHeight: contentColumn.implicitHeight
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

            Column {
              id: contentColumn
              width: cardFlick.width
              spacing: Style.space(12)

              PanelHero {
                width: parent.width
                title: "Shortcuts"
                meta: "Type to run one · Esc to close"
                foreground: Color.foreground
                fontFamily: root.fontFamily
                iconComponent: Component {
                  Text {
                    textFormat: Text.PlainText
                    text: "?"
                    color: Color.foreground
                    font.pixelSize: Style.font.display
                    font.bold: true
                  }
                }
              }

              TextField {
                id: filterField
                width: parent.width
                focus: true
                placeholderText: "Type a command: reopen, zoom, find…"
                // Handled before the field's own editing keys, so
                // Enter and Ctrl+J/K/N/P drive the palette instead.
                Keys.onPressed: function(event) {
                  if (root.handleNavKey(event)) event.accepted = true
                }
              }

              Text {
                textFormat: Text.PlainText
                visible: root.visibleSections.length === 0
                width: parent.width
                text: "No matching command"
                color: Qt.darker(Color.foreground, 1.4)
                font.family: root.fontFamily
                font.pixelSize: Style.font.body
                horizontalAlignment: Text.AlignHCenter
              }

              Repeater {
                model: root.visibleSections
                Column {
                  required property var modelData
                  width: contentColumn.width
                  spacing: Style.space(8)

                  PanelSeparator { foreground: Color.foreground }

                  PanelSectionHeader {
                    text: modelData.title
                    foreground: Color.foreground
                    fontFamily: root.fontFamily
                  }

                  Repeater {
                    model: modelData.rows
                    ShortcutRow {
                      required property var modelData
                      width: contentColumn.width
                      keys: modelData.keys
                      action: modelData.action
                      runnable: !!modelData.command
                      // By name: Repeater may hand rows a copy, not the same object.
                      selected: !!modelData.command && root.selectedIndex >= 0 &&
                                root.runnableRows[root.selectedIndex].command === modelData.command
                      onClicked: root.run(modelData)
                    }
                  }
                }
              }

              Text {
                textFormat: Text.PlainText
                width: parent.width
                topPadding: Style.space(4)
                text: "Super+K lists Hyprland's own keybindings"
                color: Qt.darker(Color.foreground, 1.4)
                font.family: root.fontFamily
                font.pixelSize: Style.font.caption
                horizontalAlignment: Text.AlignHCenter
              }
            }
          }
        }
      }
    }
  }

  component ShortcutRow: Item {
    id: row
    property string keys: ""
    property string action: ""
    property bool runnable: false
    property bool selected: false
    signal clicked()

    implicitHeight: rowContent.implicitHeight + Style.space(6)

    Rectangle {
      anchors.fill: parent
      radius: Style.cornerRadius
      color: row.selected ? Qt.rgba(Color.accent.r, Color.accent.g, Color.accent.b, 0.25)
           : rowMouse.containsMouse && row.runnable ? Qt.rgba(Color.foreground.r, Color.foreground.g, Color.foreground.b, 0.08)
           : "transparent"
    }

    MouseArea {
      id: rowMouse
      anchors.fill: parent
      hoverEnabled: true
      enabled: row.runnable
      cursorShape: row.runnable ? Qt.PointingHandCursor : Qt.ArrowCursor
      onClicked: row.clicked()
    }

    RowLayout {
      id: rowContent
      anchors.left: parent.left
      anchors.right: parent.right
      anchors.verticalCenter: parent.verticalCenter
      spacing: Style.space(12)

      Text {
        textFormat: Text.PlainText
        text: row.keys
        color: Color.accent
        font.family: root.fontFamily
        font.pixelSize: Style.font.body
        font.bold: true
        Layout.preferredWidth: Style.space(200)
        Layout.alignment: Qt.AlignVCenter | Qt.AlignRight
        horizontalAlignment: Text.AlignRight
      }

      Text {
        textFormat: Text.PlainText
        text: row.action
        color: Color.foreground
        font.family: root.fontFamily
        font.pixelSize: Style.font.body
        Layout.fillWidth: true
        elide: Text.ElideRight
      }
    }
  }
}
