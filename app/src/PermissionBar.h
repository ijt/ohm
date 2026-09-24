// "example.com wants to use your microphone  [Block] [Allow]": the prompt
// for a page's permission requests (QWebEnginePage::permissionRequested).
// Unanswered, Qt leaves the request pending forever -- a page awaiting
// Notification.requestPermission() or getUserMedia() just hangs -- so
// every request gets an answer here. A strip across the top of the page,
// over it rather than pushing it down, one request at a time.
//
// Answers persist: the profile stores grants and denials per origin
// (QWebEngineProfile's default StoreOnDisk policy), so a site asks once.
#pragma once

#include <QList>
#include <QWebEnginePermission>
#include <QWidget>

#include "ThemeLoader.h"

class QLabel;
class QPushButton;

namespace ohm {

class PermissionBar : public QWidget {
  Q_OBJECT

 public:
  explicit PermissionBar(QWidget *parent = nullptr);

  void applyPalette(const Palette &palette);

  // Answers at once what needs no question; queues the rest.
  void request(const QWebEnginePermission &permission);

 signals:
  // Shown or hidden; the owner re-lays it out.
  void visibilityChanged();

 private:
  void showNext();
  void answer(bool allow);

  QList<QWebEnginePermission> queue_;
  QLabel *text_;
  QPushButton *block_;
  QPushButton *allow_;
};

}  // namespace ohm
