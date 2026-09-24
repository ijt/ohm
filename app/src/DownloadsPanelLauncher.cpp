#include "DownloadsPanelLauncher.h"

#include <QKeyEvent>
#include <QProcess>

namespace ohm {

void showDownloadsPanel() {
  QProcess::startDetached(QStringLiteral("omarchy-shell"),
                           {QStringLiteral("shell"), QStringLiteral("toggle"),
                            QStringLiteral("ohm-browser.downloads")});
}

void showShortcutsPanel() {
  QProcess::startDetached(QStringLiteral("omarchy-shell"),
                           {QStringLiteral("shell"), QStringLiteral("toggle"),
                            QStringLiteral("ohm-browser.shortcuts")});
}

bool isShortcutsPanelKey(const QKeyEvent *key) {
  const auto mods =
      key->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier | Qt::AltModifier | Qt::MetaModifier);
  if (mods == Qt::NoModifier && key->key() == Qt::Key_F1) return true;
  if (mods != Qt::ControlModifier && mods != (Qt::ControlModifier | Qt::ShiftModifier)) return false;
  return key->key() == Qt::Key_Slash || key->key() == Qt::Key_Question;
}

}  // namespace ohm
