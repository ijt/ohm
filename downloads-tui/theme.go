// Reads Omarchy's live colors.toml so this TUI matches the same palette
// Shinto's Qt side uses (see app/src/ThemeLoader.cpp's loadPalette(), which
// this mirrors -- same as downloads-tui/src/theme.rs). No toml dependency:
// colors.toml is flat `key = "value"` lines, so a hand-rolled line parser
// is enough.
package main

import (
	"bufio"
	"os"
	"path/filepath"
	"strconv"
	"strings"

	"github.com/charmbracelet/lipgloss"
)

type Palette struct {
	Bg     lipgloss.Color
	Fg     lipgloss.Color
	Accent lipgloss.Color
	Muted  lipgloss.Color
}

func parseHex(hex string) (string, bool) {
	hex = strings.TrimSpace(hex)
	hex = strings.TrimPrefix(hex, "#")
	if len(hex) != 6 {
		return "", false
	}
	for _, c := range hex {
		if _, err := strconv.ParseInt(string(c), 16, 32); err != nil {
			return "", false
		}
	}
	return "#" + hex, true
}

func parseFlatToml(path string) map[string]string {
	out := map[string]string{}
	f, err := os.Open(path)
	if err != nil {
		return out
	}
	defer f.Close()

	scanner := bufio.NewScanner(f)
	for scanner.Scan() {
		line := strings.TrimSpace(scanner.Text())
		if line == "" || strings.HasPrefix(line, "#") || strings.HasPrefix(line, "[") {
			continue
		}
		key, value, found := strings.Cut(line, "=")
		if !found {
			continue
		}
		key = strings.TrimSpace(key)
		value = strings.TrimSpace(value)
		if len(value) >= 2 && strings.HasPrefix(value, `"`) && strings.HasSuffix(value, `"`) {
			value = value[1 : len(value)-1]
		}
		out[key] = value
	}
	return out
}

// Matches shinto::colorsTomlPath() in app/src/Shinto.h exactly.
func colorsTomlPath() string {
	home, err := os.UserHomeDir()
	if err != nil {
		home = "."
	}
	return filepath.Join(home, ".local/state/omarchy/current/theme/colors.toml")
}

// Same fallback hex defaults as Palette's struct defaults in
// app/src/ThemeLoader.h, for a missing file or a missing key.
func loadPalette() Palette {
	toml := parseFlatToml(colorsTomlPath())
	pick := func(key, def string) lipgloss.Color {
		if v, ok := toml[key]; ok {
			if hex, ok := parseHex(v); ok {
				return lipgloss.Color(hex)
			}
		}
		hex, _ := parseHex(def)
		return lipgloss.Color(hex)
	}
	return Palette{
		Bg:     pick("background", "#1a1b26"),
		Fg:     pick("bright_foreground", "#c0caf5"),
		Accent: pick("accent", "#7aa2f7"),
		Muted:  pick("dark_foreground", "#565f89"),
	}
}
