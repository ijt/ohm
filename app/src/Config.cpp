#include "Config.h"

#include <QDebug>
#include <QFile>
#include <QUrl>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

#include "Notify.h"
#include "Ohm.h"

namespace ohm {

namespace {

// A broken config should be loud, not a silent fallback the user never
// learns about -- see Notify.h for why this is more than a qWarning.
void reportConfigError(const QString &message) {
  qWarning() << "ohm:" << message;
  notify(QStringLiteral("Ohm config.lua error"), message, /*critical=*/true);
}

}  // namespace

OhmConfig loadConfig() {
  OhmConfig config;
  const QString path = configLuaPath();
  if (!QFile::exists(path)) {
    return config;  // no config.lua yet -- nothing to report
  }

  lua_State *L = luaL_newstate();
  luaL_openlibs(L);
  if (luaL_dofile(L, path.toLocal8Bit().constData()) != LUA_OK) {
    reportConfigError(QStringLiteral("%1 -- using defaults for everything")
                           .arg(QString::fromUtf8(lua_tostring(L, -1))));
    lua_close(L);
    return config;
  }

  lua_getglobal(L, "search_engine");
  if (lua_isstring(L, -1)) {
    const QString value = QString::fromUtf8(lua_tostring(L, -1));
    if (value.contains(QStringLiteral("%s"))) {
      config.searchEngineUrl = value;
    } else {
      reportConfigError(QStringLiteral("search_engine (%1) has no %s query placeholder -- "
                                        "using the default search engine instead")
                             .arg(value));
    }
  } else if (!lua_isnil(L, -1)) {
    reportConfigError(QStringLiteral(
        "search_engine must be a string -- using the default search engine instead"));
  }
  lua_pop(L, 1);

  lua_getglobal(L, "back_shortcut");
  if (lua_isstring(L, -1)) {
    const QString value = QString::fromUtf8(lua_tostring(L, -1));
    const QKeySequence seq(value);
    // QKeySequence(QString)'s isEmpty() alone isn't a reliable validity
    // check -- garbage input like "not a real shortcut???" still parses
    // to a non-empty-but-nonsensical sequence (confirmed: isEmpty()
    // false, but toString() back out is ""). A valid single-combination
    // shortcut round-trips to a non-empty display string; count() == 1
    // additionally rejects a syntactically-valid multi-key chord
    // ("Left Right"), which isn't a sensible shape for this setting.
    if (!seq.isEmpty() && seq.count() == 1 && !seq.toString().isEmpty()) {
      config.backShortcut = seq;
    } else {
      reportConfigError(QStringLiteral("back_shortcut (%1) is not a recognized key sequence -- "
                                        "using the default (Alt+Left) instead")
                             .arg(value));
    }
  } else if (!lua_isnil(L, -1)) {
    reportConfigError(QStringLiteral(
        "back_shortcut must be a string -- using the default (Alt+Left) instead"));
  }
  lua_pop(L, 1);

  lua_close(L);
  return config;
}

}  // namespace ohm
