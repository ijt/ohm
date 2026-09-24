#include "SingletonServer.h"

#include <QLocalServer>
#include <QLocalSocket>

#include "Ohm.h"

namespace ohm {

SingletonServer::SingletonServer(QObject *parent) : QObject(parent), server_(new QLocalServer(this)) {}

bool SingletonServer::listen() {
  const QString path = singletonSocketPath();
  // A prior daemon that didn't shut down cleanly can leave a stale socket
  // file behind; QLocalServer::removeServer() clears it if nothing is
  // actually listening there.
  QLocalServer::removeServer(path);
  connect(server_, &QLocalServer::newConnection, this, &SingletonServer::handleNewConnection);
  return server_->listen(path);
}

void SingletonServer::handleNewConnection() {
  while (QLocalSocket *socket = server_->nextPendingConnection()) {
    connect(socket, &QLocalSocket::readyRead, this, [this, socket]() {
      const QString line = QString::fromUtf8(socket->readLine()).trimmed();
      if (line == QStringLiteral("PING")) {
        socket->write("PONG\n");
      } else if (line == QStringLiteral("THEME")) {
        emit themeReloadRequested();
        socket->write("OK\n");
      } else if (line.startsWith(QStringLiteral("OPEN"))) {
        const QString url = line.mid(4).trimmed();
        emit openRequested(url);
        socket->write("OK\n");
      } else if (line.startsWith(QStringLiteral("COMMAND "))) {
        emit commandRequested(line.mid(8).trimmed());
        socket->write("OK\n");
      } else if (line.startsWith(QStringLiteral("CANCEL_DOWNLOAD "))) {
        bool ok = false;
        const int id = line.mid(16).trimmed().toInt(&ok);
        if (ok) emit cancelDownloadRequested(id);
        socket->write("OK\n");
      }
      socket->flush();
      socket->disconnectFromServer();
    });
    connect(socket, &QLocalSocket::disconnected, socket, &QLocalSocket::deleteLater);
  }
}

}  // namespace ohm
