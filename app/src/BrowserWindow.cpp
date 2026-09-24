#include "BrowserWindow.h"

#include <QApplication>
#include <QCloseEvent>
#include <QDataStream>
#include <QFocusEvent>
#include <QKeyEvent>
#include <QPrintDialog>
#include <QPrinter>
#include <QResizeEvent>
#include <QShortcut>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QWebEngineFindTextResult>
#include <QWebEngineFrame>
#include <QWebEngineFullScreenRequest>
#include <QWebEngineHistory>
#include <QWebEngineNewWindowRequest>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineView>
#include <QWebEngineWebAuthUxRequest>

#include "DownloadBar.h"
#include "DownloadManager.h"
#include "DownloadsPanelLauncher.h"
#include "FindBar.h"
#include "Hyprland.h"
#include "OmniboxOverlay.h"
#include "Shinto.h"

namespace shinto {

namespace {

bool isShintoShortcut(const QKeyEvent *ke, const QKeySequence &backShortcut) {
  // Alt+Left (Chromium's own back shortcut, and this app's default -- see
  // Config.h) is exactly the kind of thing a focused page/input can eat
  // via ShortcutOverride, same as Ctrl+T/N/L/K/W below -- e.g. some sites'
  // own JS treats it as "navigate within an editable field". Configurable,
  // so it has to be checked by value, not by a fixed key/modifier pair.
  if (!backShortcut.isEmpty() && QKeySequence(ke->keyCombination()) == backShortcut) {
    return true;
  }
  // Ignore KeypadModifier so Ctrl+numpad +/- still match; Shift is only
  // accepted for zoom-in (Ctrl+Shift+= is Key_Plus on most layouts), the
  // shortcuts overlay (Ctrl+? is Ctrl+Shift+/ on a US layout), and
  // reopening a closed page (Ctrl+Shift+T).
  const Qt::KeyboardModifiers mods =
      ke->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier | Qt::AltModifier | Qt::MetaModifier);
  if (mods == Qt::NoModifier && ke->key() == Qt::Key_F1) return true;
  if (mods == (Qt::ControlModifier | Qt::ShiftModifier)) {
    switch (ke->key()) {
      case Qt::Key_T:
      case Qt::Key_Equal:
      case Qt::Key_Plus:
      case Qt::Key_Slash:
      case Qt::Key_Question:
        return true;
      default:
        return false;
    }
  }
  if (mods != Qt::ControlModifier) return false;
  switch (ke->key()) {
    case Qt::Key_T:
    case Qt::Key_N:
    case Qt::Key_L:
    case Qt::Key_K:
    case Qt::Key_W:
    case Qt::Key_F:
    case Qt::Key_P:
    case Qt::Key_R:
    case Qt::Key_Equal:
    case Qt::Key_Minus:
    case Qt::Key_Plus:
    case Qt::Key_Slash:
    case Qt::Key_Question:
      return true;
    default:
      return false;
  }
}

// Chromium's own page-zoom presets. Stepping through these, rather than
// adding a fixed 0.1, matches what Ctrl+=/- does in Chrome -- including
// the same min/max.
constexpr qreal kZoomFactors[] = {0.25, 0.333, 0.5,  0.666, 0.75, 0.8, 0.9, 1.0,
                                  1.1,  1.25,  1.5,  1.75,  2.0,  2.5, 3.0, 4.0,
                                  5.0};
constexpr int kZoomFactorCount = static_cast<int>(sizeof(kZoomFactors) / sizeof(kZoomFactors[0]));
constexpr qreal kZoomEpsilon = 0.001;

void stepZoom(QWebEngineView *view, int direction) {
  const qreal current = view->zoomFactor();
  if (direction > 0) {
    for (qreal z : kZoomFactors) {
      if (z > current + kZoomEpsilon) {
        view->setZoomFactor(z);
        return;
      }
    }
    view->setZoomFactor(kZoomFactors[kZoomFactorCount - 1]);
  } else {
    for (int i = kZoomFactorCount - 1; i >= 0; --i) {
      if (kZoomFactors[i] < current - kZoomEpsilon) {
        view->setZoomFactor(kZoomFactors[i]);
        return;
      }
    }
    view->setZoomFactor(kZoomFactors[0]);
  }
}

// QtWebEngine's handleFocusEvent calls Chromium SetInitialFocus (first
// focusable node -- often a top-left logo <img> inside an <a>) whenever
// a FocusIn arrives with TabFocusReason / BacktabFocusReason. Qt uses
// those reasons for things that are not the user pressing Tab: first
// widget in a newly-shown window (activateWindow -> focusNextPrevChild),
// and hiding a focused sibling (the omnibox gate). Track a real Tab so
// those synthetic cases can be rewritten to OtherFocusReason; a genuine
// Tab still highlights, which is the accessibility path we want to keep.
bool tabKeyIsDown = false;

void givePageFocus(QWebEngineView *view) {
  view->setFocus(Qt::OtherFocusReason);
}

// QWebEnginePage's default javaScriptConsoleMessage() prints every page's
// own console.log/warn/error output to stderr -- fine for web development,
// but Shinto is an app-mode browser, not a devtools console, and real
// sites (YouTube included) constantly emit their own warnings that have
// nothing to do with Shinto. Swallow it.
class WebPage : public QWebEnginePage {
 public:
  using QWebEnginePage::QWebEnginePage;

 protected:
  void javaScriptConsoleMessage(JavaScriptConsoleMessageLevel /*level*/,
                                 const QString & /*message*/, int /*lineNumber*/,
                                 const QString & /*sourceID*/) override {}
};

}  // namespace

// A QWebEngineView by default can "eat" key events that also match one of
// our QShortcuts (Chromium's own input handling marks ShortcutOverride
// events accepted for many keys, which tells Qt not to fire the shortcut).
// Intercepting ShortcutOverride here, before it reaches the base class,
// guarantees Ctrl+T/N/L/K/W/=/- always reach BrowserWindow's shortcuts even
// when a page has focus -- there is no JS-level race to lose, unlike the
// old content-script approach.
class WebView : public QWebEngineView {
 public:
  explicit WebView(QWebEngineProfile *profile, const QKeySequence &backShortcut,
                    QWidget *parent = nullptr)
      : QWebEngineView(parent), backShortcut_(backShortcut) {
    setPage(new WebPage(profile, this));
    ensureFocusRewriteFilter();
  }

 protected:
  bool event(QEvent *e) override {
    if (e->type() == QEvent::ShortcutOverride) {
      auto *ke = static_cast<QKeyEvent *>(e);
      if (isShintoShortcut(ke, backShortcut_)) {
        e->ignore();
        return true;
      }
    }
    return QWebEngineView::event(e);
  }

 private:
  // FocusIn lands on the internal focus proxy, not this widget, so the
  // rewrite has to be an app-wide filter that walks to a WebView parent.
  static void ensureFocusRewriteFilter() {
    struct Filter : QObject {
      bool eventFilter(QObject *obj, QEvent *e) override {
        if (e->type() == QEvent::KeyPress || e->type() == QEvent::KeyRelease) {
          const auto *k = static_cast<QKeyEvent *>(e);
          if (k->key() == Qt::Key_Tab || k->key() == Qt::Key_Backtab)
            tabKeyIsDown = e->type() == QEvent::KeyPress;
          return false;
        }
        if (e->type() != QEvent::FocusIn || tabKeyIsDown) return false;
        auto *fe = static_cast<QFocusEvent *>(e);
        if (fe->reason() != Qt::TabFocusReason && fe->reason() != Qt::BacktabFocusReason) {
          return false;
        }
        for (QWidget *w = qobject_cast<QWidget *>(obj); w; w = w->parentWidget()) {
          // WebView has no Q_OBJECT, so qobject_cast<WebView*> is always
          // null; every QWebEngineView in this process is one of ours.
          if (qobject_cast<QWebEngineView *>(w)) {
            QFocusEvent rewritten(QEvent::FocusIn, Qt::OtherFocusReason);
            QCoreApplication::sendEvent(obj, &rewritten);
            return true;
          }
        }
        return false;
      }
    };
    static Filter filter;
    static bool installed = false;
    if (!installed) {
      qApp->installEventFilter(&filter);
      installed = true;
    }
  }

  QKeySequence backShortcut_;
};

QVector<BrowserWindow *> BrowserWindow::instances_;
QVector<BrowserWindow::ClosedPage> BrowserWindow::closedPages_;
Palette BrowserWindow::currentPalette_;

BrowserWindow *BrowserWindow::spawn(QWebEngineProfile *profile, HistoryStore *history,
                                     PopularDomains *domains, DownloadManager *downloads,
                                     const QString &url) {
  // A shell command that reached the daemon ("OPEN uninstall") must not
  // become a window. QUrl("uninstall") is relative, WebEngine ignores it,
  // and the window sits on about:blank -- a blank white page.
  if (isShellCommand(url)) {
    qWarning().noquote() << "shinto: ignoring command passed as a page:" << url;
    return nullptr;
  }
  QString resolved = url;
  if (!resolved.isEmpty()) {
    const QUrl parsed(resolved);
    if (!parsed.isValid() || parsed.scheme().isEmpty()) {
      resolved = HistoryStore::toUrl(resolved, loadConfig().searchEngineUrl);
    }
  }
  return spawnInternal(profile, history, domains, downloads, resolved, /*showEmptyGate=*/true,
                       /*mapWindow=*/true);
}

BrowserWindow *BrowserWindow::spawnInternal(QWebEngineProfile *profile, HistoryStore *history,
                                             PopularDomains *domains, DownloadManager *downloads,
                                             const QString &url, bool showEmptyGate, bool mapWindow) {
  auto *win = new BrowserWindow(profile, history, domains, downloads, url, showEmptyGate);
  win->setAttribute(Qt::WA_DeleteOnClose);
  instances_.push_back(win);
  win->resize(1200, 800);
  if (mapWindow) win->show();
  return win;
}

BrowserWindow *BrowserWindow::spawnForRequest(QWebEngineProfile *profile, HistoryStore *history,
                                               PopularDomains *domains,
                                               DownloadManager *downloads,
                                               QWebEngineNewWindowRequest &request) {
  // mapWindow=false: openIn() below has to happen before the first map.
  // Mapping first painted a full-window empty gate ("search or url") over
  // about:blank -- the overlay is a child QWidget, visible by default,
  // and showEmptyGate=false only skipped showGate(), it never hid it.
  // Reported concretely: clicking a target=_blank link showed a blank
  // location bar until the real page finished loading.
  BrowserWindow *win = spawnInternal(profile, history, domains, downloads, QString(),
                                      /*showEmptyGate=*/false, /*mapWindow=*/false);
  const QUrl dest = request.requestedUrl();
  if (dest.isValid() && !dest.isEmpty() && dest != QUrl(QStringLiteral("about:blank"))) {
    // Same loading gate as a CLI `shinto <url>`: destination visible,
    // with the spinner, until the page paints. Empty requestedUrl (some OAuth
    // popups) stays overlay-hidden -- openIn() still has a real
    // WebContents, just no URL string we could honestly show.
    win->overlay_->showLoading(dest.toString());
    win->relayout();
  }
  // Connect before openIn() so a synchronous loadFinished can't slip past.
  win->revealOnFirstLoad();
  // openIn() must be called before this handler returns, or Qt rejects
  // the window-open request outright -- see spawnForRequest()'s own doc
  // comment for why this, not request.requestedUrl() + setUrl(), is what
  // actually fixes OAuth popups.
  request.openIn(win->webView_->page());
  win->show();
  return win;
}

void BrowserWindow::applyPaletteToAll(const Palette &palette) {
  currentPalette_ = palette;
  for (auto *w : instances_) {
    w->overlay_->applyPalette(palette);
    w->findBar_->applyPalette(palette);
  }
}

BrowserWindow::BrowserWindow(QWebEngineProfile *profile, HistoryStore *history,
                              PopularDomains *domains, DownloadManager *downloads,
                              const QString &url, bool showEmptyGate)
    : history_(history), domains_(domains), downloads_(downloads), config_(loadConfig()) {
  setWindowTitle(QStringLiteral("Shinto"));

  auto *container = new QWidget(this);
  setCentralWidget(container);

  webView_ = new WebView(profile, config_.backShortcut, container);

  overlay_ = new OmniboxOverlay(history_, domains_, &config_, container);
  overlay_->applyPalette(currentPalette_);

  findBar_ = new FindBar(container);
  findBar_->applyPalette(currentPalette_);

  downloadBar_ = new DownloadBar(container);
  downloadBar_->applyPalette(currentPalette_);
  connect(downloadBar_, &DownloadBar::clicked, this, &BrowserWindow::showDownloadsPanel);
  connect(downloads_, &DownloadManager::downloadAdded, this, [this](int) { refreshDownloadBar(); });
  connect(downloads_, &DownloadManager::downloadProgress, this,
          [this](int, qint64, qint64) { refreshDownloadBar(); });
  connect(downloads_, &DownloadManager::downloadStateChanged, this,
          [this](int, DownloadManager::State) { refreshDownloadBar(); });

  auto *layout = new QVBoxLayout(container);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->addWidget(webView_);
  // overlay_/findBar_/downloadBar_ are deliberately not added to this
  // layout -- BrowserWindow positions them directly on top of webView_ by
  // hand (relayout()/relayoutFindBar()/relayoutDownloadBar()).

  connect(overlay_, &OmniboxOverlay::navigateRequested, this, &BrowserWindow::onOverlayNavigate);
  connect(overlay_, &OmniboxOverlay::cancelled, this, &BrowserWindow::onOverlayCancelled);

  connect(findBar_, &FindBar::searchChanged, this,
          [this](const QString &text) { doFind(text, /*backward=*/false); });
  connect(findBar_, &FindBar::findNext, this,
          [this] { doFind(findBar_->searchText(), /*backward=*/false); });
  connect(findBar_, &FindBar::findPrevious, this,
          [this] { doFind(findBar_->searchText(), /*backward=*/true); });
  connect(findBar_, &FindBar::closed, this, [this] {
    webView_->page()->findText(QString());
    givePageFocus(webView_);
  });

  // A guarded record on urlChanged/titleChanged (rather than only in
  // loadFinished below) still matters for single-page apps that change
  // the URL/title via JS after a real load already succeeded (pushState,
  // a tab-title unread-count badge, etc.) -- loadOk_ stays true across
  // those, so they're recorded too, just never for a load that hasn't
  // (yet, or ever) actually succeeded.
  connect(webView_->page(), &QWebEnginePage::urlChanged, this, [this](const QUrl &navUrl) {
    if (loadOk_) history_->recordVisit(navUrl.toString(), webView_->page()->title());
  });
  connect(webView_->page(), &QWebEnginePage::titleChanged, this, [this](const QString &title) {
    // Hyprland group tabs (and the window decoration) read this title.
    // about:blank's own document title is the string "about:blank"; keep
    // the empty gate labeled Shinto instead of that.
    const bool internal = webView_->url().scheme() == QLatin1String("about");
    setWindowTitle(title.isEmpty() || internal ? QStringLiteral("Shinto") : title);
    if (loadOk_) history_->recordVisit(webView_->url().toString(), title);
  });
  // The actual "was this visit real" gate: loadStarted resets it so a
  // pending navigation is never mistaken for its predecessor's success,
  // and loadFinished(true) is also where a search's typed query is first
  // recorded -- paired with the URL it actually landed on, not the one
  // Shinto originally requested (see onOverlayNavigate()), since search
  // engines routinely rewrite/redirect that URL before it commits.
  connect(webView_->page(), &QWebEnginePage::loadStarted, this, [this] { loadOk_ = false; });
  connect(webView_->page(), &QWebEnginePage::loadFinished, this, [this](bool ok) {
    loadOk_ = ok;
    if (!ok) {
      pendingTypedQuery_.clear();
      return;
    }
    const QString finalUrl = webView_->url().toString();
    history_->recordVisit(finalUrl, webView_->page()->title());
    if (!pendingTypedQuery_.isEmpty()) {
      history_->recordTyped(pendingTypedQuery_, finalUrl);
      pendingTypedQuery_.clear();
    }
  });
  // Overlay ignores these unless it's awaiting a navigation (see
  // OmniboxOverlay::setProgress) -- the empty gate's about:blank would
  // otherwise leave the spinner running on the search/url screen.
  connect(webView_->page(), &QWebEnginePage::loadProgress, overlay_, &OmniboxOverlay::setProgress);
  // The QtWebEngine equivalent of Chromium's "exploded" multi-tab windows:
  // target=_blank / window.open() / ctrl-click all route through this one
  // signal -- fulfilled as a brand new BrowserWindow, so "one window, one
  // page" still holds structurally. Delivered via spawnForRequest()
  // (request.openIn(), not a manually re-navigated fresh page): OAuth
  // popup flows (Sign in with Apple/Google, etc.) rely on window.opener /
  // postMessage back to this page to report success, which only survives
  // if the popup's actual page (the WebContents Chromium already created
  // for window.open()) is what ends up on screen -- an unrelated page
  // that merely loads the same URL string has no such relationship, and
  // is exactly what used to leave those flows hung on a blank/unusable
  // page (confirmed: "Continue with Apple" on x.com).
  connect(webView_->page(), &QWebEnginePage::newWindowRequested, this,
          [this](QWebEngineNewWindowRequest &request) {
            // Chromium's print-preview UI lives at chrome://print. QtWebEngine
            // doesn't implement that page, so fulfilling the request as a
            // BrowserWindow is a blank white window -- the reported PDF-viewer
            // "Print" button failure. Drop it; printRequested (below) is the
            // path Qt actually wants the embedder to handle.
            const QString scheme = request.requestedUrl().scheme();
            if (scheme == QLatin1String("chrome") || scheme == QLatin1String("chrome-untrusted")) {
              return;
            }
            BrowserWindow::spawnForRequest(webView_->page()->profile(), history_, domains_,
                                            downloads_, request);
          });
  // window.print() and the PDF viewer plugin's print button both emit this
  // instead of showing Chromium's own print dialog. Unhandled, Chromium
  // still tries to open its print-preview WebContents -- which, with the
  // newWindowRequested handler above, used to become a blank window.
  // Deferred: this signal fires from inside Chromium's print-preview
  // setup, and QPrintDialog::exec() is a nested event loop -- running it
  // synchronously here used to race the preview WebContents into a blank
  // window of our own. Let that setup finish first.
  connect(webView_->page(), &QWebEnginePage::printRequested, this,
          [this] { QTimer::singleShot(0, this, &BrowserWindow::onPrintRequested); });
  connect(webView_->page(), &QWebEnginePage::printRequestedByFrame, this,
          [this](QWebEngineFrame) {
            QTimer::singleShot(0, this, &BrowserWindow::onPrintRequested);
          });
  // Fullscreen API: enablement lives on the shared profile; accepting the
  // request here is what actually lets the element fill the viewport, and
  // we mirror that with a real window fullscreen so YouTube/etc. leave the
  // tiled Hyprland layout.
  connect(webView_->page(), &QWebEnginePage::fullScreenRequested, this,
          [this](QWebEngineFullScreenRequest request) {
            if (request.toggleOn()) {
              request.accept();
              showFullScreen();
            } else {
              request.accept();
              showNormal();
            }
          });
  // Backstop for WebProfile.cpp's installWebAuthnShim(). A publicKey
  // ceremony that still reaches Qt (shim missed, or a frame it didn't
  // patch) raises this and, unanswered, never settles the page's
  // credentials.get() -- x.com's "Sign in with passkey" spinner. There is
  // no transport/QR dialog to show (Qt only surfaces PIN, account pick,
  // and touch-the-key), so cancel instead of leaving it pending.
  connect(webView_->page(), &QWebEnginePage::webAuthUxRequested, this,
          [](QWebEngineWebAuthUxRequest *request) {
            QTimer::singleShot(0, request, &QWebEngineWebAuthUxRequest::cancel);
          });

  auto addShortcut = [this](const QKeySequence &seq, auto slot) {
    auto *sc = new QShortcut(seq, this);
    sc->setContext(Qt::WindowShortcut);
    connect(sc, &QShortcut::activated, this, slot);
  };
  addShortcut(QKeySequence(Qt::CTRL | Qt::Key_T), &BrowserWindow::onNewTabShortcut);
  addShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_T),
              &BrowserWindow::onReopenClosedShortcut);
  addShortcut(QKeySequence(Qt::CTRL | Qt::Key_N), &BrowserWindow::onNewPageShortcut);
  addShortcut(QKeySequence(Qt::CTRL | Qt::Key_L), &BrowserWindow::onEditAddressShortcut);
  addShortcut(QKeySequence(Qt::CTRL | Qt::Key_K), &BrowserWindow::onEditAddressShortcut);
  addShortcut(QKeySequence(Qt::CTRL | Qt::Key_W), [this] { close(); });
  addShortcut(config_.backShortcut, &BrowserWindow::onBackShortcut);
  addShortcut(QKeySequence(Qt::CTRL | Qt::Key_F), &BrowserWindow::onFindShortcut);
  addShortcut(QKeySequence(Qt::CTRL | Qt::Key_R), &BrowserWindow::onReloadShortcut);
  addShortcut(QKeySequence(Qt::CTRL | Qt::Key_P), &BrowserWindow::onPrintRequested);
  // Ctrl+= is the unshifted plus key on US-layout; Ctrl++ is the same key
  // with Shift (and the numpad plus). Both zoom in, matching Chrome.
  addShortcut(QKeySequence(Qt::CTRL | Qt::Key_Equal), &BrowserWindow::onZoomInShortcut);
  addShortcut(QKeySequence(Qt::CTRL | Qt::Key_Plus), &BrowserWindow::onZoomInShortcut);
  addShortcut(QKeySequence(Qt::CTRL | Qt::Key_Minus), &BrowserWindow::onZoomOutShortcut);
  // Ctrl+? is Ctrl+Shift+/ on a US layout; bind the unshifted slash too.
  addShortcut(QKeySequence(Qt::CTRL | Qt::Key_Question), &BrowserWindow::showShortcutsPanel);
  addShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Slash), &BrowserWindow::showShortcutsPanel);
  addShortcut(QKeySequence(Qt::CTRL | Qt::Key_Slash), &BrowserWindow::showShortcutsPanel);
  addShortcut(QKeySequence(Qt::Key_F1), &BrowserWindow::showShortcutsPanel);

  if (url.isEmpty()) {
    if (showEmptyGate) {
      enterEmpty(true);
    } else {
      // Popup path (spawnForRequest): openIn() is about to hand this page
      // a real WebContents. Don't load about:blank -- its loadFinished
      // would fire the SingleShot that hides the loading overlay, or
      // (if cancelled by openIn without a finished signal) leave the
      // empty gate up until the real page paints. Don't show the gate
      // either; overlay starts hidden (OmniboxOverlay ctor).
      overlay_->hideOverlay();
    }
  } else {
    // Keep the gate up with the destination visible until the page paints.
    // Hiding it here flashes a blank webview -- or worse, the empty
    // "search or url" prompt -- while the real URL is already loading.
    state_ = State::Empty;
    relayout();
    overlay_->showLoading(url);
    onOverlayNavigate(url, QString());
  }

  // Reflects a download already in progress (started before this window
  // existed) immediately, the same way overlay_/findBar_ above already
  // apply currentPalette_ without waiting for the next broadcast.
  refreshDownloadBar();
}

BrowserWindow::~BrowserWindow() {
  delete printer_;
  instances_.removeOne(this);
}

void BrowserWindow::closeEvent(QCloseEvent *event) {
  // Ctrl+W, Super+Q and the window's own close all land here. The empty
  // gate's about:blank has nothing worth reopening.
  constexpr int kMaxClosedPages = 25;
  const QUrl url = webView_->url();
  if (url.isValid() && !url.isEmpty() && url.scheme() != QLatin1String("about")) {
    QByteArray state;
    QDataStream out(&state, QIODevice::WriteOnly);
    out << *webView_->history();
    closedPages_.push_back({url, state});
    if (closedPages_.size() > kMaxClosedPages) closedPages_.removeFirst();
  }
  QMainWindow::closeEvent(event);
}

void BrowserWindow::resizeEvent(QResizeEvent *event) {
  QMainWindow::resizeEvent(event);
  relayout();
  relayoutFindBar();
  relayoutDownloadBar();
}

void BrowserWindow::relayout() {
  // Both Empty and Gate show the same full-window gate; Loaded shows none
  // (webView_ already fills the container via its own layout).
  if (state_ != State::Loaded) {
    overlay_->setGeometry(centralWidget()->rect());
  }
}

void BrowserWindow::relayoutFindBar() {
  if (!findBar_->isVisible()) return;
  constexpr int kMargin = 12;
  const QSize hint = findBar_->sizeHint();
  const int w = qMin(hint.width(), qMax(0, centralWidget()->width() - 2 * kMargin));
  findBar_->setGeometry(centralWidget()->width() - w - kMargin, kMargin, w, hint.height());
}

void BrowserWindow::relayoutDownloadBar() {
  if (!downloadBar_->isVisible()) return;
  // Flush with the very bottom edge, full width -- unlike findBar_ (a
  // small floating box), this bar's whole point is a strip you can't miss.
  downloadBar_->setGeometry(0, centralWidget()->height() - downloadBar_->height(),
                             centralWidget()->width(), downloadBar_->height());
}

void BrowserWindow::refreshDownloadBar() {
  if (downloads_->hasActive()) {
    downloadBar_->showFor(downloads_->latestActive());
    relayoutDownloadBar();
  } else {
    downloadBar_->hideBar();
  }
}

void BrowserWindow::showDownloadsPanel() { shinto::showDownloadsPanel(); }

void BrowserWindow::showShortcutsPanel() { shinto::showShortcutsPanel(); }

void BrowserWindow::enterEmpty(bool showGate) {
  state_ = State::Empty;
  setWindowTitle(QStringLiteral("Shinto"));
  relayout();
  // A QWebEngineView that's never been navigated at all can make Qt
  // recreate the window's native surface once, shortly after this window
  // is first mapped -- visible as a startup close+reopen flicker (root-
  // caused via isolated testing: reproduced with a bare idle
  // QWebEngineView, gone once it was given something, anything, to load).
  // about:blank gives the compositor a real frame to commit while still
  // looking empty -- needed regardless of showGate, since the gate (when
  // shown at all) fully covers it either way.
  webView_->setUrl(QUrl(QStringLiteral("about:blank")));
  if (showGate) overlay_->showGate();
  else overlay_->hideOverlay();
}

void BrowserWindow::showGateOverPage() {
  state_ = State::Gate;
  relayout();
  overlay_->showGate(webView_->url().toString());
}

void BrowserWindow::onOverlayNavigate(const QString &url, const QString &typedQuery) {
  // Keep the gate up until the new page actually has something to paint --
  // hiding it right away would flash the previous page's last frame while
  // the new one loads, since navigation is asynchronous. state_ stays Gate
  // (or Empty) in the meantime, so a resize mid-load still repositions the
  // still-visible gate correctly. Feedback while waiting: the spinner,
  // plus the top progress bar once Chromium reports a percent (the bar
  // hits 100% before Chromium has a frame, so the spinner covers that gap).
  overlay_->setProgress(0);
  // Picked up by the persistent loadFinished handler above once (and only
  // if) this navigation actually succeeds -- see its comment for why this
  // isn't just recorded right here.
  pendingTypedQuery_ = typedQuery;
  revealOnFirstLoad();
  webView_->setUrl(QUrl(url));
}

void BrowserWindow::onOverlayCancelled() {
  // A no-op on the empty gate: there is nothing loaded to go back to.
  if (state_ != State::Gate) return;
  state_ = State::Loaded;
  givePageFocus(webView_);
  overlay_->hideOverlay();
}

void BrowserWindow::onNewPageShortcut() {
  BrowserWindow::spawn(webView_->page()->profile(), history_, domains_, downloads_, QString());
}

void BrowserWindow::onNewTabShortcut() {
  // Group the current window first so the new one auto-joins it (Hyprland
  // group.auto_group). If it's already grouped this is a no-op; if hyprctl
  // isn't there, spawn still happens and the page just opens as a tile.
  ensureActiveWindowGrouped();
  BrowserWindow::spawn(webView_->page()->profile(), history_, domains_, downloads_, QString());
}

void BrowserWindow::onReopenClosedShortcut() {
  if (closedPages_.isEmpty()) return;
  const ClosedPage page = closedPages_.takeLast();
  ensureActiveWindowGrouped();
  // Same unmapped start as a popup (see spawnForRequest): no empty gate and
  // no about:blank load, just the loading gate until the page paints.
  BrowserWindow *win = spawnInternal(webView_->page()->profile(), history_, domains_, downloads_,
                                      QString(), /*showEmptyGate=*/false, /*mapWindow=*/false);
  win->overlay_->showLoading(page.url.toString());
  win->relayout();
  win->revealOnFirstLoad();
  // Restoring the history also navigates to its current entry. Fall back
  // to the bare URL if the saved history doesn't load.
  QDataStream in(page.history);
  in >> *win->webView_->history();
  if (in.status() != QDataStream::Ok || win->webView_->history()->count() == 0) {
    win->webView_->setUrl(page.url);
  }
  win->show();
}

void BrowserWindow::revealOnFirstLoad() {
  connect(
      webView_->page(), &QWebEnginePage::loadFinished, this,
      [this](bool) {
        state_ = State::Loaded;
        givePageFocus(webView_);
        overlay_->hideOverlay();
      },
      Qt::SingleShotConnection);
}

void BrowserWindow::onEditAddressShortcut() {
  if (state_ == State::Empty) return;  // documented no-op on the empty gate
  showGateOverPage();
}

void BrowserWindow::onBackShortcut() {
  // Chrome itself only treats this as browser-back when the page (not the
  // address bar) has focus -- while the gate is up, Alt+Left there is
  // meaningless (state_ isn't Loaded), so skip it entirely rather than
  // navigating a page the user isn't even looking at right now.
  if (state_ != State::Loaded) return;

  QWebEngineHistory *hist = webView_->history();
  // Every window's history starts with the internal about:blank
  // enterEmpty() loads before any real navigation -- once that's the
  // *only* thing left behind the current page, canGoBack() is still true,
  // but actually going back would land on a blank page, not a previous
  // one. A window opened directly with a URL (a CLI `shinto <url>`, an
  // OAuth popup) has no about:blank at all, so canGoBack() is simply
  // false there. Either way, there's no real page to go back to, so just
  // do nothing -- popping open the location gate on a plain Alt+Left felt
  // surprising in practice.
  const bool atFirstRealPage = hist->currentItemIndex() == 1 &&
                                hist->itemAt(0).url() == QUrl(QStringLiteral("about:blank"));
  if (hist->canGoBack() && !atFirstRealPage) {
    webView_->back();
  }
}

void BrowserWindow::onFindShortcut() {
  // Meaningless while the gate is up (Empty/Gate) -- there's no page
  // underneath to search yet, or its content is hidden anyway.
  if (state_ != State::Loaded) return;
  findBar_->showBar();
  relayoutFindBar();
}

void BrowserWindow::onReloadShortcut() {
  // Same reasoning as onFindShortcut(): nothing real to reload while the
  // gate is up (Empty/Gate).
  if (state_ != State::Loaded) return;
  webView_->reload();
}

void BrowserWindow::onZoomInShortcut() { stepZoom(webView_, 1); }

void BrowserWindow::onZoomOutShortcut() { stepZoom(webView_, -1); }

void BrowserWindow::onPrintRequested() {
  // Meaningless while the gate is up -- same as find/reload. Also a no-op
  // if a job is already in flight: the PDF viewer allows the print button
  // to be mashed, and Ctrl+P can race the signal from window.print().
  if (state_ != State::Loaded || printer_) return;

  printer_ = new QPrinter(QPrinter::HighResolution);
  printer_->setDocName(windowTitle());

  QPrintDialog dialog(printer_, this);
  dialog.setWindowTitle(QStringLiteral("Print"));
  // PrintToFile is the Unix "save as PDF" path in this dialog; page range
  // and collate are what a real print dialog is expected to offer.
  dialog.setOptions(QAbstractPrintDialog::PrintToFile | QAbstractPrintDialog::PrintShowPageSize |
                    QAbstractPrintDialog::PrintPageRange | QAbstractPrintDialog::PrintCollateCopies);

  if (dialog.exec() != QDialog::Accepted) {
    delete printer_;
    printer_ = nullptr;
    return;
  }

  // QWebEngineView::print() to a PdfFormat QPrinter is unreliable;
  // printToPdf is the supported "print to file" path. printer_ still has
  // to live until pdfPrintingFinished -- same async constraint as print().
  if (printer_->outputFormat() == QPrinter::PdfFormat && !printer_->outputFileName().isEmpty()) {
    connect(
        webView_, &QWebEngineView::pdfPrintingFinished, this,
        [this](const QString &, bool) {
          delete printer_;
          printer_ = nullptr;
        },
        Qt::SingleShotConnection);
    webView_->printToPdf(printer_->outputFileName(), printer_->pageLayout());
    return;
  }

  connect(
      webView_, &QWebEngineView::printFinished, this,
      [this](bool) {
        delete printer_;
        printer_ = nullptr;
      },
      Qt::SingleShotConnection);
  webView_->print(printer_);
}

void BrowserWindow::doFind(const QString &text, bool backward) {
  if (text.isEmpty()) {
    webView_->page()->findText(QString());
    findBar_->setMatchCount(0, 0);
    return;
  }
  QWebEnginePage::FindFlags flags;
  if (backward) flags |= QWebEnginePage::FindBackward;
  webView_->page()->findText(text, flags, [this](const QWebEngineFindTextResult &result) {
    findBar_->setMatchCount(result.activeMatch(), result.numberOfMatches());
  });
}

}  // namespace shinto
