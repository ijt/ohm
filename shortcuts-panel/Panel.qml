import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Quickshell
import Quickshell.Wayland
import qs.Commons
import qs.Ui

// Standalone summonable panel -- same "panel" kind contract as
// shinto.downloads / omarchy.disk-speedtest: a plain Item exposing
// open(payloadJson)/close()/dismiss(), building its own PanelWindow overlay.
// Static cheatsheet; no helper process. Ctrl+? / F1 in a Shinto window
// toggles it (and the same keys dismiss it while focused here, since
// Exclusive keyboard focus would otherwise eat the window-level shortcut).
Item {
  id: root

  property var shell: null
  property var manifest: null
  property bool opened: false

  readonly property string pluginId: manifest && manifest.id ? String(manifest.id) : "shinto.shortcuts"
  readonly property string fontFamily: Style.font.family

  readonly property var sections: [
    {
      title: "This window",
      rows: [
        { keys: "Ctrl+T", action: "New page in this Hyprland group" },
        { keys: "Ctrl+N", action: "New page as a standalone window" },
        { keys: "Ctrl+L / Ctrl+K", action: "Edit this window's address" },
        { keys: "Alt+Left", action: "Back (config.lua: back_shortcut)" },
        { keys: "Ctrl+F", action: "Find in page" },
        { keys: "Ctrl+R", action: "Reload" },
        { keys: "Ctrl+= / Ctrl+-", action: "Zoom in / zoom out" },
        { keys: "Ctrl+P", action: "Print" },
        { keys: "Ctrl+W / Super+Q", action: "Close this page" },
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

  function open(payloadJson) {
    root.opened = true
    Qt.callLater(function() { if (root.opened) keyCatcher.forceActiveFocus() })
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
    WlrLayershell.namespace: "shinto-shortcuts"
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
      Keys.onPressed: function(event) {
        if (root.isToggleKey(event)) {
          root.dismiss()
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
                meta: "Esc to close"
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

              Repeater {
                model: root.sections
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

    implicitHeight: rowContent.implicitHeight + Style.space(6)

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
