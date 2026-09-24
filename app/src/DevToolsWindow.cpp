#include "DevToolsWindow.h"

#include <algorithm>

#include <QEvent>
#include <QFontMetrics>
#include <QKeySequence>
#include <QPushButton>
#include <QShortcut>
#include <QVBoxLayout>
#include <QWebEnginePage>
#include <QWebEngineView>

namespace shinto {

namespace {

// "example.com/some/path": enough to tell pages apart without the query
// string and fragment noise. The tooltip has the whole URL.
QString shortUrl(const QUrl &url) {
  if (url.host().isEmpty()) return url.toString();
  QString path = url.path();
  if (path == QLatin1String("/")) path.clear();
  return url.host() + path;
}

}  // namespace

DevToolsWindow::DevToolsWindow(QWebEnginePage *inspected, std::function<void()> focusPage)
    : inspected_(inspected) {
  setAttribute(Qt::WA_DeleteOnClose);

  auto *container = new QWidget(this);
  setCentralWidget(container);
  auto *layout = new QVBoxLayout(container);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);

  header_ = new QPushButton(container);
  header_->setObjectName(QStringLiteral("DevToolsHeader"));
  header_->setCursor(Qt::PointingHandCursor);
  header_->setFocusPolicy(Qt::NoFocus);
  header_->installEventFilter(this);
  connect(header_, &QPushButton::clicked, this, [focusPage] { focusPage(); });
  layout->addWidget(header_);

  // Same profile as the page: DevTools' own settings (panel layout, dark
  // theme, network throttling presets) persist alongside the rest of it.
  view_ = new QWebEngineView(inspected->profile(), container);
  layout->addWidget(view_);
  inspected->setDevToolsPage(view_->page());
  // DevTools' own toolbar ✕.
  connect(view_->page(), &QWebEnginePage::windowCloseRequested, this, &QWidget::close);

  updateLabels();
  connect(inspected, &QWebEnginePage::titleChanged, this, &DevToolsWindow::updateLabels);
  connect(inspected, &QWebEnginePage::urlChanged, this, &DevToolsWindow::updateLabels);
  // The inspected page going away (its window closed) ends the session.
  connect(inspected, &QObject::destroyed, this, [this] {
    inspected_ = nullptr;
    close();
  });

  // The same keys that opened DevTools close it again from in here, like
  // Chrome's; Ctrl+W as everywhere else in Shinto.
  for (const QKeySequence &seq :
       {QKeySequence(Qt::Key_F12), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_I),
        QKeySequence(Qt::CTRL | Qt::Key_W)}) {
    auto *sc = new QShortcut(seq, this);
    sc->setContext(Qt::WindowShortcut);
    connect(sc, &QShortcut::activated, this, &QWidget::close);
  }

  resize(1000, 800);
}

DevToolsWindow::~DevToolsWindow() {
  if (inspected_ && inspected_->devToolsPage() == view_->page()) {
    inspected_->setDevToolsPage(nullptr);
  }
}

void DevToolsWindow::applyPalette(const Palette &palette) {
  // Numbered from %1, one argument per placeholder -- see FindBar.
  setStyleSheet(QStringLiteral(
                    "#DevToolsHeader {"
                    " background: %1; color: %2; border: none; border-bottom: 1px solid %3;"
                    " padding: 5px 10px; text-align: left; font-family: %4; font-size: 12px; }"
                    "#DevToolsHeader:hover { color: %5; }")
                    .arg(palette.card, palette.fg, palette.muted, palette.font, palette.accent));
  ensurePolished();
  updateLabels();  // The font, and so the elision widths, may have changed.
}

bool DevToolsWindow::eventFilter(QObject *obj, QEvent *event) {
  // The header's own resize, not the window's: the window's resizeEvent
  // arrives before the layout has given the header its new width.
  if (obj == header_ && event->type() == QEvent::Resize) updateLabels();
  return QMainWindow::eventFilter(obj, event);
}

void DevToolsWindow::updateLabels() {
  if (!inspected_) return;
  const QString title = inspected_->title();
  const QString where = shortUrl(inspected_->url());
  // A page with no <title> gets its URL as title() -- don't say it twice.
  const bool untitled = title.isEmpty() || title == inspected_->url().toString() || title == where;

  const QString host = inspected_->url().host();
  if (untitled) {
    setWindowTitle(QStringLiteral("DevTools - %1").arg(where));
  } else if (host.isEmpty()) {  // data:, file:, about: -- no host to add.
    setWindowTitle(QStringLiteral("DevTools - %1").arg(title));
  } else {
    setWindowTitle(QStringLiteral("DevTools - %1 (%2)").arg(title, host));
  }

  // Title elides from the right, the URL from the middle (host and the
  // end of the path are the parts that tell pages apart).
  const QFontMetrics fm(header_->font());
  const int avail = header_->width() - 24;  // Padding, left + right.
  const QString arrow = QStringLiteral("↖ ");
  const QString sep = QStringLiteral("  ·  ");
  QString text;
  if (untitled) {
    text = arrow + fm.elidedText(where, Qt::ElideMiddle, avail - fm.horizontalAdvance(arrow));
  } else {
    const int urlWidth = std::min(fm.horizontalAdvance(where), avail / 2);
    const int titleWidth =
        avail - urlWidth - fm.horizontalAdvance(arrow) - fm.horizontalAdvance(sep);
    text = arrow + fm.elidedText(title, Qt::ElideRight, titleWidth) + sep +
           fm.elidedText(where, Qt::ElideMiddle, urlWidth);
  }
  header_->setText(text);
  header_->setToolTip(QStringLiteral("%1\n%2\nClick to show this page")
                          .arg(title, inspected_->url().toString()));
}

}  // namespace shinto
