#include "WebNotifications.h"

#include <memory>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QList>
#include <QProcess>
#include <QStandardPaths>
#include <QUrl>
#include <QWebEngineNotification>
#include <QWebEngineProfile>

namespace ohm {

namespace {

// notify-send wants a file for a custom icon. Written under the runtime
// dir, removed when the notification is gone.
QString writeIcon(const QImage &icon) {
  if (icon.isNull()) return {};
  static int counter = 0;
  const QString dir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
  if (dir.isEmpty()) return {};
  const QString path = QDir(dir).filePath(QStringLiteral("ohm-notification-%1-%2.png")
                                              .arg(QCoreApplication::applicationPid())
                                              .arg(++counter));
  return icon.save(path, "PNG") ? path : QString();
}

// Notifications still on screen, for replacing: Qt tells the page its old
// one closed when a newer one with the same tag arrives, but leaves the
// desktop popup to the presenter (QWebEngineNotification::matches()).
struct Live {
  std::weak_ptr<QWebEngineNotification> notification;
  std::shared_ptr<QString> id;
};

QList<Live> &live() {
  static QList<Live> list;
  return list;
}

void closeDesktopNotification(const QString &id) {
  // No CLI for CloseNotification; gdbus ships with glib, which every
  // notify-send depends on anyway.
  QProcess::startDetached(
      QStringLiteral("gdbus"),
      {QStringLiteral("call"), QStringLiteral("--session"), QStringLiteral("--dest"),
       QStringLiteral("org.freedesktop.Notifications"), QStringLiteral("--object-path"),
       QStringLiteral("/org/freedesktop/Notifications"), QStringLiteral("--method"),
       QStringLiteral("org.freedesktop.Notifications.CloseNotification"), id});
}

void present(std::unique_ptr<QWebEngineNotification> owned,
             const std::function<void(const QUrl &)> &focusOrigin) {
  // Shared so the process callbacks below can keep it alive until the
  // desktop notification is gone; the page's `closed` needs it until then.
  std::shared_ptr<QWebEngineNotification> n(std::move(owned));
  for (auto it = live().begin(); it != live().end();) {
    const auto previous = it->notification.lock();
    if (!previous) {
      it = live().erase(it);
    } else if (previous->matches(n.get())) {
      if (!it->id->isEmpty()) closeDesktopNotification(*it->id);
      it = live().erase(it);
    } else {
      ++it;
    }
  }
  const QString iconPath = writeIcon(n->icon());

  QStringList args = {QStringLiteral("--app-name"), n->origin().host(),
                      QStringLiteral("--print-id"), QStringLiteral("--action"),
                      QStringLiteral("default=Open")};
  if (!iconPath.isEmpty()) args << QStringLiteral("--icon") << iconPath;
  args << n->title() << n->message();

  // --action makes notify-send wait: it prints the id, then the action
  // name if the notification is clicked, and exits when it's closed.
  auto *proc = new QProcess();
  auto id = std::make_shared<QString>();
  QObject::connect(proc, &QProcess::readyReadStandardOutput, proc, [proc, n, id, focusOrigin] {
    while (proc->canReadLine()) {
      const QString line = QString::fromUtf8(proc->readLine()).trimmed();
      if (id->isEmpty()) {
        *id = line;
      } else if (line == QLatin1String("default")) {
        n->click();
        focusOrigin(n->origin());
      }
    }
  });
  QObject::connect(proc, &QProcess::finished, proc, [proc, n, iconPath] {
    if (!iconPath.isEmpty()) QFile::remove(iconPath);
    n->close();  // Tells the page (onclose); dismissed or timed out.
    proc->deleteLater();
  });
  // The page closing its notification (notification.close()) takes the
  // desktop one down too -- when Qt says so: 6.11 doesn't emit `closed`
  // for a page's own close(), so those popups just time out.
  QObject::connect(n.get(), &QWebEngineNotification::closed, proc, [id] {
    if (!id->isEmpty()) closeDesktopNotification(*id);
  });
  live().append({n, id});
  proc->start(QStringLiteral("notify-send"), args);
  n->show();  // Tells the page (onshow).
}

}  // namespace

void installNotificationPresenter(QWebEngineProfile *profile,
                                  std::function<void(const QUrl &origin)> focusOrigin) {
  profile->setNotificationPresenter(
      [focusOrigin](std::unique_ptr<QWebEngineNotification> n) { present(std::move(n), focusOrigin); });
}

}  // namespace ohm
