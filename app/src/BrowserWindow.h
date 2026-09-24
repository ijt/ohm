// One window = one page (matching Ohm's long-standing philosophy).
// Owns a QWebEngineView and an OmniboxOverlay, and is its own state
// machine: Empty (fresh window, blank gate shown, nothing loaded yet) <->
// Loaded (gate hidden, page visible) <-> Gate (Ctrl+L on a loaded page
// shows the same gate on top of it, prefilled with the current URL and
// fully selected -- Escape reverts to Loaded; the empty window's Empty
// state has nothing to revert to, so Ctrl+L there is a no-op). There is
// no separate "gate window" type -- see the plan's "Window/overlay model"
// section.
#pragma once

#include <QByteArray>
#include <QMainWindow>
#include <QPointer>
#include <QUrl>
#include <QVector>

#include "Config.h"
#include "HistoryStore.h"
#include "PopularDomains.h"
#include "ThemeLoader.h"

class QPrinter;
class QResizeEvent;
class QWebEngineNewWindowRequest;
class QWebEngineProfile;

namespace ohm {

class DevToolsWindow;
class DownloadBar;
class DownloadManager;
class FindBar;
class OmniboxOverlay;
class PasskeyOverlay;
class PermissionBar;
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

  // Re-applies a reloaded theme to every live window (the `ohm theme` /
  // Omarchy theme-set-hook path, delivered over the singleton socket).
  static void applyPaletteToAll(const Palette &palette);

  // Runs a named command ("reopen", "zoom-in", ...; see the table in
  // BrowserWindow.cpp) in the most recently focused window. The command
  // palette sends these from the Quickshell shortcuts panel, which holds
  // keyboard focus itself, so "the active window" at that moment is none of
  // ours. Unknown names and no-window cases are ignored.
  static void runCommand(const QString &name);

  // Brings forward a window showing a page from `origin` (a clicked web
  // notification), the most recently focused one if there are several.
  static void focusWindowShowing(const QUrl &origin);

  // The last-broadcast palette, cached so a freshly-spawned window (or
  // DownloadsWindow, opened lazily and separately from this class) can
  // apply it immediately without waiting for the next broadcast.
  static const Palette &currentPalette() { return currentPalette_; }

 ~BrowserWindow() override;

 protected:
  void resizeEvent(QResizeEvent *event) override;
  // Tracks the most recently focused window for runCommand().
  void changeEvent(QEvent *event) override;
  // Remembers the page (URL and back/forward history) for Ctrl+Shift+T.
  void closeEvent(QCloseEvent *event) override;

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
  void relayoutPermissionBar();
  // Shows/hides downloadBar_ to match downloads_->hasActive() (and its
  // content to downloads_->latestActive()) -- connected to all three
  // DownloadManager signals, so it doesn't matter which one fired.
  void refreshDownloadBar();
  // Toggles the "Ohm Downloads" Quickshell panel (see downloads-panel/)
  // -- it reads DownloadManager's own downloads.sqlite directly, no IPC
  // with this process needed.
  void showDownloadsPanel();
  // Toggles the shortcuts cheatsheet (see shortcuts-panel/). Ctrl+? / F1.
  void showShortcutsPanel();
  void enterEmpty(bool showGate);
  void showGateOverPage();
  void onOverlayNavigate(const QString &url, const QString &typedQuery);
  void onOverlayCancelled();
  void onNewPageShortcut();
  // Ctrl+T: same spawn as onNewPageShortcut, but first groups the current
  // window so Hyprland auto-joins the new one (groups are Ohm's tabs).
  void onNewTabShortcut();
  // Ctrl+Shift+T: reopens the most recently closed page, with its
  // back/forward history, as a new tab in this window's group (like
  // Ctrl+T). Closed pages are kept per daemon process, so this works from
  // any Ohm window, including one opened after the last one closed.
  void onReopenClosedShortcut();
  // Once the next load finishes, hides the loading gate and focuses the
  // page. For windows that start loading without going through the gate.
  void revealOnFirstLoad();
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
  // F12 / Ctrl+Shift+I: opens this page's DevTools window, or closes it
  // if it's already open.
  void toggleDevTools();
  // The context menu's "Inspect": opens DevTools (if needed) and selects
  // the element under the menu in the Elements panel.
  void inspectElement();
  // Opens (not toggles) this page's DevTools window.
  void openDevTools();
  // Brings this window to the front via Hyprland -- the DevTools header's
  // click. A client can't raise itself on Wayland; Hyprland can.
  void focusViaCompositor();

  HistoryStore *history_;
  PopularDomains *domains_;
  DownloadManager *downloads_;
  // Owned by this window, not shared -- loaded fresh in the constructor
  // (see spawn()'s doc comment), so each window can have read a different
  // config.lua than its siblings if the file changed between opens.
  OhmConfig config_;
  WebView *webView_;
  OmniboxOverlay *overlay_;
  FindBar *findBar_;
  DownloadBar *downloadBar_;
  // Phone-passkey QR prompt; PasskeyBroker drives it.
  PasskeyOverlay *passkeyOverlay_;
  // "site wants to use your microphone" prompts, across the top.
  PermissionBar *permissionBar_;
  State state_ = State::Empty;
  // Recording (both `visited` and `typed`) is deferred to loadFinished(true)
  // -- see the constructor -- rather than done eagerly on request, so a
  // failed navigation (DNS error, connection refused) never gets recorded,
  // and a search's typed query gets paired with the URL it actually landed
  // on (search engines routinely rewrite/redirect, so that can differ from
  // the URL Ohm itself requested).
  bool loadOk_ = false;
  QString pendingTypedQuery_;
  // Heap-allocated because QWebEngineView::print() is async -- a stack
  // QPrinter would be destroyed before Chromium finished painting. Lives
  // from dialog-accept until printFinished (or this window's destructor).
  QPrinter *printer_ = nullptr;
  // Null until F12/Inspect; closes itself when this window's page goes.
  QPointer<DevToolsWindow> devTools_;
  // This window's Hyprland address ("0x..."), learned the first time it's
  // active (see changeEvent()); empty until then, or off Hyprland.
  QString hyprAddress_;
  bool hyprAddressPending_ = false;

  static QVector<BrowserWindow *> instances_;
  static BrowserWindow *lastActive_;
  // Most recently closed last. Each entry is the page's URL and its
  // QWebEngineHistory serialized with QDataStream.
  struct ClosedPage {
    QUrl url;
    QByteArray history;
  };
  static QVector<ClosedPage> closedPages_;
  static Palette currentPalette_;
};

}  // namespace ohm
