// The "use your phone" prompt for a passkey ceremony (see PasskeyBroker):
// a card over the page with the QR code to scan, which site is asking, and
// progress as the phone connects. Covers the whole page, like the omnibox
// gate, so the page can't be clicked into mid-ceremony.
#pragma once

#include <QString>
#include <QWidget>

#include "ThemeLoader.h"

class QLabel;
class QPushButton;

namespace shinto {

class QrView;

class PasskeyOverlay : public QWidget {
  Q_OBJECT

 public:
  explicit PasskeyOverlay(QWidget *parent = nullptr);

  void applyPalette(const Palette &palette);

  // Shows the card for a ceremony from `host`; `create` is registering a
  // new passkey rather than signing in with one.
  void begin(const QString &host, bool create);
  // `modules` is size*size '0'/'1' characters, row-major, '1' = dark.
  void showQr(int size, const QString &modules);
  // A shinto-passkey status word: proximity, connecting, authenticating,
  // connected, touch.
  void setStatus(const QString &status);
  // Hides the card. Does not emit cancelled().
  void end();

  bool isBusy() const { return busy_; }

 signals:
  // Cancel button or Escape.
  void cancelled();

 protected:
  void keyPressEvent(QKeyEvent *event) override;
  void paintEvent(QPaintEvent *event) override;
  void resizeEvent(QResizeEvent *event) override;

 private:
  // Centres the card, at its current content size.
  void placeCard();

  bool busy_ = false;
  QWidget *card_;
  QLabel *title_;
  QLabel *site_;
  QrView *qr_;
  QLabel *status_;
  QPushButton *cancel_;
  QString backdrop_;
};

}  // namespace shinto
