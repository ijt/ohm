#include "ReadlineEditing.h"

#include <algorithm>
#include <cstdlib>

#include <QKeyEvent>
#include <QLineEdit>

namespace ohm {

namespace {

// One kill buffer shared by every input this module touches (currently the
// omnibox and the find bar) -- readline itself has exactly one kill ring
// per process too, so sharing it across widgets matches the model users
// already know, rather than surprising them with a per-widget one. Not the
// system clipboard on purpose: Ctrl+K/Ctrl+W etc. shouldn't clobber
// whatever the user just copied from a page.
//
// Real readline accumulates consecutive kills into one entry (kill,
// kill, kill, yank restores all three concatenated); this only ever
// keeps the most recent kill. That's a deliberate simplification -- the
// accumulate-on-consecutive-kill behavior needs tracking "was the
// previous command also a kill", which isn't worth the complexity for a
// single-line address/search box.
QString g_killBuffer;

// Ctrl-only (no Shift/Alt/Meta) and Alt-only, ignoring the numpad bit some
// platforms set on ordinary keys.
constexpr Qt::KeyboardModifiers kIgnoredBits = Qt::KeypadModifier | Qt::GroupSwitchModifier;

bool isCtrlOnly(const QKeyEvent *key) {
  return (key->modifiers() & ~kIgnoredBits) == Qt::ControlModifier;
}
bool isAltOnly(const QKeyEvent *key) {
  return (key->modifiers() & ~kIgnoredBits) == Qt::AltModifier;
}

void killSelectionOrRange(QLineEdit *edit, int start, int length) {
  edit->setSelection(start, length);
  g_killBuffer = edit->selectedText();
  edit->del();  // deletes the current selection
}

// True readline word-boundary scanning, not QLineEdit's built-in
// cursorWordForward()/cursorWordBackward() -- those two aren't symmetric
// (confirmed concretely: cursorWordForward() lands on the *start* of the
// next word, consuming trailing whitespace along the way, matching the
// Ctrl+Right convention most GUI editors use; cursorWordBackward() lands
// right at the *start of the current/previous word*, not consuming
// leading whitespace). Readline's own forward-word/backward-word are
// symmetric: each stops right at a word's edge, treating any run of
// non-word characters between words as a boundary to skip over but never
// to consume into the kill. "Word" = readline's own default: letters and
// digits.
int forwardWordBoundary(const QString &text, int pos) {
  const int n = text.length();
  while (pos < n && !text.at(pos).isLetterOrNumber()) ++pos;
  while (pos < n && text.at(pos).isLetterOrNumber()) ++pos;
  return pos;
}
int backwardWordBoundary(const QString &text, int pos) {
  while (pos > 0 && !text.at(pos - 1).isLetterOrNumber()) --pos;
  while (pos > 0 && text.at(pos - 1).isLetterOrNumber()) --pos;
  return pos;
}

void moveByWord(QLineEdit *edit, bool forward) {
  const int target = forward ? forwardWordBoundary(edit->text(), edit->cursorPosition())
                              : backwardWordBoundary(edit->text(), edit->cursorPosition());
  edit->setCursorPosition(target);  // also clears any existing selection
}

// Kills from the cursor to the next word boundary in the given direction
// -- readline's kill-word/Alt+D (forward) and unix-word-rubout/Ctrl+W,
// backward-kill-word/Alt+Backspace (backward).
void killByWord(QLineEdit *edit, bool forward) {
  const int cur = edit->cursorPosition();
  const int target = forward ? forwardWordBoundary(edit->text(), cur)
                              : backwardWordBoundary(edit->text(), cur);
  killSelectionOrRange(edit, std::min(cur, target), std::abs(target - cur));
}

}  // namespace

bool isReadlineEditKey(const QKeyEvent *key) {
  if (isCtrlOnly(key)) {
    switch (key->key()) {
      case Qt::Key_A:
      case Qt::Key_E:
      case Qt::Key_B:
      case Qt::Key_F:
      case Qt::Key_D:
      case Qt::Key_H:
      case Qt::Key_K:
      case Qt::Key_U:
      case Qt::Key_W:
      case Qt::Key_Y:
        return true;
      default:
        return false;
    }
  }
  if (isAltOnly(key)) {
    switch (key->key()) {
      case Qt::Key_B:
      case Qt::Key_F:
      case Qt::Key_D:
      case Qt::Key_Backspace:
        return true;
      default:
        return false;
    }
  }
  return false;
}

void applyReadlineEdit(QLineEdit *edit, const QKeyEvent *key) {
  if (isCtrlOnly(key)) {
    switch (key->key()) {
      case Qt::Key_A:  // beginning-of-line
        edit->home(false);
        return;
      case Qt::Key_E:  // end-of-line
        edit->end(false);
        return;
      case Qt::Key_B:  // backward-char
        edit->cursorBackward(false);
        return;
      case Qt::Key_F:  // forward-char
        edit->cursorForward(false);
        return;
      case Qt::Key_D:  // delete-char (forward)
        edit->del();
        return;
      case Qt::Key_H:  // backward-delete-char
        edit->backspace();
        return;
      case Qt::Key_K:  // kill-line (cursor to end)
        killSelectionOrRange(edit, edit->cursorPosition(),
                              edit->text().length() - edit->cursorPosition());
        return;
      case Qt::Key_U:  // unix-line-discard (start to cursor)
        killSelectionOrRange(edit, 0, edit->cursorPosition());
        return;
      case Qt::Key_W:  // unix-word-rubout
        killByWord(edit, /*forward=*/false);
        return;
      case Qt::Key_Y:  // yank
        edit->insert(g_killBuffer);
        return;
      default:
        return;
    }
  }
  if (isAltOnly(key)) {
    switch (key->key()) {
      case Qt::Key_B:  // backward-word
        moveByWord(edit, /*forward=*/false);
        return;
      case Qt::Key_F:  // forward-word
        moveByWord(edit, /*forward=*/true);
        return;
      case Qt::Key_D:  // kill-word (forward)
        killByWord(edit, /*forward=*/true);
        return;
      case Qt::Key_Backspace:  // backward-kill-word
        killByWord(edit, /*forward=*/false);
        return;
      default:
        return;
    }
  }
}

}  // namespace ohm
