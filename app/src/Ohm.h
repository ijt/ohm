// Shared constants and small path helpers used across the Ohm app.
// Mirrors the paths the old bash `ohm` script/control-server.py used, so a
// migrating install lands in familiar places.
#pragma once

#include <QDir>
#include <QStandardPaths>
#include <QString>

namespace ohm {

// Fixed Wayland app_id / QGuiApplication name. Every Ohm window uses this
// same id (see hypr.lua), unlike the old Chromium --app windows whose app_id
// was derived from the loaded URL.
inline const char *kAppId = "ohm-browser";

// Subcommands of the `ohm` wrapper (install/uninstall/daemon/…). The
// browser binary must never navigate to these: a schemeless QUrl paints as
// a blank white about:blank window and the command never runs.
inline bool isShellCommand(const QString &arg) {
  const QString c = arg.trimmed().toLower();
  return c == QLatin1String("install") || c == QLatin1String("uninstall") ||
         c == QLatin1String("start") || c == QLatin1String("stop") ||
         c == QLatin1String("restart") || c == QLatin1String("status") ||
         c == QLatin1String("default") || c == QLatin1String("daemon") ||
         c == QLatin1String("theme") || c == QLatin1String("help");
}

// ~/.local/share/ohm-browser (respects XDG_DATA_HOME).
inline QString dataHome() {
  QString base = QString::fromLocal8Bit(qgetenv("XDG_DATA_HOME"));
  if (base.isEmpty()) {
    base = QDir::homePath() + "/.local/share";
  }
  QDir dir(base + "/ohm-browser");
  dir.mkpath(".");
  return dir.absolutePath();
}

// Where QWebEngineProfile keeps cookies/localStorage/cache. Deliberately a
// new subpath, not the old Chromium --user-data-dir, since the on-disk
// formats aren't compatible.
inline QString webEngineStoragePath() { return dataHome() + "/profile/webengine"; }

// SQLite typed/visited history store, independent of the WebEngine profile.
inline QString historyDbPath() { return dataHome() + "/history.sqlite"; }

// SQLite download-tracking store (see DownloadManager). A separate file
// (and, per DownloadManager::open(), a separate QSqlDatabase connection
// name) from historyDbPath() -- unrelated data, no reason to share a file.
inline QString downloadsDbPath() { return dataHome() + "/downloads.sqlite"; }

// One dismissed PopularDomains domain per line -- the omnibox suggestion
// dropdown's per-row "x" button on a default (non-history) suggestion.
// Unlike a history entry (an actual SQLite row deleted outright), the
// baked-in domain list is a read-only Qt resource, so a dismissal is
// tracked as an exclusion list layered on top instead.
inline QString dismissedDomainsPath() { return dataHome() + "/dismissed_domains.txt"; }

// Camera/microphone answers per site. Qt only persists the other permission
// types itself (see PermissionBar).
inline QString mediaPermissionsPath() { return dataHome() + "/media_permissions.ini"; }

// Omarchy's per-theme color file.
inline QString colorsTomlPath() {
  return QDir::homePath() + "/.local/state/omarchy/current/theme/colors.toml";
}

// $XDG_CONFIG_HOME/ohm-browser/config.lua (~/.config/ohm-browser/config.lua by
// default) -- the user-editable Lua config file (search engine, etc; see
// Config.h). Unlike colorsTomlPath() this one is Ohm's own, not
// Omarchy-managed, and it's fine for it not to exist yet.
inline QString configLuaPath() {
  QString base = QString::fromLocal8Bit(qgetenv("XDG_CONFIG_HOME"));
  if (base.isEmpty()) {
    base = QDir::homePath() + "/.config";
  }
  return base + "/ohm-browser/config.lua";
}

// $XDG_RUNTIME_DIR/ohm-browser.sock — the singleton handoff socket.
inline QString singletonSocketPath() {
  QString runtime = QString::fromLocal8Bit(qgetenv("XDG_RUNTIME_DIR"));
  if (runtime.isEmpty()) {
    runtime = QDir::tempPath();
  }
  return runtime + "/ohm-browser.sock";
}

}  // namespace ohm
