// Toggles Shinto's Omarchy Quickshell panels (downloads-panel/,
// shortcuts-panel/). Quickshell owns showing/hiding/focusing them, so
// there's no dedup or window-focus logic to do here, unlike the old
// terminal-spawned downloads-tui this replaced. Shared by BrowserWindow's
// bar-click / Ctrl+? and the "Download started" notification's click
// action (see Notify.h's notifyClickable).
#pragma once

class QKeyEvent;

namespace shinto {

void showDownloadsPanel();
void showShortcutsPanel();

// Ctrl+/, Ctrl+? (Ctrl+Shift+/ on a US layout), and F1. QLineEdit does not
// treat these as its own shortcuts, but a focused address field still
// receives the KeyPress when the window QShortcut's sequence doesn't match
// the event Qt actually delivered (Ctrl+Shift+Question vs Ctrl+Shift+Slash).
bool isShortcutsPanelKey(const QKeyEvent *key);

}  // namespace shinto
