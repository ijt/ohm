// A slim horizontal progress bar overlay anchored to the very bottom of a
// BrowserWindow, shown only while a download is actively in progress (see
// BrowserWindow::refreshDownloadBar()) -- minimalism is the default, so
// there is deliberately no idle/empty state to show. Same "deliberately
// dumb, BrowserWindow drives it" shape as FindBar/OmniboxOverlay: this
// widget only knows how to display one DownloadManager::DownloadRecord and
// emit a click.
#pragma once

#include <QWidget>

#include "DownloadManager.h"
#include "ThemeLoader.h"

class QLabel;

namespace ohm {

class DownloadBar : public QWidget {
  Q_OBJECT

 public:
  explicit DownloadBar(QWidget *parent = nullptr);

  void applyPalette(const Palette &palette);

  // Updates the label/fill for `record` and shows the bar (show()+raise()
  // -- it's a bare-positioned overlay, not layout-managed, same as
  // FindBar/OmniboxOverlay).
  void showFor(const DownloadManager::DownloadRecord &record);

  void hideBar();

 signals:
  // The whole bar is one big click target -- opens the downloads window.
  void clicked();

 protected:
  void mousePressEvent(QMouseEvent *event) override;
  void resizeEvent(QResizeEvent *event) override;

 private:
  void layoutChildren();

  QLabel *label_;
  QWidget *fill_;
  int percent_ = 0;  // 0 when totalBytes is unknown (e.g. no Content-Length)
};

}  // namespace ohm
