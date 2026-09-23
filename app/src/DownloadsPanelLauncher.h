// Toggles Shinto's Omarchy Quickshell panels (downloads-panel/,
// shortcuts-panel/). Quickshell owns showing/hiding/focusing them, so
// there's no dedup or window-focus logic to do here, unlike the old
// terminal-spawned downloads-tui this replaced. Shared by BrowserWindow's
// bar-click / Ctrl+? and the "Download started" notification's click
// action (see Notify.h's notifyClickable).
#pragma once

namespace shinto {

void showDownloadsPanel();
void showShortcutsPanel();

}  // namespace shinto
