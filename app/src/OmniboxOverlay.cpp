#include "OmniboxOverlay.h"

#include <algorithm>

#include <QColor>
#include <QEvent>
#include <QGraphicsOpacityEffect>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMouseEvent>
#include <QPainter>
#include <QPen>
#include <QPropertyAnimation>
#include <QResizeEvent>
#include <QSet>
#include <QSignalBlocker>
#include <QTimer>
#include <QToolButton>

#include "ReadlineEditing.h"

namespace shinto {

// Accent-colored arc next to the URL, kept spinning until the gate hides.
// Chromium's loadProgress hits 100% before it has a frame to paint, so the
// top bar vanishes while the overlay is still up -- this is the "something
// is still happening" cue for that gap (and for the rest of the load once
// the URL shimmer has handed off).
class Spinner : public QWidget {
 public:
  explicit Spinner(QWidget *parent = nullptr) : QWidget(parent) {
    setFixedSize(kSize, kSize);
    hide();
    connect(&timer_, &QTimer::timeout, this, [this] {
      angle_ = (angle_ + 10) % 360;
      update();
    });
    timer_.setInterval(30);
  }

  void setColor(const QColor &color) {
    color_ = color;
    update();
  }

  void start() {
    if (!timer_.isActive()) timer_.start();
    show();
  }

  void stop() {
    timer_.stop();
    hide();
  }

 protected:
  void paintEvent(QPaintEvent *) override {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    constexpr int kPad = 2;
    const QRectF r = QRectF(rect()).adjusted(kPad, kPad, -kPad, -kPad);
    QPen pen(color_, 2.0, Qt::SolidLine, Qt::RoundCap);
    p.setPen(pen);
    // Qt angles: 1/16th of a degree, 0 at 3 o'clock, positive CCW.
    p.drawArc(r, -angle_ * 16, 270 * 16);
  }

 private:
  static constexpr int kSize = 16;
  QTimer timer_;
  QColor color_ = Qt::white;
  int angle_ = 0;
};

namespace {
constexpr int kMargin = 24;
// A generous safety cap, not a target -- maxSuggestionRows() below almost
// always lands well under this on any real window size; it just stops an
// absurdly tall window (or a mis-measured row height) from turning into
// hundreds of suggestions / an oversized domain-list query.
constexpr int kMaxSuggestionsCap = 100;
// Rough estimate of a suggestion row's height, used only to decide how
// many rows can fit -- doesn't need to be exact (layoutInput() measures
// the real thing once rows exist), just close enough that the list
// roughly fills the window without leaving a large empty gap or
// overflowing it.
constexpr int kApproxRowHeight = 30;
}  // namespace

OmniboxOverlay::OmniboxOverlay(HistoryStore *history, PopularDomains *domains,
                                const ShintoConfig *config, QWidget *parent)
    : QWidget(parent), history_(history), domains_(domains), config_(config) {
  setObjectName(QStringLiteral("OmniboxOverlay"));
  // A plain QWidget doesn't paint its stylesheet background by default --
  // without this, the page underneath shows through everywhere except
  // exactly where a child widget sits.
  setAttribute(Qt::WA_StyledBackground, true);

  input_ = new QLineEdit(this);
  input_->setAttribute(Qt::WA_MacShowFocusRect, false);
  input_->setPlaceholderText(QStringLiteral("search or url"));
  input_->installEventFilter(this);
  connect(input_, &QLineEdit::textChanged, this, &OmniboxOverlay::updateSuggestions);

  list_ = new QListWidget(this);
  list_->setFocusPolicy(Qt::NoFocus);
  list_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  list_->hide();

  progressBar_ = new QWidget(this);
  progressBar_->setObjectName(QStringLiteral("ProgressBar"));
  progressBar_->hide();
  spinner_ = new Spinner(this);
  // No QLayout -- position is managed by hand in layoutInput().

  // Loading a real page has some inherent latency (DNS, connect, TLS)
  // before Chromium reports the first loadProgress tick -- the shimmer is
  // immediate feedback for that gap, submitted at the exact moment Enter
  // is pressed, independent of the network. setProgress() takes over and
  // stops it once real progress data arrives.
  inputOpacity_ = new QGraphicsOpacityEffect(input_);
  inputOpacity_->setOpacity(1.0);
  input_->setGraphicsEffect(inputOpacity_);
  shimmer_ = new QPropertyAnimation(inputOpacity_, "opacity", this);
  shimmer_->setDuration(700);
  shimmer_->setStartValue(1.0);
  shimmer_->setKeyValueAt(0.5, 0.35);
  shimmer_->setEndValue(1.0);
  shimmer_->setLoopCount(-1);
}

void OmniboxOverlay::applyPalette(const Palette &palette) {
  currentPalette_ = palette;
  setStyleSheet(palette.toQss());
  QPalette pal = input_->palette();
  pal.setColor(QPalette::PlaceholderText, QColor(palette.muted));
  input_->setPalette(pal);
  spinner_->setColor(QColor(palette.accent));
  updateSuggestionSelectionStyle();
}

void OmniboxOverlay::showGate(const QString &prefill) {
  input_->setText(prefill);
  clearSuggestions();
  progress_ = 0;
  progressBar_->hide();
  stopShimmer();
  stopSpinner();
  layoutInput();
  show();
  raise();
  input_->setFocus();
  if (!prefill.isEmpty()) {
    input_->selectAll();
  }
}

void OmniboxOverlay::showLoading(const QString &url) {
  {
    const QSignalBlocker blocker(input_);
    input_->setText(url);
    input_->setCursorPosition(input_->text().length());
  }
  clearSuggestions();
  progress_ = 0;
  progressBar_->hide();
  stopSpinner();
  layoutInput();
  show();
  raise();
  startShimmer();
  input_->setFocus();
}

void OmniboxOverlay::hideOverlay() {
  clearSuggestions();
  stopShimmer();
  stopSpinner();
  hide();
}

void OmniboxOverlay::setProgress(int percent) {
  progress_ = qBound(0, percent, 100);
  // Real progress data has arrived -- the bar takes over from the URL
  // shimmer, and the spinner stays until hideOverlay() because Chromium
  // reports 100% (bar hides) before it has a frame to paint.
  if (progress_ > 0) {
    stopShimmer();
    startSpinner();
  }
  progressBar_->setVisible(progress_ > 0 && progress_ < 100 && isVisible());
  layoutProgressBar();
}

void OmniboxOverlay::startShimmer() {
  inputOpacity_->setOpacity(1.0);
  shimmer_->start();
}

void OmniboxOverlay::stopShimmer() {
  shimmer_->stop();
  inputOpacity_->setOpacity(1.0);
}

void OmniboxOverlay::startSpinner() {
  if (!isVisible()) return;
  spinner_->start();
  layoutInput();
}

void OmniboxOverlay::stopSpinner() {
  spinner_->stop();
  layoutInput();
}

void OmniboxOverlay::resizeEvent(QResizeEvent *event) {
  QWidget::resizeEvent(event);
  layoutInput();
}

int OmniboxOverlay::suggestionListHeight() const {
  if (items_.isEmpty()) return 0;
  const int rowHeight = list_->sizeHintForRow(0);
  return rowHeight * items_.size() + 2 * list_->frameWidth();
}

void OmniboxOverlay::layoutInput() {
  // Full window width (minus margins) to type into -- was capped at a
  // modest fixed width, which made a long URL or search query scroll
  // inside a narrow field instead of just being visible. Suggestions drop
  // down below it, matching this same width. The spinner, when shown,
  // sits on the right of the field so a long URL doesn't cover it.
  const int h = input_->sizeHint().height();
  const int w = qMax(0, width() - 2 * kMargin);
  int inputW = w;
  if (spinner_->isVisible()) {
    constexpr int kGap = 10;
    const int spinnerY = kMargin + (h - spinner_->height()) / 2;
    spinner_->setGeometry(kMargin + w - spinner_->width(), spinnerY, spinner_->width(),
                          spinner_->height());
    inputW = qMax(0, w - spinner_->width() - kGap);
  }
  input_->setGeometry(kMargin, kMargin, inputW, h);
  list_->setGeometry(kMargin, kMargin + h + 4, w, suggestionListHeight());
  list_->setVisible(!items_.isEmpty());
  layoutProgressBar();
}

int OmniboxOverlay::maxSuggestionRows() const {
  const int inputHeight = input_->sizeHint().height();
  const int available = height() - kMargin - inputHeight - 4 - kMargin;
  return qBound(1, available / kApproxRowHeight, kMaxSuggestionsCap);
}

void OmniboxOverlay::layoutProgressBar() {
  constexpr int kBarHeight = 3;
  progressBar_->setGeometry(0, 0, width() * progress_ / 100, kBarHeight);
}

bool OmniboxOverlay::eventFilter(QObject *obj, QEvent *event) {
  // Hovering a row's dismiss button highlights the whole row (obj is the
  // QToolButton -- its parent is the row).
  if (event->type() == QEvent::Enter || event->type() == QEvent::Leave) {
    if (auto *dismiss = qobject_cast<QToolButton *>(obj)) {
      setRowHovered(dismiss->parentWidget(), event->type() == QEvent::Enter);
      return false;  // don't swallow it -- the button's own hover/cursor still applies
    }
  }
  // Clicking a row anywhere but the dismiss button navigates there
  // directly (obj is the row itself -- see renderSuggestions()).
  if (event->type() == QEvent::MouseButtonRelease) {
    auto *mouseEvent = static_cast<QMouseEvent *>(event);
    if (mouseEvent->button() == Qt::LeftButton) {
      const QVariant urlProp = obj->property("suggestionUrl");
      if (urlProp.isValid()) {
        // Match what Tab/arrow-selecting a suggestion already does
        // (applySelectionToInput()) -- without this, the input keeps
        // showing whatever was typed, and the shimmer plays on that
        // stale text instead of what's actually about to load. Blocked
        // for the same reason applySelectionToInput() blocks it: setText()
        // fires textChanged -> updateSuggestions() -> rebuilds this very
        // list (and deletes `obj`, the row this event was delivered to)
        // synchronously, before navigateTo() below gets a chance to.
        const QVariant labelProp = obj->property("suggestionLabel");
        if (labelProp.isValid()) {
          const QSignalBlocker blocker(input_);
          input_->setText(labelProp.toString());
          input_->setCursorPosition(input_->text().length());
        }
        navigateTo(urlProp.toString(), QString());
        return true;
      }
    }
  }
  // GNU-readline-style editing (Ctrl+A/E/K/U/W/Y/T, Alt+B/F/D, ...): some
  // of these combos (Ctrl+K, Ctrl+T, Ctrl+F) are also window-level
  // QShortcuts (edit address / new page / find) -- ShortcutOverride is
  // Qt's mechanism for a focused widget to reclaim a key before the
  // shortcut system sees it, and it's safe to reclaim here specifically
  // because all three of those shortcuts are no-ops anyway once input_
  // already has focus (edit-address/new-page just (re)focus an input
  // that's already focused; find doesn't apply while typing a URL).
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
    case Qt::Key_Down:
      moveSelection(1);
      return true;
    case Qt::Key_Up:
      moveSelection(-1);
      return true;
    case Qt::Key_Tab:
      moveSelection(1);
      return true;
    case Qt::Key_Backtab:  // Shift+Tab
      moveSelection(-1);
      return true;
    case Qt::Key_Escape:
      if (!items_.isEmpty()) {
        clearSuggestions();
      } else {
        emit cancelled();
      }
      return true;
    case Qt::Key_Return:
    case Qt::Key_Enter:
      submit();
      return true;
    default:
      return QWidget::eventFilter(obj, event);
  }
}

void OmniboxOverlay::moveSelection(int delta) {
  if (items_.isEmpty()) return;
  int cur = list_->currentRow();
  if (cur < 0) {
    cur = delta > 0 ? 0 : items_.size() - 1;
  } else {
    cur = (cur + delta + items_.size()) % items_.size();
  }
  list_->setCurrentRow(cur);
  applySelectionToInput();
  updateSuggestionSelectionStyle();
}

void OmniboxOverlay::applySelectionToInput() {
  const int row = list_->currentRow();
  if (row < 0 || row >= items_.size()) return;
  // setText() fires textChanged, which is wired to updateSuggestions() --
  // without blocking it here, applying a selection would immediately
  // re-query suggestions using the just-selected text as the new prefix,
  // regenerating (and likely shrinking) the very list being cycled
  // through on every Tab/Up/Down press.
  const QSignalBlocker blocker(input_);
  input_->setText(items_[row].label);
  input_->setCursorPosition(input_->text().length());
}

void OmniboxOverlay::submit() {
  const int row = list_->currentRow();
  const bool hasPick = row >= 0 && row < items_.size() && !list_->isHidden();
  if (hasPick) {
    navigateTo(items_[row].url, QString());
  } else {
    navigateTo(HistoryStore::toUrl(input_->text(), config_->searchEngineUrl), input_->text());
  }
}

void OmniboxOverlay::navigateTo(const QString &url, const QString &typedQuery) {
  if (url.isEmpty()) return;
  clearSuggestions();
  startShimmer();
  emit navigateRequested(url, typedQuery);
}

namespace {
// "https://rubyonrails.org/" (a history visit, trailing slash and all) and
// "https://rubyonrails.org" (PopularDomains' bare-domain form) are the same
// site, but not the same string -- strip a bare trailing slash before
// comparing so the domain suggestion doesn't show up redundantly right
// next to the history one for the same root page.
QString dedupKey(const QString &url) {
  QString key = url;
  if (key.endsWith(QLatin1Char('/'))) key.chop(1);
  return key;
}

// Linear RGB interpolation from `from` toward `to` by `t` in [0, 1] --
// used to fade a row's label color toward the list's own background as it
// goes further down the list. Deliberately opaque colors, not alpha/
// QGraphicsOpacityEffect: the same *look* (dimmer = less prominent)
// without the per-row compositing cost of real transparency, which
// matters once the list can run to dozens of rows (maxSuggestionRows()).
QColor blend(const QColor &from, const QColor &to, double t) {
  const auto lerp = [&](int a, int b) { return a + int((b - a) * t); };
  return QColor(lerp(from.red(), to.red()), lerp(from.green(), to.green()),
                lerp(from.blue(), to.blue()));
}
}  // namespace

void OmniboxOverlay::updateSuggestions() {
  const QString text = input_->text();
  const int maxTotal = maxSuggestionRows();

  struct Scored {
    Suggestion s;
    double score;
  };
  QVector<Scored> candidates;
  QSet<QString> seenUrls;
  QSet<QString> seenLabels;
  // "https://rubyonrails.org" and "https://rubyonrails.org/" are two
  // distinct SQLite `visited` rows (url is the PRIMARY KEY) if the site
  // was ever visited both ways -- completeVisited() can legitimately
  // return both. Dedup as everything is assembled, not just between
  // history and PopularDomains, so two history rows for the same site
  // don't both survive either.
  //
  // Also dedup by label, not just url: a search-derived history label is
  // the raw typed.q text (see completeVisited()), recorded against
  // whatever URL the navigation actually landed on -- which can be a
  // *different* URL than a second, ordinary visited-page history row (or
  // a PopularDomains entry) for what's, to the eye, obviously the same
  // site. Confirmed concretely: visiting funkyimg.com (which redirects
  // elsewhere) left both a `visited` row for the bare domain and a
  // `typed` row (q="funkyimg.com") against the redirect target -- two
  // rows, different urls, identical "funkyimg.com" label. A url-only key
  // doesn't catch that; the label the user actually sees does.
  auto tryAdd = [&](const QString &label, const QString &url, SuggestionKind kind, double score) {
    // QSet::insert() (unlike std::set's) doesn't report whether the value
    // was already present, so check first.
    const QString urlKey = dedupKey(url);
    const QString labelKey = label.trimmed().toLower();
    if (seenUrls.contains(urlKey)) return;
    if (!labelKey.isEmpty() && seenLabels.contains(labelKey)) return;
    seenUrls.insert(urlKey);
    if (!labelKey.isEmpty()) seenLabels.insert(labelKey);
    candidates.push_back({{label, url, kind}, score});
  };
  // A single blended ranking, not "history first, domains fill the rest":
  // a page you've actually used should usually win, but a strong domain
  // match can still outrank a history entry you've only visited once or
  // twice. History score is visit_count itself, capped so one obsessively-
  // visited page can't permanently bury every domain suggestion; domain
  // score is scaled from its popularity rank so the single most popular
  // domain in the list scores roughly like a page visited kDomainTopScore
  // times -- competitive with a lightly-used history entry, not with a
  // frequently-used one.
  constexpr int kHistoryScoreCap = 20;
  constexpr double kDomainTopScore = 5.0;
  for (const auto &h : history_->completeVisited(text, maxTotal)) {
    tryAdd(h.label, h.url, SuggestionKind::History, qMin(h.visitCount, kHistoryScoreCap));
  }
  for (const auto &d : domains_->complete(text, maxTotal)) {
    tryAdd(d.label, d.url, SuggestionKind::Popular, kDomainTopScore / double(d.rank + 1));
  }
  std::sort(candidates.begin(), candidates.end(),
            [](const Scored &a, const Scored &b) { return a.score > b.score; });

  QVector<Suggestion> combined;
  for (int i = 0; i < candidates.size() && i < maxTotal; ++i) {
    combined.push_back(candidates[i].s);
  }
  renderSuggestions(combined);
}

void OmniboxOverlay::renderSuggestions(const QVector<Suggestion> &items) {
  items_ = items;
  list_->clear();
  for (int i = 0; i < items_.size(); ++i) {
    const Suggestion &item = items_[i];
    // Every row -- History or Popular -- gets a dismiss button, so every
    // row is a real child widget rather than plain QListWidgetItem text
    // (which can't host one).
    auto *listItem = new QListWidgetItem();
    list_->addItem(listItem);

    auto *row = new QWidget(list_);
    row->setObjectName(QStringLiteral("SuggestionRow"));
    auto *rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(8, 0, 4, 0);
    rowLayout->setSpacing(4);

    auto *label = new QLabel(item.label, row);
    label->setObjectName(QStringLiteral("SuggestionLabel"));
    // Mouse events land on whichever child widget is under the cursor,
    // not the row itself -- transparent so a click/hover anywhere on the
    // label still reaches `row`'s own event filter below, instead of
    // stopping at the label.
    label->setAttribute(Qt::WA_TransparentForMouseEvents);
    rowLayout->addWidget(label, 1);

    // Rows further down fade toward the list's own background -- "the
    // top few matter most", not a wall of equally-loud text once the list
    // fills the window (maxSuggestionRows()). Stashed on the row itself
    // so updateSuggestionSelectionStyle() (keyboard selection) and the
    // hover highlight below both know what to restore a label to.
    const double t =
        items_.size() > 1 ? double(i) / double(items_.size() - 1) : 0.0;
    constexpr double kMaxDim = 0.7;
    const QColor dimColor =
        blend(QColor(currentPalette_.fg), QColor(currentPalette_.card), t * kMaxDim);
    row->setProperty("dimColor", dimColor.name());
    label->setStyleSheet(QStringLiteral("color: %1;").arg(dimColor.name()));

    auto *dismiss = new QToolButton(row);
    dismiss->setObjectName(QStringLiteral("SuggestionDismiss"));
    dismiss->setText(QString::fromUtf8("\xC3\x97"));  // "×"
    dismiss->setAutoRaise(true);
    dismiss->setCursor(Qt::PointingHandCursor);
    dismiss->setFocusPolicy(Qt::NoFocus);
    const bool isHistory = item.kind == SuggestionKind::History;
    dismiss->setToolTip(isHistory ? QStringLiteral("Forget this from history")
                                   : QStringLiteral("Hide this suggestion"));
    rowLayout->addWidget(dismiss);

    const QString url = item.url;
    connect(dismiss, &QToolButton::clicked, this, [this, url, isHistory] {
      if (isHistory) {
        dismissHistorySuggestion(url);
      } else {
        dismissPopularSuggestion(url);
      }
    });
    // Hovering the dismiss button highlights the whole row, so it's clear
    // exactly which suggestion an accidental click would remove.
    dismiss->installEventFilter(this);

    // Clicking anywhere else on the row (not the dismiss button) navigates
    // there directly, same destination Tab-selecting it and pressing
    // Enter would reach. suggestionLabel lets the click handler update
    // the input to match, same as Tab/arrow-selecting it already does.
    row->setProperty("suggestionUrl", url);
    row->setProperty("suggestionLabel", item.label);
    row->installEventFilter(this);

    list_->setItemWidget(listItem, row);
    listItem->setSizeHint(row->sizeHint());
  }
  updateSuggestionSelectionStyle();
  layoutInput();
}

void OmniboxOverlay::dismissHistorySuggestion(const QString &url) {
  history_->forgetVisited(url);
  updateSuggestions();
}

void OmniboxOverlay::dismissPopularSuggestion(const QString &url) {
  domains_->dismiss(url);
  updateSuggestions();
}

void OmniboxOverlay::updateSuggestionSelectionStyle() {
  const int cur = list_->currentRow();
  for (int i = 0; i < items_.size(); ++i) {
    auto *row = list_->itemWidget(list_->item(i));
    if (!row) continue;
    auto *label = row->findChild<QLabel *>();
    if (!label) continue;
    if (i == cur) {
      // Matches card (dark) on the accent-colored selected background --
      // the same pairing QListWidget::item:selected uses for plain items.
      label->setStyleSheet(QStringLiteral("color: %1;").arg(currentPalette_.card));
    } else {
      // Not just plain fg -- restore this row's own depth-based dim shade
      // (see renderSuggestions()), so moving the keyboard selection away
      // from a deep row doesn't suddenly brighten it back to full color.
      label->setStyleSheet(
          QStringLiteral("color: %1;").arg(row->property("dimColor").toString()));
    }
  }
}

void OmniboxOverlay::setRowHovered(QWidget *row, bool hovered) {
  auto *label = row ? row->findChild<QLabel *>() : nullptr;
  if (!row || !label) return;
  if (hovered) {
    row->setStyleSheet(
        QStringLiteral("#SuggestionRow { background: %1; }").arg(currentPalette_.accent));
    label->setStyleSheet(QStringLiteral("color: %1;").arg(currentPalette_.card));
  } else {
    row->setStyleSheet(QString());
    // Restores this row's real state (keyboard-selected, or its own dim
    // shade) rather than guessing -- cheap enough to just recompute all
    // rows (renderSuggestions() already does this on every keystroke).
    updateSuggestionSelectionStyle();
  }
}

void OmniboxOverlay::clearSuggestions() {
  items_.clear();
  list_->clear();
  list_->hide();
}

}  // namespace shinto
