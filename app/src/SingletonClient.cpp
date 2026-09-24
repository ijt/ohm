#include "SingletonClient.h"

#include <QLocalSocket>

#include "Ohm.h"

namespace ohm {

bool SingletonClient::tryHandoff(const QString &commandLine, int timeoutMs) {
  QLocalSocket socket;
  socket.connectToServer(singletonSocketPath());
  if (!socket.waitForConnected(timeoutMs)) {
    return false;
  }
  const QByteArray payload = (commandLine + "\n").toUtf8();
  const qint64 written = socket.write(payload);
  if (written != payload.size()) {
    return false;
  }
  // A write to a local socket this small typically completes synchronously,
  // so waitForBytesWritten() legitimately returns false here (nothing left
  // pending to wait for) even though the write already succeeded -- its
  // return value is not a success signal, unlike waitForConnected()'s.
  socket.waitForBytesWritten(timeoutMs);
  socket.waitForReadyRead(timeoutMs);  // best-effort ack; don't fail hard without one
  socket.disconnectFromServer();
  return true;
}

}  // namespace ohm
