#include "UrlMatch.h"

#include <QRegularExpression>

namespace shinto {

namespace {

bool isSegmentSeparator(QChar c) {
  switch (c.unicode()) {
    case '/': case '.': case '-': case '_': case '+': case ':': case '~':
      return true;
    default:
      return false;
  }
}

// Width of the narrowest window of `s` holding `p`'s characters in order,
// or -1 if they don't all appear in order.
int narrowestSpan(const QString &s, const QString &p) {
  int best = -1;
  for (int start = s.indexOf(p[0]); start >= 0; start = s.indexOf(p[0], start + 1)) {
    int i = start;
    int j = 0;
    while (i < s.size() && j < p.size()) {
      if (s[i] == p[j]) ++j;
      ++i;
    }
    if (j < p.size()) break;  // later starts can't finish either
    const int span = i - start;
    if (best < 0 || span < best) best = span;
  }
  return best;
}

}  // namespace

UrlMatchTier matchUrl(const QString &target, const QString &needle) {
  if (needle.isEmpty()) return UrlMatchTier::None;
  QString s = target.toLower();
  const QString p = needle.toLower();
  if (s.startsWith(p)) return UrlMatchTier::Prefix;

  // The looser tiers only look at host and path. Query strings and
  // fragments are long, mostly opaque tokens (OAuth redirects, tracking
  // params) that would otherwise match nearly anything.
  const int queryStart = s.indexOf(QRegularExpression(QStringLiteral("[?#]")));
  if (queryStart >= 0) s.truncate(queryStart);

  bool substring = false;
  for (int i = s.indexOf(p, 1); i >= 0; i = s.indexOf(p, i + 1)) {
    if (isSegmentSeparator(s[i - 1])) return UrlMatchTier::SegmentStart;
    substring = true;
  }
  if (substring) return UrlMatchTier::Substring;

  if (p.size() < 3) return UrlMatchTier::None;
  const int span = narrowestSpan(s, p);
  if (span >= 0 && span <= 2 * p.size()) return UrlMatchTier::Scattered;
  return UrlMatchTier::None;
}

}  // namespace shinto
