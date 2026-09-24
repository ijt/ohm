#include "DevToolsWindow.h"

#include <QKeySequence>
#include <QShortcut>
#include <QWebEnginePage>
#include <QWebEngineView>

namespace shinto {

DevToolsWindow::DevToolsWindow(QWebEnginePage *inspected) : inspected_(inspected) {
  setAttribute(Qt::WA_DeleteOnClose);
  // Same profile as the page: DevTools' own settings (panel layout, dark
  // theme, network throttling presets) persist alongside the rest of it.
  view_ = new QWebEngineView(inspected->profile(), this);
  setCentralWidget(view_);
  inspected->setDevToolsPage(view_->page());
  // DevTools' own toolbar ✕.
  connect(view_->page(), &QWebEnginePage::windowCloseRequested, this, &QWidget::close);

  updateTitle();
  connect(inspected, &QWebEnginePage::titleChanged, this, &DevToolsWindow::updateTitle);
  connect(inspected, &QWebEnginePage::urlChanged, this, &DevToolsWindow::updateTitle);
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

void DevToolsWindow::updateTitle() {
  if (!inspected_) return;
  const QString title = inspected_->title();
  setWindowTitle(QStringLiteral("DevTools - %1")
                     .arg(title.isEmpty() ? inspected_->url().toString() : title));
}

}  // namespace shinto
