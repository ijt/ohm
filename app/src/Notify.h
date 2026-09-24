// Best-effort desktop notifications via notify-send
// (org.freedesktop.Notifications). Ohm is normally launched from a
// Hyprland keybinding or systemd, with no terminal in sight, so this is
// often the only way a message reaches the user at all.
#pragma once

#include <functional>

#include <QString>

namespace ohm {

// If notify-send isn't installed, or nothing implements the notification
// spec, this just silently does nothing -- callers that also want a
// terminal-launched run to see the message should qWarning() it too.
void notify(const QString &title, const QString &body, bool critical = false);

// Like notify(), but the notification's own body is clickable -- runs
// `onClicked` if the user clicks it (as opposed to letting it time out or
// dismissing it), and nothing otherwise. notify-send's -A/--action
// implies --wait (the process blocks until the user interacts with it, or
// the notification server closes it), so unlike notify()'s fire-and-forget
// process this keeps a QProcess alive to actually read that outcome --
// harmless either way if nothing implements the notification spec, or if
// the notification server doesn't support actions.
void notifyClickable(const QString &title, const QString &body, std::function<void()> onClicked);

}  // namespace ohm
