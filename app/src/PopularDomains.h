// Offline omnibox autocomplete: a baked-in list of the 100k most popular
// domains (app/resources/top100k-domains.txt -- see the .SOURCE.md next to
// it), not a live network request. Nothing the user types ever leaves the
// machine to power suggestions.
#pragma once

#include <QSet>
#include <QString>
#include <QVector>

namespace ohm {

class PopularDomains {
 public:
  struct Suggestion {
    QString label;  // the bare domain, e.g. "youtube.com"
    QString url;    // "https://" + label
    // Source-list popularity rank, 0 = most popular. Lets a caller (see
    // OmniboxOverlay's blended ranking against HistoryStore) weigh how
    // strong a match this is, not just that it matched.
    int rank;
  };

  // Loads and sorts the baked-in list once, along with any previously
  // dismissed domains (ohm::dismissedDomainsPath()). Cheap enough
  // (~100k short strings) to do synchronously at daemon startup.
  PopularDomains();

  // Prefix match against the domain list (case-insensitive; a leading
  // scheme and "www." on the prefix are ignored), ranked by
  // popularity (source-list order) among matches, not alphabetically. A
  // prefix shorter than 2 characters returns nothing -- consistent with
  // the old typed/history completion's threshold, and avoids single-letter
  // prefixes matching thousands of entries for no useful purpose.
  // Dismissed domains (see dismiss()) never appear.
  QVector<Suggestion> complete(const QString &prefix, int limit) const;

  // The suggestion dropdown's per-row "x" button on a default (non-
  // history) suggestion -- permanently excludes this domain from future
  // complete() results, for every window (this one instance is shared by
  // the whole daemon), persisted to dismissedDomainsPath() so it survives
  // a daemon restart too. `url` is a Suggestion::url ("https://domain");
  // the scheme is stripped before matching against the domain list.
  void dismiss(const QString &url);

 private:
  struct Entry {
    QString domain;
    int rank;
  };
  QVector<Entry> domains_;  // sorted alphabetically by domain
  QSet<QString> dismissed_;
};

}  // namespace ohm
