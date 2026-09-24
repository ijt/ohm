// Chromium's own DevTools (Elements, Console, Network, Sources, ...) for
// one page, as its own top-level window. Chrome docks DevTools inside the
// browser window because it has no window manager to lean on; Shinto
// leaves placement to Hyprland, same as its tabs. DevTools' dock-side
// buttons do nothing here -- docking is the host app's job in Qt, and
// this host deliberately doesn't do it.
#pragma once

#include <QMainWindow>

class QWebEnginePage;
class QWebEngineView;

namespace shinto {

class DevToolsWindow : public QMainWindow {
  Q_OBJECT

 public:
  // Attaches to `inspected` for as long as this window lives. Deletes
  // itself on close, which detaches.
  explicit DevToolsWindow(QWebEnginePage *inspected);
  ~DevToolsWindow() override;

 private:
  void updateTitle();

  QWebEnginePage *inspected_;
  QWebEngineView *view_;
};

}  // namespace shinto
