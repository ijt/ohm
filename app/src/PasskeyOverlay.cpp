#include "PasskeyOverlay.h"

#include <QKeyEvent>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QVBoxLayout>

namespace ohm {

// The QR code itself. Always black on white with a quiet zone, whatever the
// theme: phone cameras are tuned for that, and inverted codes scan poorly.
class QrView : public QWidget {
 public:
  explicit QrView(QWidget *parent) : QWidget(parent) { setFixedSize(264, 264); }

  void setCode(int size, const QString &modules) {
    size_ = size;
    modules_ = modules;
    update();
  }

 protected:
  void paintEvent(QPaintEvent *) override {
    QPainter p(this);
    p.fillRect(rect(), Qt::white);
    if (size_ <= 0 || modules_.size() != size_ * size_) return;
    constexpr int kQuietZone = 4;  // Modules, per the QR spec.
    const int cell = width() / (size_ + 2 * kQuietZone);
    const int offset = (width() - cell * size_) / 2;
    for (int y = 0; y < size_; ++y) {
      for (int x = 0; x < size_; ++x) {
        if (modules_.at(y * size_ + x) == QLatin1Char('1')) {
          p.fillRect(offset + x * cell, offset + y * cell, cell, cell, Qt::black);
        }
      }
    }
  }

 private:
  int size_ = 0;
  QString modules_;
};

PasskeyOverlay::PasskeyOverlay(QWidget *parent) : QWidget(parent) {
  setObjectName(QStringLiteral("PasskeyOverlay"));
  setFocusPolicy(Qt::StrongFocus);
  hide();

  card_ = new QWidget(this);
  card_->setObjectName(QStringLiteral("PasskeyCard"));
  card_->setAttribute(Qt::WA_StyledBackground);
  auto *layout = new QVBoxLayout(card_);
  layout->setContentsMargins(28, 24, 28, 20);
  layout->setSpacing(10);

  title_ = new QLabel(card_);
  title_->setObjectName(QStringLiteral("PasskeyTitle"));
  site_ = new QLabel(card_);
  site_->setObjectName(QStringLiteral("PasskeySite"));
  qr_ = new QrView(card_);
  status_ = new QLabel(card_);
  status_->setObjectName(QStringLiteral("PasskeyStatus"));
  status_->setWordWrap(true);
  status_->setAlignment(Qt::AlignCenter);
  cancel_ = new QPushButton(QStringLiteral("Cancel"), card_);
  cancel_->setObjectName(QStringLiteral("PasskeyCancel"));
  cancel_->setCursor(Qt::PointingHandCursor);
  connect(cancel_, &QPushButton::clicked, this, &PasskeyOverlay::cancelled);

  layout->addWidget(title_, 0, Qt::AlignHCenter);
  layout->addWidget(site_, 0, Qt::AlignHCenter);
  layout->addSpacing(6);
  layout->addWidget(qr_, 0, Qt::AlignHCenter);
  layout->addWidget(status_);
  layout->addWidget(cancel_, 0, Qt::AlignHCenter);
  card_->setFixedWidth(360);
}

void PasskeyOverlay::applyPalette(const Palette &palette) {
  // Numbered from %1, one argument per placeholder -- see FindBar.
  setStyleSheet(QStringLiteral(
                    "#PasskeyCard { background: %1; border: 1px solid %3; border-radius: 8px; }"
                    "#PasskeyCard QLabel { color: %2; font-family: %4; }"
                    "#PasskeyTitle { font-size: 16px; font-weight: bold; }"
                    "#PasskeySite { color: %5; font-size: 13px; }"
                    "#PasskeyStatus { color: %3; font-size: 12px; }"
                    "#PasskeyCancel { color: %2; background: transparent; border: 1px solid %3;"
                    " border-radius: 4px; padding: 5px 18px; font-family: %4; }"
                    "#PasskeyCancel:hover { border-color: %5; }")
                    .arg(palette.card, palette.fg, palette.muted, palette.font, palette.accent));
  backdrop_ = palette.bg;
  update();
}

void PasskeyOverlay::begin(const QString &host, bool create) {
  busy_ = true;
  title_->setText(create ? QStringLiteral("Create a passkey on your phone")
                         : QStringLiteral("Sign in with your phone"));
  // The one thing the user must check before approving on the phone.
  site_->setText(host);
  qr_->setCode(0, QString());
  qr_->show();
  status_->setText(QStringLiteral("Starting…"));
  // Shown when the QR code arrives, not now: a request that fails
  // validation or finds no Bluetooth shouldn't flash a dialog.
}

void PasskeyOverlay::showQr(int size, const QString &modules) {
  if (!busy_) return;
  qr_->setCode(size, modules);
  // Unlocked, not just the camera from the lock screen: iOS will create a
  // passkey from a locked phone but refuses to sign in with one, and only
  // says "This passkey could not be used to sign in".
  status_->setText(QStringLiteral(
      "Unlock your phone, then scan with its Camera app.\n"
      "Bluetooth needs to be on for both devices."));
  setGeometry(parentWidget()->rect());
  placeCard();
  show();
  raise();
  setFocus();
}

void PasskeyOverlay::setStatus(const QString &status) {
  if (!busy_) return;
  if (status == QLatin1String("proximity")) return;  // Still waiting on the scan.
  if (status == QLatin1String("connecting") || status == QLatin1String("authenticating")) {
    qr_->hide();
    status_->setText(QStringLiteral("Connecting to your phone…"));
  } else if (status == QLatin1String("connected") || status == QLatin1String("touch")) {
    qr_->hide();
    status_->setText(QStringLiteral("Confirm on your phone."));
  }
  placeCard();  // The card shrinks without the QR code.
}

void PasskeyOverlay::end() {
  busy_ = false;
  hide();
  // Hand the keyboard back to the page underneath.
  if (parentWidget()) parentWidget()->setFocus();
}

void PasskeyOverlay::keyPressEvent(QKeyEvent *event) {
  if (event->key() == Qt::Key_Escape) {
    emit cancelled();
    return;
  }
  QWidget::keyPressEvent(event);
}

void PasskeyOverlay::paintEvent(QPaintEvent *) {
  QPainter p(this);
  QColor dim(backdrop_.isEmpty() ? QStringLiteral("#000000") : backdrop_);
  dim.setAlpha(200);
  p.fillRect(rect(), dim);
}

void PasskeyOverlay::resizeEvent(QResizeEvent *) { placeCard(); }

void PasskeyOverlay::placeCard() {
  // Room for the title, site, status and button (~260px) comes first; the
  // code shrinks to fit a short tiled window, down to what still scans.
  const int side = qBound(150, height() - 260, 264);
  qr_->setFixedSize(side, side);
  card_->adjustSize();
  card_->move((width() - card_->width()) / 2, (height() - card_->height()) / 2);
}

}  // namespace ohm
