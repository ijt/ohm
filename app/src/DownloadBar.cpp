#include "DownloadBar.h"

#include <QLabel>
#include <QMouseEvent>
#include <QResizeEvent>

namespace ohm {

namespace {
constexpr int kHeight = 28;
}  // namespace

DownloadBar::DownloadBar(QWidget *parent) : QWidget(parent) {
  setObjectName(QStringLiteral("DownloadBar"));
  setAttribute(Qt::WA_StyledBackground, true);
  setFixedHeight(kHeight);
  hide();

  // Fill first, label on top -- same "hand-positioned, no QLayout" trick
  // as OmniboxOverlay's progressBar_, but spanning the whole bar's height
  // as the primary visual rather than a thin top strip, since this bar's
  // only job is showing download progress.
  fill_ = new QWidget(this);
  fill_->setObjectName(QStringLiteral("DownloadBarFill"));
  fill_->setAttribute(Qt::WA_StyledBackground, true);
  fill_->setAttribute(Qt::WA_TransparentForMouseEvents);

  label_ = new QLabel(this);
  label_->setObjectName(QStringLiteral("DownloadBarLabel"));
  label_->setAttribute(Qt::WA_TransparentForMouseEvents);
  label_->setContentsMargins(10, 0, 10, 0);
}

void DownloadBar::applyPalette(const Palette &palette) {
  // QString::arg() binds to the lowest-numbered placeholder present, not
  // literally %1 -- see FindBar::applyPalette's comment for the bug this
  // avoids. Numbered from %1 with one argument per placeholder used.
  setStyleSheet(QStringLiteral("#DownloadBar { background: %1; }"
                                "#DownloadBar #DownloadBarFill { background: %2; }"
                                "#DownloadBar #DownloadBarLabel {"
                                " background: transparent; color: %3; font-family: %4; font-size: 12px; }")
                    .arg(palette.card, palette.accent, palette.fg, palette.font));
}

void DownloadBar::showFor(const DownloadManager::DownloadRecord &record) {
  // QStringLiteral is a UTF-16 literal -- a raw UTF-8 byte escape
  // (\xE2\x80\x94, as FindBar.cpp's QString::fromUtf8() calls correctly
  // use for a `const char*` literal instead) would decode as three
  // garbage UTF-16 code units here, not one em dash (confirmed: exactly
  // the "â??" seen in the bar). — is the right escape for a UTF-16
  // string literal.
  if (record.totalBytes > 0) {
    qint64 pct = record.receivedBytes * 100 / record.totalBytes;
    percent_ = static_cast<int>(pct < 0 ? 0 : (pct > 100 ? 100 : pct));
    label_->setText(QStringLiteral("%1 — %2%").arg(record.filename).arg(percent_));
  } else {
    // Some sources (a data: URL, a server with no Content-Length) never
    // report a total -- show bytes-so-far instead of an unknown percent,
    // and leave the fill at 0 rather than guessing.
    percent_ = 0;
    label_->setText(QStringLiteral("%1 — %2 MB")
                         .arg(record.filename)
                         .arg(record.receivedBytes / (1024.0 * 1024.0), 0, 'f', 1));
  }
  layoutChildren();
  show();
  raise();
}

void DownloadBar::hideBar() { hide(); }

void DownloadBar::mousePressEvent(QMouseEvent * /*event*/) { emit clicked(); }

void DownloadBar::resizeEvent(QResizeEvent *event) {
  QWidget::resizeEvent(event);
  layoutChildren();
}

void DownloadBar::layoutChildren() {
  label_->setGeometry(0, 0, width(), height());
  fill_->setGeometry(0, 0, width() * percent_ / 100, height());
}

}  // namespace ohm
