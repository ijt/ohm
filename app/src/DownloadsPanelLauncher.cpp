#include "DownloadsPanelLauncher.h"

#include <QProcess>

namespace shinto {

void showDownloadsPanel() {
  QProcess::startDetached(QStringLiteral("omarchy-shell"),
                           {QStringLiteral("shell"), QStringLiteral("toggle"),
                            QStringLiteral("shinto.downloads")});
}

void showShortcutsPanel() {
  QProcess::startDetached(QStringLiteral("omarchy-shell"),
                           {QStringLiteral("shell"), QStringLiteral("toggle"),
                            QStringLiteral("shinto.shortcuts")});
}

}  // namespace shinto
