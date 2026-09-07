// Best-effort Hyprland compositor helpers. Shinto treats Hyprland groups
// as its tab bar; these talk to hyprctl so a new page can join the
// current window's group. No-ops when hyprctl isn't there (not Hyprland,
// or a session without it).
#pragma once

namespace shinto {

// If the compositor's active window isn't already in a group, toggle one
// on it. Hyprland's default group.auto_group then inserts the next mapped
// window into that (unlocked) group -- which is how Ctrl+T becomes "new
// tab" rather than a sibling tile. Failures are swallowed so the caller
// can still spawn the window.
void ensureActiveWindowGrouped();

}  // namespace shinto
