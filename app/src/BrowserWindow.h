// One window = one page (matching Shinto's long-standing philosophy).
// Owns a QWebEngineView and an OmniboxOverlay, and is its own state
// machine: Empty (fresh window, blank gate shown, nothing loaded yet) <->
// Loaded (gate hidden, page visible) <-> Gate (Ctrl+L on a loaded page
// shows the same gate on top of it, prefilled with the current URL and
// fully selected -- Escape reverts to Loaded; the empty window's Empty
// state has nothing to revert to, so Ctrl+L there is a no-op). There is
// no separate "gate window" type -- see the plan's "Window/overlay model"
// section.
#pragma once

#include <QMainWindow>
#include <QVector>

#include "Config.h"
#include "HistoryStore.h"
#include "PopularDomains.h"
#include "ThemeLoader.h"

class QPrinter;
class QResizeEvent;
class QWebEngineNewWindowRequest;
class QWebEngineProfile;

namespace shinto {

class DownloadBar;
class DownloadManager;
class FindBar;
class OmniboxOverlay;
class WebView;

class BrowserWindow : public QMainWindow {
  Q_OBJECT

 public:
  // Creates, registers, shows, and returns a new window. `url` empty means
  // start in the empty-gate state; non-empty shows that URL on the gate
  // and loads it, hiding the gate once the page paints. Reads config.lua
  // fresh (see Config.h) for this one window -- the daemon can stay warm
  // for days, so config shouldn't be stuck at whatever it read at daemon
  // startup; loadConfig() is cheap next to everything else spawn already
  // does.
  static BrowserWindow *spawn(QWebEngineProfile *profile, HistoryStore *history,
                               PopularDomains *domains, DownloadManager *downloads,
                               const QString &url);

  // Fulfills a window.open()-driven popup request (target=_blank, JS
  // window.open(), ctrl-click -- all route through
  // QWebEnginePage::newWindowRequested) as a new BrowserWindow, same as
  // spawn(), but via request.openIn() rather than a manually re-navigated
  // fresh page. That preserves window.opener/postMessage back to the
  // requesting page, which OAuth popup flows (Sign in with Apple/Google,
  // etc.) need to report success -- an unrelated page that merely loads
  // the same URL string has no such relationship and leaves those flows
  // hung on a blank/unusable page instead.
  static BrowserWindow *spawnForRequest(QWebEngineProfile *profile, HistoryStore *history,
                                         PopularDomains *domains, DownloadManager *downloads,
                                         QWebEngineNewWindowRequest &request);

  // Re-applies a reloaded theme to every live window (the `shinto theme` /
  // Omarchy theme-set-hook path, delivered over the singleton socket).
  static void applyPaletteToAll(const Palette &palette);

  // The last-broadcast palette, cached so a freshly-spawned window (or
  // DownloadsWindow, opened lazily and separately from this class) can
  // apply it immediately without waiting for the next broadcast.
  static const Palette &currentPalette() { return currentPalette_; }

 ~BrowserWindow() override;

 protected:
  void resizeEvent(QResizeEvent *event) override;

 private:
  enum class State { Empty, Loaded, Gate };

  // Shared by spawn() and spawnForRequest(). `mapWindow` is false for
  // popups so spawnForRequest() can openIn() before the first map -- an
  // idle QWebEngineView flickers on first show, and mapping before openIn
  // would paint the empty gate (or about:blank) for a frame.
  static BrowserWindow *spawnInternal(QWebEngineProfile *profile, HistoryStore *history,
                                       PopularDomains *domains, DownloadManager *downloads,
                                       const QString &url, bool showEmptyGate, bool mapWindow);

  BrowserWindow(QWebEngineProfile *profile, HistoryStore *history, PopularDomains *domains,
                DownloadManager *downloads, const QString &url, bool showEmptyGate);

  void relayout();
  void relayoutFindBar();
  void relayoutDownloadBar();
  // Shows/hides downloadBar_ to match downloads_->hasActive() (and its
  // content to downloads_->latestActive()) -- connected to all three
  // DownloadManager signals, so it doesn't matter which one fired.
  void refreshDownloadBar();
  // Toggles the "Shinto Downloads" Quickshell panel (see downloads-panel/)
  // -- it reads DownloadManager's own downloads.sqlite directly, no IPC
  // with this process needed.
  void showDownloadsPanel();
  void enterEmpty(bool showGate);
  void showGateOverPage();
  void onOverlayNavigate(const QString &url, const QString &typedQuery);
  void onOverlayCancelled();
  void onNewPageShortcut();
  // Ctrl+T: same spawn as onNewPageShortcut, but first groups the current
  // window so Hyprland auto-joins the new one (groups are Shinto's tabs).
  void onNewTabShortcut();
  void onEditAddressShortcut();
  void onBackShortcut();
  void onFindShortcut();
  void onReloadShortcut();
  void onZoomInShortcut();
  void onZoomOutShortcut();
  // PDF-viewer print button and window.print() both arrive as
  // QWebEnginePage::printRequested -- QtWebEngine does not show a print
  // dialog on its own. Ctrl+P uses the same path.
  void onPrintRequested();
  // `backward` selects QWebEnginePage::FindBackward -- the ↑/previous
  // direction. An empty `text` just clears any existing highlighting
  // (searchChanged's "cleared the box" case) rather than searching.
  void doFind(const QString &text, bool backward);

  HistoryStore *history_;
  PopularDomains *domains_;
  DownloadManager *downloads_;
  // Owned by this window, not shared -- loaded fresh in the constructor
  // (see spawn()'s doc comment), so each window can have read a different
  // config.lua than its siblings if the file changed between opens.
  ShintoConfig config_;
  WebView *webView_;
  OmniboxOverlay *overlay_;
  FindBar *findBar_;
  DownloadBar *downloadBar_;
  State state_ = State::Empty;
  // Recording (both `visited` and `typed`) is deferred to loadFinished(true)
  // -- see the constructor -- rather than done eagerly on request, so a
  // failed navigation (DNS error, connection refused) never gets recorded,
  // and a search's typed query gets paired with the URL it actually landed
  // on (search engines routinely rewrite/redirect, so that can differ from
  // the URL Shinto itself requested).
  bool loadOk_ = false;
  QString pendingTypedQuery_;
  // Heap-allocated because QWebEngineView::print() is async -- a stack
  // QPrinter would be destroyed before Chromium finished painting. Lives
  // from dialog-accept until printFinished (or this window's destructor).
  QPrinter *printer_ = nullptr;

  static QVector<BrowserWindow *> instances_;
  static Palette currentPalette_;
};

}  // namespace shinto
