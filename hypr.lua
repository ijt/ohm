-- Every Ohm window shares one fixed Wayland app_id ("ohm-browser",
-- QGuiApplication::setDesktopFileName in app/src/main.cpp) -- there's no
-- more URL-derived app_id or spare/keep-alive window to special-case; the
-- daemon stays warm with zero windows on its own (setQuitOnLastWindowClosed).

o.window("ohm-browser", { tag = "+chromium-based-browser" })
