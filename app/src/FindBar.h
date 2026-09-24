// Find-in-page bar: a small widget anchored to the top-right of a
// BrowserWindow's QWebEngineView, shown on Ctrl+F. QtWebEngine embeds
// Chromium's content layer, not its full browser chrome -- find-in-page
// has a C++ API (QWebEnginePage::findText()) but no built-in UI, so this
// is that UI. Deliberately dumb: it only knows about text/counts/buttons,
// emitting signals for BrowserWindow to actually drive findText() with --
// same separation as OmniboxOverlay/BrowserWindow.
#pragma once

#include <QWidget>

#include "ThemeLoader.h"

class QLabel;
class QLineEdit;
class QToolButton;

namespace ohm {

class FindBar : public QWidget {
  Q_OBJECT

 public:
  explicit FindBar(QWidget *parent = nullptr);

  void applyPalette(const Palette &palette);

  // Shows (or, if already visible, just refocuses) the bar, selecting any
  // existing search text -- matches Chrome's own Ctrl+F-while-open
  // behavior (retype, don't reopen).
  void showBar();

  // Hides the bar. Does not itself clear the page's highlighted matches
  // -- BrowserWindow does that (it owns the QWebEnginePage), on the
  // `closed` signal this emits.
  void hideBar();

  QString searchText() const;

  // Reflects a QWebEngineFindTextResult back into the "N/M" count label.
  // `total` 0 with a non-empty search text reads as "0/0" (no matches).
  void setMatchCount(int active, int total);

 signals:
  // The search text changed (live, as-you-type) -- empty means the bar
  // was cleared, not just closed.
  void searchChanged(const QString &text);
  // Enter, or the next/prev buttons.
  void findNext();
  void findPrevious();
  // Escape, or the close button. BrowserWindow clears highlighting and
  // returns focus to the page.
  void closed();

 protected:
  bool eventFilter(QObject *obj, QEvent *event) override;

 private:
  QLineEdit *input_;
  QLabel *countLabel_;
  QToolButton *prevBtn_;
  QToolButton *nextBtn_;
  QToolButton *closeBtn_;
};

}  // namespace ohm
