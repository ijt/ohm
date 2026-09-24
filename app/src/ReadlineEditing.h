// GNU-readline-style single-line editing (Ctrl+A/E/B/F/D/H/K/U/W/Y/T,
// Alt+B/F/D) shared by every plain QLineEdit that wants it -- currently
// OmniboxOverlay's address/search input and FindBar's search input.
//
// Wired into an existing eventFilter(), not a QLineEdit subclass: both
// call sites already route their input_'s events through a QObject
// eventFilter (for arrow-key/Escape/Enter handling), so adding one more
// check there is less churn than swapping the widget type everywhere it's
// constructed.
#pragma once

class QLineEdit;
class QKeyEvent;

namespace ohm {

// True if `key` is one of the combos this module knows how to handle at
// all, regardless of whether the caller is going to apply the edit right
// now. Callers use this from two places:
//  - QEvent::ShortcutOverride, to `accept()` the event and pre-empt a
//    conflicting window-level QShortcut (this app binds Ctrl+K/Ctrl+T/
//    Ctrl+W/Ctrl+F to other things -- see ReadlineEditing.cpp for why
//    stealing them back while one of these inputs has focus is safe).
//  - QEvent::KeyPress, to decide whether to call applyReadlineEdit()
//    instead of falling through to the input's own default handling.
bool isReadlineEditKey(const QKeyEvent *key);

// Applies the edit for `key` to `edit`. Caller must have already checked
// isReadlineEditKey(key); behavior for any other key is undefined.
void applyReadlineEdit(QLineEdit *edit, const QKeyEvent *key);

}  // namespace ohm
