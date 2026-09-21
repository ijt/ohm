// Toggles the "Shinto Downloads" Omarchy Quickshell panel (see
// downloads-panel/) -- Quickshell owns showing/hiding/focusing it, so
// there's no dedup or window-focus logic to do here, unlike the old
// terminal-spawned downloads-tui this replaced. Shared by BrowserWindow's
// bar-click and the "Download started" notification's click action (see
// Notify.h's notifyClickable).
#pragma once

namespace shinto {

void showDownloadsPanel();

}  // namespace shinto
