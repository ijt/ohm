// Parses Omarchy's colors.toml into a small palette and renders it to a Qt
// stylesheet for the OmniboxOverlay. Replaces bash `toml_color`/`write_theme`
// and extension/theme.css.
#pragma once

#include <QString>

namespace ohm {

struct Palette {
  QString bg = QStringLiteral("#1a1b26");
  QString fg = QStringLiteral("#c0caf5");
  QString accent = QStringLiteral("#7aa2f7");
  QString muted = QStringLiteral("#565f89");
  QString card = QStringLiteral("#24283b");
  // Not from colors.toml -- fonts aren't a per-theme thing in Omarchy, they're
  // a separate system-wide choice (`omarchy-font-set`). This default is the
  // last-resort fallback (both if fontconfig has nothing to say and as the
  // tail of the QSS font-family chain either way); loadPalette() normally
  // prepends the live-resolved system monospace font ahead of it.
  QString font = QStringLiteral(
      "'JetBrainsMono Nerd Font','JetBrains Mono',ui-monospace,monospace");

  // A QSS stylesheet for OmniboxOverlay using these colors.
  QString toQss() const;
};

// Reads ~/.local/state/omarchy/current/theme/colors.toml (via
// ohm::colorsTomlPath()) for colors, and resolves the live system
// monospace font (see resolveMonospaceFont() in the .cpp) for the font.
// Missing colors.toml keys, a missing file entirely, or no resolvable font
// all fall back to Palette's defaults above -- same behavior as the old
// bash `toml_color`, extended to the font.
Palette loadPalette();

}  // namespace ohm
