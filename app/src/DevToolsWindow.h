// Chromium's own DevTools (Elements, Console, Network, Sources, ...) for
// one page, as its own top-level window. Chrome docks DevTools inside the
// browser window because it has no window manager to lean on; Ohm
// leaves placement to Hyprland, same as its tabs. DevTools' dock-side
// buttons do nothing here -- docking is the host app's job in Qt, and
// this host deliberately doesn't do it.
#pragma once

#include <functional>

#include <QMainWindow>

#include "ThemeLoader.h"

class QPushButton;
class QWebEnginePage;
class QWebEngineView;

namespace ohm {

class DevToolsWindow : public QMainWindow {
  Q_OBJECT

 public:
  // Attaches to `inspected` for as long as this window lives. Deletes
  // itself on close, which detaches. `focusPage` brings the inspected
  // page's window to the front; the header strip calls it.
  DevToolsWindow(QWebEnginePage *inspected, std::function<void()> focusPage);
  ~DevToolsWindow() override;

  void applyPalette(const Palette &palette);

 protected:
  // Re-elides the header to its width.
  bool eventFilter(QObject *obj, QEvent *event) override;

 private:
  // Window title and header both name the page -- nothing else on screen
  // ties this window to it, since Hyprland may tile it anywhere.
  void updateLabels();

  QWebEnginePage *inspected_;
  QPushButton *header_;
  QWebEngineView *view_;
};

}  // namespace ohm
