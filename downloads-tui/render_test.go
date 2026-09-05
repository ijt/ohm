package main

import (
	"strings"
	"testing"
	"time"

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

func TestSparklineWidthAndActivity(t *testing.T) {
	const width = 10
	empty := sparkline(nil, width)
	if lipgloss.Width(empty) != width {
		t.Fatalf("empty sparkline width %d, want %d (%q)", lipgloss.Width(empty), width, empty)
	}
	for _, r := range empty {
		if r != sparkBars[0] {
			t.Fatalf("empty sparkline should be all floor glyphs, got %q", empty)
		}
	}
	s := sparkline([]float64{1, 2, 4, 8, 4, 2}, width)
	if lipgloss.Width(s) != width {
		t.Fatalf("sparkline width %d, want %d (%q)", lipgloss.Width(s), width, s)
	}
	if !strings.ContainsRune(s, sparkBars[len(sparkBars)-1]) {
		t.Fatalf("peak rate should reach max glyph, got %q", s)
	}
}

func TestRecordHistoryOnlyOnAdvance(t *testing.T) {
	m := model{}
	d := Download{ID: 1, ReceivedBytes: 100, State: StateInProgress}
	m.recordHistory([]Download{d})
	if len(m.ratesFor(1)) != 0 {
		t.Fatalf("baseline sample should not invent a rate, got %v", m.ratesFor(1))
	}
	d.ReceivedBytes = 100
	m.recordHistory([]Download{d})
	if len(m.ratesFor(1)) != 0 {
		t.Fatalf("unchanged received_bytes must not append, got %v", m.ratesFor(1))
	}
	time.Sleep(10 * time.Millisecond)
	d.ReceivedBytes = 1100
	m.recordHistory([]Download{d})
	rates := m.ratesFor(1)
	if len(rates) != 1 {
		t.Fatalf("expected one rate after advance, got %v", rates)
	}
	if rates[0] <= 0 {
		t.Fatalf("rate should be positive, got %v", rates[0])
	}
	// Finished / missing ids are pruned.
	m.recordHistory([]Download{{ID: 1, ReceivedBytes: 1100, State: StateCompleted}})
	if m.ratesFor(1) != nil {
		t.Fatalf("completed download should drop history")
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
	rates := []float64{1e6, 2e6, 1.5e6, 3e6, 2e6}
	row := renderRow(d, true, inner, p, rates)
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
