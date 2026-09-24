#include "HistoryStore.h"

#include <algorithm>

#include <QDateTime>
#include <QDebug>
#include <QRegularExpression>
#include <QSqlError>
#include <QSqlQuery>
#include <QUrl>

#include "Shinto.h"
#include "UrlMatch.h"

namespace shinto {

namespace {

// Hide the default scheme so suggestion rows don't flicker between
// "https://github.com" (typed with a scheme, or Ctrl+L then Enter) and
// "github.com" (PopularDomains, or a visit with no typed query). https is
// implied; other schemes stay visible so http:// / file:// don't look like
// ordinary sites. A leading "www." is stripped either way, matching
// PopularDomains' bare-domain labels. A path is kept, e.g.
// "https://www.github.com/ijt/shinto" -> "github.com/ijt/shinto".
QString displayLabel(const QString &url) {
  static const QRegularExpression kHttps(
      QStringLiteral("^https://"), QRegularExpression::CaseInsensitiveOption);
  static const QRegularExpression kWww(
      QStringLiteral("^((?:[a-zA-Z][a-zA-Z0-9+.-]*://)?)www\\."),
      QRegularExpression::CaseInsensitiveOption);
  QString label = url;
  label.remove(kHttps);
  label.replace(kWww, QStringLiteral("\\1"));
  // A bare root path adds nothing over the domain alone, and otherwise
  // duplicates -- to the eye, if not by exact string -- PopularDomains'
  // own bare-domain suggestion for the same site (e.g. history's
  // "rubyonrails.org/" next to the domain list's "rubyonrails.org"). Only
  // the root: a real path keeps its trailing slash if it had one. Measured
  // on the host+path so a kept scheme's slashes ("http://example.com/")
  // don't block the chop.
  const int schemeSep = label.indexOf(QStringLiteral("://"));
  const QString hostAndPath =
      schemeSep >= 0 ? label.mid(schemeSep + 3) : label;
  if (hostAndPath.endsWith(QLatin1Char('/')) &&
      hostAndPath.count(QLatin1Char('/')) == 1) {
    label.chop(1);
  }
  return label;
}

// Inverse of the URL cases in HistoryStore::toUrl -- a typed query that
// was itself a URL should display as displayLabel(landed url), not the raw
// typed string (which may still have "https://"). Search queries
// ("weather today") stay as typed.
bool looksLikeUrl(const QString &q) {
  static const QRegularExpression kScheme(QStringLiteral("^[a-zA-Z][a-zA-Z0-9+.-]*:"));
  static const QRegularExpression kLocalhost(
      QStringLiteral("^localhost(:\\d+)?(/|$)"), QRegularExpression::CaseInsensitiveOption);
  static const QRegularExpression kIpv4(
      QStringLiteral("^(\\d{1,3}\\.){3}\\d{1,3}(:\\d+)?(/|$)"));
  static const QRegularExpression kBareDomain(QStringLiteral("^\\S+\\.\\S+$"));
  if (kScheme.match(q).hasMatch()) return true;
  if (q.startsWith(QStringLiteral("//"))) return true;
  if (kLocalhost.match(q).hasMatch()) return true;
  if (kIpv4.match(q).hasMatch()) return true;
  if (kBareDomain.match(q).hasMatch() && !q.contains(QLatin1Char(' '))) return true;
  return false;
}

// Typing "https://git" should complete like "git" -- the scheme is default
// noise, not part of what's matched. Also applied to each visited URL, so
// both sides are compared without scheme or leading "www.".
QString completionPrefix(const QString &prefix) {
  static const QRegularExpression kScheme(
      QStringLiteral("^[a-zA-Z][a-zA-Z0-9+.-]*://"));
  static const QRegularExpression kWww(
      QStringLiteral("^www\\."), QRegularExpression::CaseInsensitiveOption);
  QString p = prefix.trimmed();
  p.remove(kScheme);
  p.remove(kWww);
  return p;
}

}  // namespace

HistoryStore::HistoryStore(QObject *parent) : QObject(parent) {}

bool HistoryStore::open() {
  db_ = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("shinto_history"));
  db_.setDatabaseName(historyDbPath());
  if (!db_.open()) {
    qWarning() << "shinto: could not open history db:" << db_.lastError().text();
    return false;
  }
  QSqlQuery q(db_);
  q.exec(QStringLiteral(
      "CREATE TABLE IF NOT EXISTS typed ("
      " url TEXT PRIMARY KEY, q TEXT, t INTEGER, n INTEGER)"));
  q.exec(QStringLiteral(
      "CREATE TABLE IF NOT EXISTS visited ("
      " url TEXT PRIMARY KEY, title TEXT, visit_count INTEGER, last_visit INTEGER)"));
  return true;
}

void HistoryStore::recordTyped(const QString &q, const QString &url) {
  if (url.isEmpty()) return;
  QSqlQuery query(db_);
  query.prepare(QStringLiteral(
      "INSERT INTO typed(url, q, t, n) VALUES(:url, :q, :t, 1)"
      " ON CONFLICT(url) DO UPDATE SET"
      "  q = excluded.q, t = excluded.t, n = typed.n + 1"));
  query.bindValue(":url", url);
  query.bindValue(":q", q.trimmed());
  query.bindValue(":t", QDateTime::currentMSecsSinceEpoch());
  if (!query.exec()) {
    qWarning() << "shinto: recordTyped failed:" << query.lastError().text();
  }
  trimTyped();
}

void HistoryStore::trimTyped() {
  QSqlQuery query(db_);
  query.prepare(QStringLiteral(
      "DELETE FROM typed WHERE url NOT IN"
      " (SELECT url FROM typed ORDER BY t DESC LIMIT :max)"));
  query.bindValue(":max", kTypedMax);
  query.exec();
}

void HistoryStore::recordVisit(const QString &url, const QString &title) {
  if (isInternalUrl(url)) return;
  QSqlQuery query(db_);
  query.prepare(QStringLiteral(
      "INSERT INTO visited(url, title, visit_count, last_visit)"
      " VALUES(:url, :title, 1, :t)"
      " ON CONFLICT(url) DO UPDATE SET"
      "  title = excluded.title, visit_count = visited.visit_count + 1,"
      "  last_visit = excluded.last_visit"));
  query.bindValue(":url", url);
  query.bindValue(":title", title);
  query.bindValue(":t", QDateTime::currentMSecsSinceEpoch());
  if (!query.exec()) {
    qWarning() << "shinto: recordVisit failed:" << query.lastError().text();
  }
}

QVector<HistoryStore::Suggestion> HistoryStore::completeVisited(const QString &prefix,
                                                                  int limit) const {
  QVector<Suggestion> out;
  const QString p = completionPrefix(prefix);
  if (p.size() < 2) return out;

  QSqlQuery query(db_);
  // Every http(s) visit is loaded and scored in C++ (see UrlMatch.h):
  // history is small enough that this is cheap per keystroke, and the
  // looser tiers can't be expressed as an indexable LIKE anyway. Only the
  // URL is searched. Title / typed-query text is not -- those would need
  // their own flow that actually shows the title, otherwise "git" hits
  // archive.org because its title contains "Digital". The LEFT JOIN
  // against `typed` is only for the display label of a search visit.
  query.prepare(QStringLiteral(
      "SELECT v.url, t.q, v.visit_count, v.last_visit FROM visited v"
      " LEFT JOIN typed t ON t.url = v.url"
      " WHERE v.url LIKE 'http://%' OR v.url LIKE 'https://%'"));
  if (!query.exec()) {
    qWarning() << "shinto: completeVisited failed:" << query.lastError().text();
    return out;
  }
  struct Match {
    Suggestion s;
    int depth;
    qint64 lastVisit;
  };
  QVector<Match> matches;
  while (query.next()) {
    const QString url = query.value(0).toString();
    const QString rest = completionPrefix(url);
    const auto tier = matchUrl(rest, p);
    if (tier == UrlMatchTier::None) continue;
    // A search visit's `typed.q` is the human-readable form
    // ("weather today"); anything else falls back to the URL-derived
    // label -- most search-engine urls are the noisy one here, not most
    // visits, so this is the exception, not the rule.
    const QString typedQuery = query.value(1).toString();
    const int visitCount = query.value(2).toInt();
    // Search queries stay human-readable ("weather today"). A typed URL,
    // with or without https://, uses the same scheme-stripped label as a
    // visit that never went through the omnibox.
    const QString label =
        (!typedQuery.isEmpty() && !looksLikeUrl(typedQuery)) ? typedQuery
                                                            : displayLabel(url);
    matches.push_back({{label, url, visitCount, tier}, int(rest.count(QLatin1Char('/'))),
                       query.value(3).toLongLong()});
  }
  // Stricter tiers first. Within a tier, shallower URLs first
  // (github.com before github.com/foo) so a heavily-used deep path doesn't
  // crowd the root out of `limit`; then visit_count, then recency.
  std::sort(matches.begin(), matches.end(), [](const Match &a, const Match &b) {
    if (a.s.matchTier != b.s.matchTier) return a.s.matchTier < b.s.matchTier;
    if (a.depth != b.depth) return a.depth < b.depth;
    if (a.s.visitCount != b.s.visitCount) return a.s.visitCount > b.s.visitCount;
    return a.lastVisit > b.lastVisit;
  });
  for (int i = 0; i < matches.size() && i < limit; ++i) out.push_back(matches[i].s);
  return out;
}

void HistoryStore::forgetVisited(const QString &url) {
  QSqlQuery query(db_);
  query.prepare(QStringLiteral("DELETE FROM visited WHERE url = :url"));
  query.bindValue(":url", url);
  if (!query.exec()) {
    qWarning() << "shinto: forgetVisited failed:" << query.lastError().text();
  }
  // Also drop any `typed` row for this exact URL -- completeVisited() joins
  // FROM visited, so an orphaned typed row is harmless for suggestions, but
  // it's still stale data left behind by a dismiss that's supposed to mean
  // "forget this" (confirmed concretely: funkyimg.com/x.co rows surviving
  // in `typed` after being visited, independent of the `visited` deletion
  // above -- see the history.sqlite cleanup that prompted this fix).
  QSqlQuery typedQuery(db_);
  typedQuery.prepare(QStringLiteral("DELETE FROM typed WHERE url = :url"));
  typedQuery.bindValue(":url", url);
  if (!typedQuery.exec()) {
    qWarning() << "shinto: forgetVisited (typed) failed:" << typedQuery.lastError().text();
  }
}

bool HistoryStore::isInternalUrl(const QString &url) {
  if (url.isEmpty()) return true;
  return url.startsWith(QStringLiteral("chrome://")) ||
         url.startsWith(QStringLiteral("chrome-extension://")) ||
         url.startsWith(QStringLiteral("qrc:")) || url.startsWith(QStringLiteral("about:"));
}

QString HistoryStore::toUrl(const QString &raw, const QString &searchEngineUrl) {
  const QString q = raw.trimmed();
  if (q.isEmpty()) return {};

  static const QRegularExpression kScheme(QStringLiteral("^[a-zA-Z][a-zA-Z0-9+.-]*:"));
  static const QRegularExpression kLocalhost(
      QStringLiteral("^localhost(:\\d+)?(/|$)"), QRegularExpression::CaseInsensitiveOption);
  static const QRegularExpression kIpv4(QStringLiteral("^(\\d{1,3}\\.){3}\\d{1,3}(:\\d+)?(/|$)"));
  static const QRegularExpression kBareDomain(QStringLiteral("^\\S+\\.\\S+$"));

  if (kScheme.match(q).hasMatch()) return q;
  if (q.startsWith(QStringLiteral("//"))) return QStringLiteral("https:") + q;
  if (kLocalhost.match(q).hasMatch()) return QStringLiteral("http://") + q;
  if (kIpv4.match(q).hasMatch()) return QStringLiteral("http://") + q;
  if (kBareDomain.match(q).hasMatch() && !q.contains(' ')) return QStringLiteral("https://") + q;

  QString url = searchEngineUrl;
  url.replace(QStringLiteral("%s"), QString::fromLatin1(QUrl::toPercentEncoding(q)));
  return url;
}

}  // namespace shinto
