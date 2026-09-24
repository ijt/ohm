// User-editable settings loaded from a real Lua config file
// (ohm::configLuaPath(), normally ~/.config/ohm-browser/config.lua) via an
// embedded Lua interpreter. Currently just the search engine, but unlike
// ThemeLoader's flat colors.toml reader this one runs actual Lua, so a
// config file can compute a value however it likes (env vars via os.getenv,
// conditionals, etc.) as long as it ends up setting the right globals.
#pragma once

#include <QKeySequence>
#include <QString>

namespace ohm {

struct OhmConfig {
  // URL template used for the omnibox's search fallback (whatever wasn't
  // recognized as a bare URL -- see HistoryStore::toUrl). "%s" is replaced
  // with the percent-encoded query. Overridden by config.lua setting the
  // global `search_engine`, e.g.:
  //   search_engine = "https://www.google.com/search?q=%s"
  QString searchEngineUrl = QStringLiteral("https://duckduckgo.com/?q=%s");

  // Browser-back shortcut. Defaults to Chromium's own default (Alt+Left).
  // Overridden by config.lua setting the global `back_shortcut` to a Qt
  // key-sequence string, e.g.:
  //   back_shortcut = "Ctrl+["
  QKeySequence backShortcut = QKeySequence(Qt::ALT | Qt::Key_Left);
};

// Executes configLuaPath() (if it exists) and reads back recognized
// globals. A missing file is silently fine (nothing to report). A Lua
// syntax/runtime error, or a `search_engine` that isn't a string or has no
// "%s" placeholder, falls back to OhmConfig's defaults but is reported
// loudly -- both a qWarning and a desktop notification (see
// reportConfigError in Config.cpp), since Ohm is normally launched with
// no terminal in sight. A broken user config should never stop Ohm from
// starting, but it should never fail silently either.
OhmConfig loadConfig();

}  // namespace ohm
