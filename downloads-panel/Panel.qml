import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Quickshell
import Quickshell.Wayland
import qs.Commons
import qs.Ui
import "Model.js" as Model

// Standalone summonable panel (not tied to a bar icon) -- same "panel" kind
// contract as omarchy.disk-speedtest/omarchy.wifiqr: a plain Item exposing
// open(payloadJson)/close()/dismiss(), building its own PanelWindow overlay.
// Replaces the old downloads-tui Go/Bubble Tea TUI: same downloads.sqlite,
// same shinto.sock cancel protocol (both via downloads_helper.py), same
// Omarchy theme file, just as a mouse-and-keyboard Quickshell panel instead
// of a separate terminal window.
Item {
  id: root

  property var shell: null
  property var manifest: null
  property bool opened: false

  readonly property string pluginId: manifest && manifest.id ? String(manifest.id) : "shinto.downloads"
  readonly property string pluginDir: manifest && manifest.__sourceDir ? String(manifest.__sourceDir) : ""
  readonly property string fontFamily: Style.font.family

  property bool cursorActive: false
  property int cursorIndex: -1

  readonly property int activeCount: {
    var n = 0
    for (var i = 0; i < downloadsSvc.downloads.length; i++)
      if (downloadsSvc.downloads[i].state === Model.STATE_IN_PROGRESS) n++
    return n
  }

  function open(payloadJson) {
    root.opened = true
    root.cursorActive = false
    root.cursorIndex = -1
    downloadsSvc.refresh()
    Qt.callLater(function() { if (root.opened) keyCatcher.forceActiveFocus() })
  }

  function close() {
    root.opened = false
  }

  function dismiss() {
    if (root.shell && typeof root.shell.hide === "function") root.shell.hide(root.pluginId)
    else root.close()
  }

  function ensureCursor() {
    var n = downloadsSvc.downloads.length
    if (n === 0) { root.cursorIndex = -1; return }
    if (root.cursorIndex < 0) root.cursorIndex = 0
    if (root.cursorIndex >= n) root.cursorIndex = n - 1
  }

  function moveCursor(dy) {
    root.cursorActive = true
    ensureCursor()
    if (downloadsSvc.downloads.length === 0) return
    root.cursorIndex = Math.max(0, Math.min(downloadsSvc.downloads.length - 1, root.cursorIndex + dy))
  }

  function setCursor(index) {
    root.cursorActive = true
    root.cursorIndex = index
  }

  function activateCursor() {
    ensureCursor()
    if (root.cursorIndex < 0) return
    var row = downloadsSvc.downloads[root.cursorIndex]
    var actions = Model.actionsFor(row.state)
    if (actions.indexOf("open") !== -1) downloadsSvc.openFile(row.path)
    else if (actions.indexOf("reveal") !== -1) downloadsSvc.reveal(row.path)
  }

  Service {
    id: downloadsSvc
    pluginDir: root.pluginDir
    active: root.opened
  }

  PanelWindow {
    id: win
    visible: root.opened
    anchors { top: true; bottom: true; left: true; right: true }
    color: "transparent"
    exclusionMode: ExclusionMode.Ignore
    WlrLayershell.namespace: "shinto-downloads"
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

    Item {
      id: keyCatcher
      anchors.fill: parent
      focus: true

      Keys.onEscapePressed: root.dismiss()
      Keys.onUpPressed: root.moveCursor(-1)
      Keys.onDownPressed: root.moveCursor(1)
      Keys.onReturnPressed: if (root.cursorActive) root.activateCursor()
      Keys.onEnterPressed: if (root.cursorActive) root.activateCursor()
      Keys.onPressed: function(event) {
        if (event.text === "c" || event.text === "C") {
          downloadsSvc.clearFinished()
          event.accepted = true
        }
      }

      Item {
        id: cardWrap
        anchors.centerIn: parent
        // Hugs the content's natural height (hero + separator + rows, or the
        // empty-state text) up to a cap, instead of always claiming the full
        // maximum -- a single download no longer leaves a big empty card.
        readonly property real maxWidth: Style.space(460)
        readonly property real maxHeight: Style.space(560)
        readonly property real minHeight: Style.space(160)
        width: Math.min(cardWrap.maxWidth, keyCatcher.width - Style.space(48))
        height: Math.min(
          Math.min(cardWrap.maxHeight, keyCatcher.height - Style.space(48)),
          Math.max(cardWrap.minHeight, contentColumn.implicitHeight + card.contentTopInset + card.contentBottomInset))

        // Swallow clicks so only the scrim outside the card dismisses.
        MouseArea { anchors.fill: parent; onClicked: {} }

        BorderSurface {
          id: card
          anchors.fill: parent
          color: Color.popups.background
          radius: Style.cornerRadius
          padding: Style.space(16)
          borderSpec: Border.localOrSurfaceSpec("popups", "border", Color.popups.border, Color.popups.border, Math.max(1, Style.space(2)))

          // One Flickable for the whole card (hero included), not just the
          // row list -- same idiom as the Dropbox panel -- so the card can
          // size itself to contentColumn.implicitHeight below the cap and
          // only scroll once real overflow happens above it.
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
                id: hero
                width: parent.width
                title: "Downloads"
                meta: root.activeCount > 0 ? root.activeCount + " active" : ""
                foreground: Color.foreground
                fontFamily: root.fontFamily
                iconComponent: Component {
                  Text {
                    textFormat: Text.PlainText
                    text: "⤓" // ⤓
                    color: Color.foreground
                    font.pixelSize: Style.font.display
                  }
                }
                trailingControl: Component {
                  PanelActionButton {
                    iconText: "⊘" // ⊘ -- "clear finished"
                    tooltipText: "Clear finished"
                    foreground: Color.foreground
                    fontFamily: root.fontFamily
                    onClicked: downloadsSvc.clearFinished()
                  }
                }
              }

              PanelSeparator { foreground: Color.foreground }

              Text {
                textFormat: Text.PlainText
                visible: downloadsSvc.downloads.length === 0
                width: parent.width
                topPadding: Style.space(24)
                text: "No downloads yet"
                color: Qt.darker(Color.foreground, 1.4)
                font.family: root.fontFamily
                font.pixelSize: Style.font.body
                horizontalAlignment: Text.AlignHCenter
              }

              Column {
                id: rowColumn
                visible: downloadsSvc.downloads.length > 0
                width: parent.width
                spacing: Style.space(6)

                Repeater {
                  model: downloadsSvc.downloads
                  DownloadRow {
                    required property var modelData
                    required property int index
                    width: rowColumn.width
                    row: modelData
                    rowIndex: index
                  }
                }
              }
            }
          }
        }
      }
    }
  }

  // Glyphs below are plain Unicode, not JetBrains Mono Nerd Font PUA
  // codepoints -- safe to render in any font, unlike the rest of this
  // shell's icon set.
  component DownloadRow: CursorSurface {
    id: dlRow
    property var row: null
    property int rowIndex: 0
    readonly property int rowState: row ? row.state : -1
    readonly property var prog: row ? Model.progress(row) : ({ known: false, fraction: 0, text: "" })
    readonly property var rowActions: row ? Model.actionsFor(row.state) : []
    readonly property color stateColor: rowState === Model.STATE_IN_PROGRESS ? Color.accent
      : rowState === Model.STATE_INTERRUPTED ? Color.urgent
      : rowState === Model.STATE_COMPLETED ? Color.foreground
      : Qt.darker(Color.foreground, 1.4)

    hasCursor: root.cursorActive && root.cursorIndex === rowIndex
    foreground: Color.foreground

    implicitHeight: rowContent.implicitHeight + Style.space(14)

    MouseArea {
      anchors.fill: parent
      hoverEnabled: true
      onEntered: root.setCursor(dlRow.rowIndex)
    }

    RowLayout {
      id: rowContent
      anchors.left: parent.left
      anchors.right: parent.right
      anchors.verticalCenter: parent.verticalCenter
      anchors.leftMargin: Style.space(10)
      anchors.rightMargin: Style.space(10)
      spacing: Style.space(8)

      Text {
        textFormat: Text.PlainText
        text: Model.stateGlyph(dlRow.rowState)
        color: dlRow.stateColor
        font.family: root.fontFamily
        font.pixelSize: Style.font.body
        Layout.alignment: Qt.AlignVCenter
      }

      ColumnLayout {
        Layout.fillWidth: true
        spacing: Style.space(2)

        Text {
          textFormat: Text.PlainText
          Layout.fillWidth: true
          text: dlRow.row ? String(dlRow.row.filename || "") : ""
          color: Color.foreground
          font.family: root.fontFamily
          font.pixelSize: Style.font.body
          elide: Text.ElideRight
        }

        Item {
          Layout.fillWidth: true
          implicitHeight: Math.max(Style.space(4), capText.implicitHeight)

          Rectangle {
            visible: dlRow.rowState === Model.STATE_IN_PROGRESS && dlRow.prog.known
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            height: Style.space(4)
            radius: height / 2
            color: Qt.darker(Color.foreground, 3)

            Rectangle {
              width: parent.width * dlRow.prog.fraction
              height: parent.height
              radius: height / 2
              color: Color.accent
            }
          }

          Text {
            id: capText
            visible: !(dlRow.rowState === Model.STATE_IN_PROGRESS && dlRow.prog.known)
            textFormat: Text.PlainText
            text: dlRow.rowState === Model.STATE_IN_PROGRESS
              ? dlRow.prog.text
              : Model.stateLabel(dlRow.rowState) + " · " + Model.formatBytes(dlRow.row ? dlRow.row.received_bytes : 0)
            color: Qt.darker(Color.foreground, 1.4)
            font.family: root.fontFamily
            font.pixelSize: Style.font.caption
          }
        }
      }

      RowLayout {
        spacing: Style.space(4)

        PanelActionButton {
          visible: dlRow.rowActions.indexOf("open") !== -1
          iconText: "↗" // ↗ open file
          tooltipText: "Open file"
          foreground: Color.foreground
          fontFamily: root.fontFamily
          onClicked: downloadsSvc.openFile(dlRow.row.path)
        }
        PanelActionButton {
          visible: dlRow.rowActions.indexOf("reveal") !== -1
          iconText: "▣" // ▣ show in folder
          tooltipText: "Show in folder"
          foreground: Color.foreground
          fontFamily: root.fontFamily
          onClicked: downloadsSvc.reveal(dlRow.row.path)
        }
        PanelActionButton {
          visible: dlRow.rowActions.indexOf("cancel") !== -1
          iconText: "✕" // ✕ cancel
          tooltipText: "Cancel download"
          foreground: Color.foreground
          hoverColor: Color.urgent
          fontFamily: root.fontFamily
          onClicked: downloadsSvc.cancel(dlRow.row.id)
        }
      }
    }
  }
}
