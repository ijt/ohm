#include "Hyprland.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QString>

namespace ohm {

void ensureActiveWindowGrouped() {
  // Hyprland 0.56's dispatch surface is Lua (hl.dsp.*). A window's
  // `.group` is nil when ungrouped; togglegroup on a lone window creates
  // a one-window group. hyprctl is a local socket call -- bound so a
  // stuck compositor can't hang the UI. QProcess args, not a shell, so
  // the Lua is one argument as-is.
  QProcess proc;
  proc.start(QStringLiteral("hyprctl"),
             {QStringLiteral("eval"),
              QStringLiteral("local w = hl.get_active_window(); "
                             "if w and not w.group then "
                             "hl.dispatch(hl.dsp.group.toggle()) "
                             "end")});
  if (!proc.waitForFinished(500)) proc.kill();
}

void queryActiveWindow(QObject *context,
                       std::function<void(const QString &address, qint64 pid)> done) {
  auto *proc = new QProcess(context);
  QObject::connect(proc, &QProcess::finished, context, [proc, done] {
    const QJsonObject w = QJsonDocument::fromJson(proc->readAllStandardOutput()).object();
    proc->deleteLater();
    done(w.value(QStringLiteral("address")).toString(),
         w.value(QStringLiteral("pid")).toInteger());
  });
  QObject::connect(proc, &QProcess::errorOccurred, context, [proc, done](QProcess::ProcessError e) {
    if (e != QProcess::FailedToStart) return;
    proc->deleteLater();
    done(QString(), 0);
  });
  proc->start(QStringLiteral("hyprctl"), {QStringLiteral("activewindow"), QStringLiteral("-j")});
}

bool focusWindow(const QString &address) {
  // The address goes into Lua source, so only ever a hex literal.
  static const QRegularExpression kAddress(QStringLiteral("^0x[0-9a-fA-F]+$"));
  if (!kAddress.match(address).hasMatch()) return false;
  QProcess proc;
  proc.start(QStringLiteral("hyprctl"),
             {QStringLiteral("eval"),
              QStringLiteral("hl.dispatch(hl.dsp.focus({window = 'address:%1'}))").arg(address)});
  if (!proc.waitForFinished(500)) {
    proc.kill();
    return false;
  }
  return proc.exitCode() == 0 && proc.readAllStandardOutput().trimmed() == "ok";
}

}  // namespace ohm
