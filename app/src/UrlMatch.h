// How loosely typed omnibox text matches a visited URL, for history
// suggestions. Tiers go from strict to loose, and the omnibox ranks a
// stricter tier above a looser one before anything else, so loosening the
// match only adds rows below the ones plain prefix matching already gave.
#pragma once

#include <QString>

namespace shinto {

enum class UrlMatchTier {
  Prefix = 0,        // "git" -> github.com/...
  SegmentStart = 1,  // "shinto" -> github.com/ijt/shinto (starts after / . - _ + : ~)
  Substring = 2,     // "hint" -> github.com/ijt/shinto
  Scattered = 3,     // "shnto" -> github.com/ijt/shinto (letters in order, close together)
  None = 4,
};

// `target` is the URL with its scheme and a leading "www." already removed
// ("github.com/ijt/shinto"). `needle` is the typed text, likewise stripped.
// Both are compared case-insensitively. Only Prefix looks past the path
// into a query string or fragment. Scattered matches need at least 3
// typed characters, and the matched letters must fall within a window at
// most twice the typed length, so a long URL doesn't match by accident.
UrlMatchTier matchUrl(const QString &target, const QString &needle);

}  // namespace shinto
