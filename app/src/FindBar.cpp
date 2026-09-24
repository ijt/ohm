#include "FindBar.h"

#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QToolButton>

#include "ReadlineEditing.h"

namespace ohm {

FindBar::FindBar(QWidget *parent) : QWidget(parent) {
  setObjectName(QStringLiteral("FindBar"));
  setAttribute(Qt::WA_StyledBackground, true);
  hide();

  auto *layout = new QHBoxLayout(this);
  layout->setContentsMargins(10, 4, 6, 4);
  layout->setSpacing(4);

  input_ = new QLineEdit(this);
  input_->setAttribute(Qt::WA_MacShowFocusRect, false);
  input_->installEventFilter(this);
  connect(input_, &QLineEdit::textChanged, this, &FindBar::searchChanged);
  layout->addWidget(input_, 1);

  countLabel_ = new QLabel(this);
  countLabel_->setObjectName(QStringLiteral("FindBarCount"));
  countLabel_->setMinimumWidth(48);
  countLabel_->setAlignment(Qt::AlignCenter);
  layout->addWidget(countLabel_);

  auto addButton = [this, layout](const QString &symbol, const QString &tooltip) {
    auto *btn = new QToolButton(this);
    btn->setObjectName(QStringLiteral("FindBarButton"));
    btn->setText(symbol);
    btn->setToolTip(tooltip);
    btn->setAutoRaise(true);
    btn->setCursor(Qt::PointingHandCursor);
    btn->setFocusPolicy(Qt::NoFocus);
    layout->addWidget(btn);
    return btn;
  };
  prevBtn_ = addButton(QString::fromUtf8("\xE2\x86\x91"), QStringLiteral("Previous match"));  // ↑
  nextBtn_ = addButton(QString::fromUtf8("\xE2\x86\x93"), QStringLiteral("Next match"));      // ↓
  closeBtn_ = addButton(QString::fromUtf8("\xC3\x97"), QStringLiteral("Close"));              // ×

  connect(prevBtn_, &QToolButton::clicked, this, &FindBar::findPrevious);
  connect(nextBtn_, &QToolButton::clicked, this, &FindBar::findNext);
  connect(closeBtn_, &QToolButton::clicked, this, [this] {
    hideBar();
    emit closed();
  });

  setMatchCount(0, 0);
  adjustSize();
}

void FindBar::applyPalette(const Palette &palette) {
  // QString::arg() binds sequentially to the *lowest-numbered placeholder
  // actually present in the string* -- not literally %1 -- so a template
  // that never uses %1 silently shifts every argument down one slot
  // (confirmed: this used to start at %2, and every color came out wrong,
  // with the font dropped entirely). Numbering from %1 with one argument
  // per placeholder used avoids the whole class of bug.
  setStyleSheet(QStringLiteral(
                    "#FindBar { background: %1; border: 1px solid %3; border-radius: 4px; }"
                    "#FindBar QLineEdit {"
                    " background: transparent; color: %2; border: none;"
                    " font-family: %4; font-size: 13px; }"
                    "#FindBar #FindBarCount { color: %3; font-family: %4; font-size: 12px; }"
                    "#FindBar #FindBarButton {"
                    " color: %3; background: transparent; border: none; font-family: %4; }"
                    "#FindBar #FindBarButton:hover { color: %2; }"
                    "#FindBar #FindBarButton:disabled { color: %3; }")
                    .arg(palette.card, palette.fg, palette.muted, palette.font));
}

void FindBar::showBar() {
  show();
  raise();
  input_->setFocus();
  input_->selectAll();
}

void FindBar::hideBar() { hide(); }

QString FindBar::searchText() const { return input_->text(); }

void FindBar::setMatchCount(int active, int total) {
  countLabel_->setText(total > 0 ? QStringLiteral("%1/%2").arg(active).arg(total)
                                  : QStringLiteral("0/0"));
  const bool hasMatches = total > 0;
  prevBtn_->setEnabled(hasMatches);
  nextBtn_->setEnabled(hasMatches);
}

bool FindBar::eventFilter(QObject *obj, QEvent *event) {
  // See OmniboxOverlay::eventFilter's matching comment: Ctrl+K/Ctrl+T/
  // Ctrl+W/Ctrl+F are also window-level QShortcuts, and reclaiming them
  // here while this box has focus is safe for the same reason -- each is
  // a no-op in that state anyway (they'd just refocus/reopen something
  // that's already open, or -- Ctrl+F -- reopen the find bar itself).
  if (obj == input_ && event->type() == QEvent::ShortcutOverride) {
    auto *key = static_cast<QKeyEvent *>(event);
    if (isReadlineEditKey(key)) {
      key->accept();
      return true;
    }
  }
  if (obj != input_ || event->type() != QEvent::KeyPress) {
    return QWidget::eventFilter(obj, event);
  }
  auto *key = static_cast<QKeyEvent *>(event);
  if (isReadlineEditKey(key)) {
    applyReadlineEdit(input_, key);
    return true;
  }
  switch (key->key()) {
    case Qt::Key_Escape:
      hideBar();
      emit closed();
      return true;
    case Qt::Key_Return:
    case Qt::Key_Enter:
      if (key->modifiers() & Qt::ShiftModifier) {
        emit findPrevious();
      } else {
        emit findNext();
      }
      return true;
    default:
      return QWidget::eventFilter(obj, event);
  }
}

}  // namespace ohm
