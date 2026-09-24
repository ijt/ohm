// The native omnibox: a widget drawn on top of a BrowserWindow's
// QWebEngineView. One appearance: a caret at the top-left, no visible
// field at rest -- used both for a fresh window's empty gate and for
// Ctrl+L on an already-loaded page (which shows this same blank gate over
// the still-loaded page, rather than a prefilled address bar). A dropdown
// appears below the input as you type, filling whatever vertical room the
// window has (maxSuggestionRows()), blending two sources into one ranked
// list rather than showing one before the other (updateSuggestions()): the
// user's own visited history (HistoryStore::completeVisited()) and offline
// domain-prefix suggestions (PopularDomains). Shallower URLs sort first
// (github.com before github.com/foo/bar); among the same path depth, a page
// you've actually used usually wins, but a strong domain match can still
// outrank a history entry you've only visited once or twice. Nothing typed
// ever leaves the machine to power either. Rows fade toward the background
// color going down the list (renderSuggestions()) so a long list reads as
// "the top few matter most", not a wall of equally-loud text. Every row
// has a per-row "x" button to dismiss it -- permanently, from HistoryStore
// or PopularDomains' respective backing store, whichever it came from.
//
// This is never a navigable URL -- Escape simply hides it, since the
// underlying page (if any) was never touched.
#pragma once

#include <QVector>
#include <QWidget>

#include "Config.h"
#include "HistoryStore.h"
#include "PopularDomains.h"
#include "ThemeLoader.h"

class QLabel;
class QLineEdit;
class QListWidget;
class QResizeEvent;

namespace ohm {

class Spinner;

class OmniboxOverlay : public QWidget {
  Q_OBJECT

 public:
  OmniboxOverlay(HistoryStore *history, PopularDomains *domains, const OhmConfig *config,
                 QWidget *parent = nullptr);

  void applyPalette(const Palette &palette);

  // Shows the gate, focuses it. `prefill` empty means a blank gate (a
  // fresh window); non-empty (Ctrl+L on a loaded page) fills the input
  // with it, fully selected, so typing replaces it but Enter alone just
  // re-submits the current address. BrowserWindow always gives this
  // widget the full window area before calling this.
  void showGate(const QString &prefill = QString());

  // A window opened already aimed at a destination (CLI / xdg-open).
  // Prefills `url` without selecting it -- select-all would look like
  // Ctrl+L "edit this" -- and starts the spinner so the gate reads as
  // "this is loading", not the empty "search or url" prompt.
  void showLoading(const QString &url);

  // Hides without navigating anywhere -- what Escape does.
  void hideOverlay();

  // A thin accent-colored line at the top, `percent` of the window wide.
  // Chromium reports 100% before it has a frame to paint, so the bar is
  // hidden at 100% and a small spinner next to the URL keeps moving until
  // hideOverlay() (see BrowserWindow::onOverlayNavigate).
  void setProgress(int percent);

 signals:
  // The user submitted a destination. `typedQuery` is the raw text typed
  // when that's what actually produced `url` (via HistoryStore::toUrl,
  // using config_->searchEngineUrl) -- empty when an existing suggestion
  // was picked instead, since then `url` is already a real destination,
  // not something resolved from free-typed text. BrowserWindow uses
  // `typedQuery` to record it against whatever URL the navigation actually
  // lands on (see its loadFinished handler) rather than here: search
  // engines routinely rewrite/redirect the URL Ohm originally
  // requested, so pairing it with the pre-redirect `url` would silently
  // never match anything back up later (HistoryStore::completeVisited()'s
  // join).
  void navigateRequested(const QString &url, const QString &typedQuery);
  // The user backed out (Escape) without navigating.
  void cancelled();

 protected:
  bool eventFilter(QObject *obj, QEvent *event) override;
  void resizeEvent(QResizeEvent *event) override;

 private:
  enum class SuggestionKind { History, Popular };
  struct Suggestion {
    QString label;
    QString url;
    SuggestionKind kind;
  };

  void submit();
  // Shared tail of a navigation: clears suggestions, starts the spinner,
  // and emits navigateRequested. `typedQuery` is forwarded as-is (see the
  // signal's doc comment) -- empty for anything already a real
  // destination (a clicked/picked suggestion), non-empty only for
  // free-typed text resolved via HistoryStore::toUrl.
  void navigateTo(const QString &url, const QString &typedQuery);
  void layoutInput();
  void layoutProgressBar();
  void updateSuggestions();
  void renderSuggestions(const QVector<Suggestion> &items);
  void clearSuggestions();
  // How many suggestion rows roughly fit below the input in the window's
  // current height -- "fill available space" rather than a fixed count.
  int maxSuggestionRows() const;
  void moveSelection(int delta);
  void applySelectionToInput();
  int suggestionListHeight() const;
  // Marks this overlay as waiting on a navigation and starts the spinner.
  // The URL text stays still -- the bar and spinner are the loading cue.
  void beginLoad();
  void startSpinner();
  void stopSpinner();
  // The suggestion dropdown's per-row "x" button: forgets it (permanently,
  // from HistoryStore or PopularDomains, whichever it came from) and
  // refreshes the list.
  void dismissHistorySuggestion(const QString &url);
  void dismissPopularSuggestion(const QString &url);
  // A row's label is a real QLabel, not item text -- QListWidget's own
  // `::item:selected` QSS color override never reaches it, so this
  // mirrors that override by hand on whichever row is currentRow().
  void updateSuggestionSelectionStyle();
  // Highlights (or un-highlights) one row -- hovering its dismiss button
  // makes clear exactly which suggestion an accidental click would remove.
  // Independent of keyboard selection (list_->currentRow()): hovering the
  // "x" on a row you didn't arrow-key to shouldn't change what pressing
  // Enter would submit.
  void setRowHovered(QWidget *row, bool hovered);

  HistoryStore *history_;
  PopularDomains *domains_;
  const OhmConfig *config_;
  Palette currentPalette_;
  QLineEdit *input_;
  QListWidget *list_;
  QWidget *progressBar_;
  Spinner *spinner_;
  QLabel *hint_;
  // Empty-gate only -- hidden for Ctrl+L / loading so it isn't chrome on
  // an already-aimed window.
  bool hintEnabled_ = false;
  // True between beginLoad() (overlay submit / showLoading) and the gate
  // going idle (showGate / hideOverlay). setProgress() no-ops unless this
  // is set, so about:blank on the empty gate can't leave the spinner
  // running forever.
  bool awaitingLoad_ = false;
  int progress_ = 0;
  QVector<Suggestion> items_;
};

}  // namespace ohm
