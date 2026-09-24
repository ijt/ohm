#include "Notify.h"

#include <QProcess>

namespace ohm {

void notify(const QString &title, const QString &body, bool critical) {
  QProcess::startDetached(
      QStringLiteral("notify-send"),
      {QStringLiteral("-u"), critical ? QStringLiteral("critical") : QStringLiteral("normal"),
       QStringLiteral("-a"), QStringLiteral("Ohm"), title, body});
}

void notifyClickable(const QString &title, const QString &body, std::function<void()> onClicked) {
  // "default" is the freedesktop-notifications convention for "the user
  // clicked the notification body itself" (as opposed to a separate
  // action button) -- confirmed against Omarchy's own quickshell
  // notification service treating it that way. Its label ("Open" here)
  // only matters for a server that renders explicit action buttons
  // instead.
  auto *proc = new QProcess();
  QObject::connect(proc, &QProcess::readyReadStandardOutput, proc, [proc, onClicked]() {
    if (proc->readAllStandardOutput().trimmed() == "default") onClicked();
  });
  QObject::connect(proc, &QProcess::finished, proc, &QObject::deleteLater);
  proc->start(QStringLiteral("notify-send"),
              {QStringLiteral("-u"), QStringLiteral("normal"), QStringLiteral("-a"),
               QStringLiteral("Ohm"), QStringLiteral("-A"), QStringLiteral("default=Open"), title,
               body});
}

}  // namespace ohm
