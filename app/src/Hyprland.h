// Best-effort Hyprland compositor helpers. Ohm treats Hyprland groups
// as its tab bar; these talk to hyprctl so a new page can join the
// current window's group. No-ops when hyprctl isn't there (not Hyprland,
// or a session without it).
#pragma once

#include <functional>

#include <QtGlobal>

class QObject;
class QString;

namespace ohm {

// If the compositor's active window isn't already in a group, toggle one
// on it. Hyprland's default group.auto_group then inserts the next mapped
// window into that (unlocked) group -- which is how Ctrl+T becomes "new
// tab" rather than a sibling tile. Failures are swallowed so the caller
// can still spawn the window.
void ensureActiveWindowGrouped();

// Asks Hyprland, asynchronously, which window is active and calls `done`
// with its address ("0x...") and client pid -- or an empty address if
// hyprctl isn't there or fails. `done` is dropped if `context` dies first.
void queryActiveWindow(QObject *context,
                       std::function<void(const QString &address, qint64 pid)> done);

// Focuses the window at `address` (switching its group to it, if it's a
// tab). Returns false if hyprctl isn't there or Hyprland knows no such
// window, e.g. it has since closed.
bool focusWindow(const QString &address);

}  // namespace ohm
