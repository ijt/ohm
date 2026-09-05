package main

import (
	"strings"
	"testing"

	"github.com/charmbracelet/lipgloss"
)

func TestFormatBytes(t *testing.T) {
	cases := []struct {
		n    int64
		want string
	}{
		{0, "0 B"},
		{512, "512 B"},
		{1536, "1.5 KB"},
		{5 << 20, "5.0 MB"},
		{900 << 20, "900.0 MB"},
	}
	for _, c := range cases {
		if got := formatBytes(c.n); got != c.want {
			t.Errorf("formatBytes(%d) = %q, want %q", c.n, got, c.want)
		}
	}
}

func TestBarPlainMonotonicAndWidth(t *testing.T) {
	const width = 10
	prev := -1
	for pct := 0.0; pct <= 100; pct += 12.5 {
		s := barPlain(pct, width)
		if lipgloss.Width(s) != width {
			t.Fatalf("barPlain(%.1f, %d) width %d, want %d (%q)", pct, width, lipgloss.Width(s), width, s)
		}
		filled := strings.Count(s, "█")
		if filled < prev {
			t.Fatalf("barPlain filled cells decreased at %.1f%%: %q", pct, s)
		}
		prev = filled
	}
}

func TestTruncate(t *testing.T) {
	if got := truncate("hello", 10); got != "hello" {
		t.Errorf("short string changed: %q", got)
	}
	got := truncate("abcdefghijklmnopqrstuvwxyz", 8)
	if lipgloss.Width(got) != 8 {
		t.Errorf("truncate width %d, want 8 (%q)", lipgloss.Width(got), got)
	}
	if !strings.HasSuffix(got, "…") {
		t.Errorf("expected ellipsis, got %q", got)
	}
}

func TestRenderRowKeepsSelectionFullWidth(t *testing.T) {
	p := Palette{
		Bg:        lipgloss.Color("#1a1b26"),
		Fg:        lipgloss.Color("#c0caf5"),
		Accent:    lipgloss.Color("#7aa2f7"),
		Muted:     lipgloss.Color("#565f89"),
		Selection: lipgloss.Color("#292e42"),
		Success:   lipgloss.Color("#9ece6a"),
		Danger:    lipgloss.Color("#f7768e"),
		Warning:   lipgloss.Color("#e0af68"),
		Card:      lipgloss.Color("#24283b"),
	}
	d := Download{
		ID:            1,
		Filename:      "goland.tar.gz",
		TotalBytes:    900 << 20,
		ReceivedBytes: 450 << 20,
		State:         StateInProgress,
	}
	const inner = 72
	row := renderRow(d, true, inner, p)
	if lipgloss.Width(row) != inner {
		t.Fatalf("selected row width %d, want %d", lipgloss.Width(row), inner)
	}
	if !strings.Contains(row, "goland") {
		t.Fatalf("filename missing from row: %q", row)
	}
	if !strings.Contains(row, "48;2") {
		t.Fatalf("selected row missing truecolor background, got %q", row)
	}
}

func TestFrameTitleActiveCount(t *testing.T) {
	ds := []Download{
		{State: StateInProgress},
		{State: StateInProgress},
		{State: StateCompleted},
	}
	if got := frameTitle(ds); got != "Downloads · 2 active" {
		t.Fatalf("got %q", got)
	}
	if got := frameTitle(nil); got != "Downloads" {
		t.Fatalf("empty: got %q", got)
	}
}
