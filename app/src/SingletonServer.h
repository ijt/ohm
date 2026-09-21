// Daemon-side half of the singleton handoff. Listens on
// shinto::singletonSocketPath() for line commands from SingletonClient.
// Replaces control-server.py's /open endpoint and Chromium's own
// SingletonSocket/SingletonLock.
#pragma once

#include <QObject>
#include <QString>

class QLocalServer;

namespace shinto {

class SingletonServer : public QObject {
  Q_OBJECT

 public:
  explicit SingletonServer(QObject *parent = nullptr);

  // Starts listening at shinto::singletonSocketPath(), clearing any stale
  // socket file left by an unclean shutdown first. Returns false on failure
  // (e.g. another daemon is genuinely already running).
  bool listen();

 signals:
  // "OPEN <url>" (url may be empty -> open the empty gate).
  void openRequested(const QString &url);
  // "THEME" -> re-read colors.toml and re-apply to every open window.
  void themeReloadRequested();
  // "CANCEL_DOWNLOAD <id>" -- the downloads panel's helper script
  // (downloads-panel/downloads_helper.py) sends this directly over this
  // same socket (a raw AF_UNIX connection, no Qt on that end) since
  // cancelling a live QWebEngineDownloadRequest can only happen in this
  // process; the panel otherwise only ever reads downloads.sqlite, no
  // other IPC with the daemon.
  void cancelDownloadRequested(int id);

 private:
  void handleNewConnection();

  QLocalServer *server_;
};

}  // namespace shinto
