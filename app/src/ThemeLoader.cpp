#include "ThemeLoader.h"

#include <QFile>
#include <QFont>
#include <QFontInfo>
#include <QHash>
#include <QTextStream>

#include "Shinto.h"

namespace shinto {

namespace {

// Omarchy's `omarchy-font-set` writes ~/.config/fontconfig/fonts.conf
// aliasing the literal family name "monospace" to whatever font the user
// picked -- its own comment on that file says plainly: "fontconfig is the
// canonical source of truth -- the omarchy shell, Qt apps, and anything
// resolving 'monospace' all read from here." Asking Qt to resolve that
// same family name (confirmed: matches `fc-match monospace`) picks up the
// live choice, Omarchy or not -- a vanilla Linux system's fontconfig still
// resolves "monospace" to *something* reasonable, just not one Omarchy
// picked. An empty result (no fontconfig at all, somehow) falls back to
// Palette's own hardcoded default entirely.
QString resolveMonospaceFont() {
  QFont font(QStringLiteral("monospace"));
  font.setStyleHint(QFont::Monospace);
  return QFontInfo(font).family();
}

// colors.toml is flat `key = "value"` lines (confirmed against a live
// Omarchy theme file) -- no need for a real TOML parser.
QHash<QString, QString> parseFlatToml(const QString &path) {
  QHash<QString, QString> out;
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
    return out;
  }
  QTextStream stream(&file);
  while (!stream.atEnd()) {
    QString line = stream.readLine().trimmed();
    if (line.isEmpty() || line.startsWith('#') || line.startsWith('[')) {
      continue;
    }
    const int eq = line.indexOf('=');
    if (eq < 0) {
      continue;
    }
    QString key = line.left(eq).trimmed();
    QString value = line.mid(eq + 1).trimmed();
    if (value.startsWith('"') && value.endsWith('"') && value.size() >= 2) {
      value = value.mid(1, value.size() - 2);
    }
    out.insert(key, value);
  }
  return out;
}

QString pick(const QHash<QString, QString> &toml, const char *key, const QString &fallback) {
  const auto it = toml.constFind(QString::fromLatin1(key));
  if (it == toml.constEnd() || it.value().isEmpty()) {
    return fallback;
  }
  return it.value();
}

}  // namespace

Palette loadPalette() {
  Palette palette;
  const QHash<QString, QString> toml = parseFlatToml(colorsTomlPath());
  palette.bg = pick(toml, "background", palette.bg);
  palette.fg = pick(toml, "bright_foreground", palette.fg);
  palette.accent = pick(toml, "accent", palette.accent);
  palette.muted = pick(toml, "dark_foreground", palette.muted);
  palette.card = pick(toml, "lighter_background", palette.card);

  const QString resolvedFont = resolveMonospaceFont();
  if (!resolvedFont.isEmpty()) {
    // Prepended, not replaced -- if the resolved name somehow can't
    // actually be used (a quoting edge case, a font that vanished between
    // resolution and paint), QSS's own font-family fallback chain still
    // has something to land on.
    palette.font = QStringLiteral("'%1',%2").arg(resolvedFont, palette.font);
  }
  return palette;
}

QString Palette::toQss() const {
  return QStringLiteral(
             "#OmniboxOverlay { background: %1; }\n"
             // No visible field at rest -- just the caret. Same style
             // whether it's a fresh window's gate or Ctrl+L on a loaded
             // page (both show this same gate now).
             "#OmniboxOverlay QLineEdit {"
             " background: transparent; color: #ffffff; border: none;"
             " padding: 0; font-family: %5; font-size: 15px;"
             " selection-background-color: %6; }\n"
             "#OmniboxOverlay QListWidget {"
             " background: %2; color: %3; border: none;"
             " font-family: %5; outline: none; }\n"
             "#OmniboxOverlay QListWidget::item:selected { background: %6; color: %2; }\n"
             // History suggestion rows (SuggestionRow/SuggestionLabel/
             // SuggestionDismiss): a real child widget per row, not plain
             // item text (see OmniboxOverlay::renderSuggestions) -- keep it
             // transparent so QListWidget's own selection highlight above
             // still shows through, and match the label to plain items.
             "#OmniboxOverlay #SuggestionRow { background: transparent; }\n"
             "#OmniboxOverlay #SuggestionLabel { color: %3; font-family: %5; background: transparent; }\n"
             "#OmniboxOverlay #SuggestionDismiss {"
             " color: %4; background: transparent; border: none; font-family: %5; }\n"
             "#OmniboxOverlay #SuggestionDismiss:hover { color: %3; }\n"
             "#OmniboxOverlay #ProgressBar { background: %6; border: none; }\n"
             "#OmniboxOverlay #ShortcutsHint { color: %4; background: transparent;"
             " font-family: %5; font-size: 13px; }\n")
      .arg(bg, card, fg, muted, font, accent);
}

}  // namespace shinto
