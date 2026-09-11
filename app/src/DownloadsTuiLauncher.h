// Launches `shinto-downloads` (see downloads-tui/) in a fresh terminal --
// but only if one isn't already running, to avoid piling up redundant
// panes (confirmed concretely: repeated bar-clicks each spawning a new
// one). Shared by BrowserWindow's bar-click and the "Download started"
// notification's click action (see Notify.h's notifyClickable).
#pragma once

namespace shinto {

void launchOrSkipDownloadsTui();

}  // namespace shinto
