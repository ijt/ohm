// Typed-URL + visited-history log, the omnibox's URL-or-search heuristic,
// and the omnibox's history-based autocomplete (completeVisited()).
#pragma once

#include <QObject>
#include <QSqlDatabase>
#include <QString>
#include <QVector>

#include "UrlMatch.h"

namespace ohm {

class HistoryStore : public QObject {
  Q_OBJECT

 public:
  struct Suggestion {
    QString label;  // https:// (and a leading www.) stripped; other schemes kept
    QString url;
    // How many times this URL has been visited. Lets a caller (see
    // OmniboxOverlay's blended ranking against PopularDomains) weigh how
    // strong a match this is, not just that it matched.
    int visitCount;
    // How loosely the typed text matched (see UrlMatch.h). The omnibox
    // ranks by this before anything else.
    UrlMatchTier matchTier;
  };

  explicit HistoryStore(QObject *parent = nullptr);

  // Opens (creating if needed) the SQLite store at ohm::historyDbPath().
  // Returns false (and logs) if the SQLite driver / file can't be opened.
  bool open();

  // Records an omnibox submission. `url` is what was navigated to; `q` is
  // the raw text the user typed.
  void recordTyped(const QString &q, const QString &url);

  // Records a page visit, called from BrowserWindow on urlChanged/
  // titleChanged. Internal/gate-ish URLs are filtered out.
  void recordVisit(const QString &url, const QString &title);

  // Visited URLs (each url is a SQLite PRIMARY KEY, so already unique)
  // that `prefix` matches, by host prefix or more loosely (see UrlMatch.h),
  // stricter matches first, then shallower URLs, then visit_count -- the
  // omnibox's "you've been here before" suggestions, as opposed to PopularDomains'
  // baked-in popularity list. A visit that came from an omnibox search
  // shows as the query text itself ("weather today"), not the search
  // engine's own URL ("duckduckgo.com/?q=weather+today") -- recovered via
  // a join against `typed` (see recordTyped()). A typed URL uses the same
  // https-stripped label as any other visit, so "https://github.com" and
  // "github.com" don't appear as two different-looking rows. A leading
  // scheme on `prefix` is ignored ("https://git" completes like "git").
  // A prefix shorter than 2 characters returns nothing, matching
  // PopularDomains::complete()'s threshold.
  QVector<Suggestion> completeVisited(const QString &prefix, int limit) const;

  // Removes one URL from visited history -- the omnibox suggestion
  // dropdown's per-row "x" button on a history suggestion. Permanent: the
  // point is the user asked to stop being reminded of it, not a session-
  // only hide.
  void forgetVisited(const QString &url);

  // Heuristic URL-or-search resolution: has a scheme -> as-is; "//" -> https:;
  // "localhost[:port]/..." or an IPv4 host -> http://; a bare "word.word"
  // with no spaces -> https://; otherwise -> `searchEngineUrl` with "%s"
  // replaced by the percent-encoded query (see Config.h; defaults to
  // DuckDuckGo). Mirrors toUrl()/completeToUrl() from the old
  // extension/background.js verbatim, aside from the configurable engine.
  static QString toUrl(const QString &raw, const QString &searchEngineUrl);

 private:
  static bool isInternalUrl(const QString &url);
  void trimTyped();

  QSqlDatabase db_;
  static constexpr int kTypedMax = 300;
};

}  // namespace ohm
