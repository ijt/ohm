#include "PopularDomains.h"

#include <algorithm>

#include <QDebug>
#include <QFile>
#include <QRegularExpression>
#include <QTextStream>

#include "Shinto.h"

namespace shinto {

namespace {

// Mirrors HistoryStore's scheme-stripping regex -- Suggestion::url is
// always "https://" + domain, but dismiss() also accepts whatever a
// PopularDomains::Suggestion::url actually looked like, defensively.
QString stripScheme(const QString &url) {
  static const QRegularExpression kScheme(QStringLiteral("^[a-zA-Z][a-zA-Z0-9+.-]*://"));
  QString domain = url;
  domain.remove(kScheme);
  return domain.toLower();
}

}  // namespace

PopularDomains::PopularDomains() {
  QFile file(QStringLiteral(":/shinto/top100k-domains.txt"));
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
    qWarning() << "shinto: could not open bundled domain list";
    return;
  }
  QTextStream stream(&file);
  int rank = 0;
  while (!stream.atEnd()) {
    const QString line = stream.readLine().trimmed().toLower();
    if (!line.isEmpty()) {
      domains_.push_back({line, rank});
    }
    ++rank;
  }
  std::sort(domains_.begin(), domains_.end(),
            [](const Entry &a, const Entry &b) { return a.domain < b.domain; });

  QFile dismissedFile(dismissedDomainsPath());
  if (dismissedFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
    QTextStream dismissedStream(&dismissedFile);
    while (!dismissedStream.atEnd()) {
      const QString domain = dismissedStream.readLine().trimmed().toLower();
      if (!domain.isEmpty()) dismissed_.insert(domain);
    }
  }
  // A missing file is the common case (nothing dismissed yet) -- nothing
  // to warn about.
}

QVector<PopularDomains::Suggestion> PopularDomains::complete(const QString &prefix,
                                                              int limit) const {
  QVector<Suggestion> out;
  // Same scheme/www stripping as HistoryStore::completeVisited -- typing
  // "https://git" should complete like "git", not look for a domain that
  // literally starts with "https://".
  static const QRegularExpression kScheme(
      QStringLiteral("^[a-zA-Z][a-zA-Z0-9+.-]*://"));
  static const QRegularExpression kWww(
      QStringLiteral("^www\\."), QRegularExpression::CaseInsensitiveOption);
  QString p = prefix.trimmed().toLower();
  p.remove(kScheme);
  p.remove(kWww);
  if (p.size() < 2 || domains_.isEmpty()) return out;

  // [lo, hi) is the contiguous range of domains starting with `p` --
  // `upper` is `p` with its last character bumped by one code point, the
  // standard trick for a prefix range query over a sorted sequence.
  QString upper = p;
  upper[upper.size() - 1] = QChar(upper.at(upper.size() - 1).unicode() + 1);

  const auto lo = std::lower_bound(
      domains_.begin(), domains_.end(), p,
      [](const Entry &e, const QString &v) { return e.domain < v; });
  const auto hi = std::lower_bound(
      domains_.begin(), domains_.end(), upper,
      [](const Entry &e, const QString &v) { return e.domain < v; });

  QVector<Entry> matches(lo, hi);
  std::sort(matches.begin(), matches.end(),
            [](const Entry &a, const Entry &b) { return a.rank < b.rank; });

  for (const Entry &e : matches) {
    if (out.size() >= limit) break;
    if (dismissed_.contains(e.domain)) continue;
    out.push_back({e.domain, QStringLiteral("https://") + e.domain, e.rank});
  }
  return out;
}

void PopularDomains::dismiss(const QString &url) {
  const QString domain = stripScheme(url);
  if (domain.isEmpty() || dismissed_.contains(domain)) return;
  dismissed_.insert(domain);

  QFile file(dismissedDomainsPath());
  if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
    qWarning() << "shinto: could not persist dismissed domain" << domain << "--"
               << file.errorString();
    return;  // still excluded for the rest of this daemon's lifetime, just not saved
  }
  QTextStream(&file) << domain << '\n';
}

}  // namespace shinto
