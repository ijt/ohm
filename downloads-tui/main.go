// Shinto's downloads view: a standalone Go/Bubble Tea terminal UI, launched
// by BrowserWindow::launchDownloadsTui() (see app/src/BrowserWindow.cpp) in
// a fresh terminal via `xdg-terminal-exec -- shinto-downloads`. Reads
// DownloadManager's already-persisted downloads.sqlite directly -- no IPC
// with the running Shinto daemon at all, other than cancel_download's
// one-shot connection to the singleton socket (see actions.go).
//
// (A Ratatui/Rust implementation shipped here first; replaced after a
// side-by-side comparison spike -- same data contract, same keybindings,
// same action set, just a nicer-looking result on a couple of rendering
// details Bubble Tea made easier to get right: a highlight that reliably
// covers the whole selected row, and a popup that overlays the list
// instead of hiding it.)
package main

import (
	"fmt"
	"os"
	"strings"
	"time"

	tea "github.com/charmbracelet/bubbletea"
	"github.com/charmbracelet/lipgloss"
	"github.com/charmbracelet/x/ansi"
)

type action int

const (
	actionOpenFile action = iota
	actionOpenFolder
	actionCancelDownload
	actionClose
)

type actionItem struct {
	label string
	act   action
}

func actionsFor(s State) []actionItem {
	switch s {
	case StateInProgress:
		return []actionItem{
			{"📁 Open containing folder", actionOpenFolder},
			{"🗑 Cancel download", actionCancelDownload},
			{"✕ Close", actionClose},
		}
	case StateCompleted:
		return []actionItem{
			{"📄 Open file", actionOpenFile},
			{"📁 Open containing folder", actionOpenFolder},
			{"✕ Close", actionClose},
		}
	default: // Interrupted, Cancelled
		return []actionItem{
			{"📁 Open containing folder", actionOpenFolder},
			{"✕ Close", actionClose},
		}
	}
}

// The glyph is a leading status marker for finished/failed/cancelled rows
// (✓/✗/-) -- an in-progress row has no glyph, just its own progress bar,
// which already says "in progress" better than an arrow would.
func stateGlyphAndColor(s State, p Palette) (string, lipgloss.Color) {
	switch s {
	case StateInProgress:
		return "", p.Accent
	case StateCompleted:
		return "✓", lipgloss.Color("#4caf50")
	case StateInterrupted:
		return "✗", lipgloss.Color("#e05252")
	default:
		return "-", p.Muted
	}
}

func stateLabel(s State) string {
	switch s {
	case StateInProgress:
		return "In progress"
	case StateCompleted:
		return "Completed"
	case StateInterrupted:
		return "Failed"
	default:
		return "Cancelled"
	}
}

func bar(pct float64, width int) string {
	filled := int(pct/100.0*float64(width) + 0.5)
	if filled > width {
		filled = width
	}
	if filled < 0 {
		filled = 0
	}
	return strings.Repeat("█", filled) + strings.Repeat("░", width-filled)
}

type focusKind int

const (
	focusList focusKind = iota
	focusPopup
)

// -1 is used throughout as "no such download" (an empty list, or an id
// that's since disappeared from downloads.sqlite).
const noID int64 = -1

func indexOfID(ds []Download, id int64) int {
	for i, d := range ds {
		if d.ID == id {
			return i
		}
	}
	return -1
}

func downloadByID(ds []Download, id int64) (Download, bool) {
	i := indexOfID(ds, id)
	if i < 0 {
		return Download{}, false
	}
	return ds[i], true
}

type model struct {
	dbPath    string
	palette   Palette
	downloads []Download

	// Selection/popup target are tracked by download id, not slice index --
	// the underlying query is ORDER BY started_at DESC, so a live poll
	// (every 500ms, independent of key input -- the daemon is a separate
	// process writing to this file the whole time this view is open) can
	// reorder or shrink the slice out from under whatever index was
	// selected a moment ago. An id survives a reorder; a raw index doesn't,
	// and would either point at the wrong row (silently wrong action taken)
	// or panic outright once it runs past the end of a shrunk slice.
	selectedID    int64
	focus         focusKind
	popupID       int64
	popupSelected int

	width, height int
	quitting      bool
}

type tickMsg time.Time

// ok is false on a transient read error -- most likely SQLITE_BUSY from
// landing a poll right as the daemon commits a write (see db.go's openDB
// comment). downloads is nil in that case and must NOT be treated as "an
// empty list": that previously flashed the "No downloads yet." message
// over a real, populated list every time a poll raced a write.
type reloadedMsg struct {
	downloads []Download
	ok        bool
}

func tick() tea.Cmd {
	return tea.Tick(500*time.Millisecond, func(t time.Time) tea.Msg { return tickMsg(t) })
}

func reload(path string) tea.Cmd {
	return func() tea.Msg {
		d, err := loadAll(path)
		return reloadedMsg{downloads: d, ok: err == nil}
	}
}

func initialModel() model {
	path := dbPath()
	// A fresh window shouldn't stare at whatever finished/failed/cancelled
	// downloads piled up across every previous session -- mirrors
	// downloads-tui/src/main.rs's own startup clear_finished() call.
	_ = clearFinished(path)
	d, _ := loadAll(path)
	selectedID := noID
	if len(d) > 0 {
		selectedID = d[0].ID
	}
	return model{
		dbPath:     path,
		palette:    loadPalette(),
		downloads:  d,
		selectedID: selectedID,
		popupID:    noID,
		focus:      focusList,
	}
}

func (m model) Init() tea.Cmd {
	return tick()
}

func (m model) Update(msg tea.Msg) (tea.Model, tea.Cmd) {
	// Ctrl+C quits from either focus -- handled ahead of the focus switch
	// below so the popup can't swallow it.
	if key, ok := msg.(tea.KeyMsg); ok && key.String() == "ctrl+c" {
		m.quitting = true
		return m, tea.Quit
	}

	switch msg := msg.(type) {
	case tea.WindowSizeMsg:
		m.width, m.height = msg.Width, msg.Height
		return m, nil

	case tickMsg:
		return m, tea.Batch(reload(m.dbPath), tick())

	case reloadedMsg:
		if !msg.ok {
			// Keep showing the last-known-good list -- see reloadedMsg's
			// own doc comment. Nothing below needs to run either: the
			// selection/popup targets are still valid against the list
			// we're keeping, since it hasn't changed.
			return m, nil
		}
		m.downloads = msg.downloads
		if _, ok := downloadByID(m.downloads, m.selectedID); !ok {
			m.selectedID = noID
			if len(m.downloads) > 0 {
				m.selectedID = m.downloads[0].ID
			}
		}
		if m.focus == focusPopup {
			if d, ok := downloadByID(m.downloads, m.popupID); !ok {
				// The download the popup was open on is gone (cleared,
				// or the daemon otherwise dropped its row) -- there's
				// nothing left to act on, so fall back to the list
				// rather than risk indexing into a stale action set.
				m.focus = focusList
				m.popupID = noID
			} else {
				n := len(actionsFor(d.State))
				if m.popupSelected >= n {
					m.popupSelected = n - 1
				}
			}
		}
		return m, nil

	case tea.KeyMsg:
		switch m.focus {
		case focusList:
			switch msg.String() {
			case "q", "esc":
				m.quitting = true
				return m, tea.Quit
			case "down", "j":
				if i := indexOfID(m.downloads, m.selectedID); i >= 0 && i < len(m.downloads)-1 {
					m.selectedID = m.downloads[i+1].ID
				} else if i < 0 && len(m.downloads) > 0 {
					m.selectedID = m.downloads[0].ID
				}
			case "up", "k":
				if i := indexOfID(m.downloads, m.selectedID); i > 0 {
					m.selectedID = m.downloads[i-1].ID
				}
			case "enter":
				if _, ok := downloadByID(m.downloads, m.selectedID); ok {
					m.focus = focusPopup
					m.popupID = m.selectedID
					m.popupSelected = 0
				}
			case "c":
				_ = clearFinished(m.dbPath)
				return m, reload(m.dbPath)
			}
		case focusPopup:
			d, ok := downloadByID(m.downloads, m.popupID)
			if !ok {
				// Stale popup with nothing left to act on -- bail back
				// to the list instead of acting on a zero-value Download.
				m.focus = focusList
				break
			}
			acts := actionsFor(d.State)
			switch msg.String() {
			case "esc":
				m.focus = focusList
			case "down", "j":
				m.popupSelected = (m.popupSelected + 1) % len(acts)
			case "up", "k":
				m.popupSelected = (m.popupSelected - 1 + len(acts)) % len(acts)
			case "enter":
				switch acts[m.popupSelected].act {
				case actionOpenFile:
					openFile(d.Path)
				case actionOpenFolder:
					openFolder(d.Path)
				case actionCancelDownload:
					cancelDownload(d.ID)
				case actionClose:
				}
				m.focus = focusList
			}
		}
	}
	return m, nil
}

// centerInLine centers `text` within a `width`-cell-wide line, padding
// both sides with `fill` -- the Ratatui Block::title()/title_bottom() look:
// the text lives directly in the border line, not in the content area.
func centerInLine(text string, width int, fill string) string {
	if width < 1 {
		return ""
	}
	label := text
	if label != "" {
		label = " " + label + " "
	}
	labelW := lipgloss.Width(label)
	if labelW >= width {
		return strings.Repeat(fill, width)
	}
	left := (width - labelW) / 2
	right := width - labelW - left
	return strings.Repeat(fill, left) + label + strings.Repeat(fill, right)
}

func maxInt(a, b int) int {
	if a > b {
		return a
	}
	return b
}

// placeOverlay splices `overlay` on top of `base` at cell position (x, y),
// preserving whatever of `base` surrounds it -- unlike lipgloss.Place,
// which paints a fresh width x height canvas (losing everything under the
// popup, not just the part it covers). ansi.Cut is escape-code- and
// wide-rune-aware, so this doesn't corrupt styling at the splice points
// the way naive byte-slicing of an ANSI string would.
func placeOverlay(x, y int, overlay, base string) string {
	baseLines := strings.Split(base, "\n")
	overlayLines := strings.Split(overlay, "\n")

	overlayWidth := 0
	for _, l := range overlayLines {
		if w := ansi.StringWidth(l); w > overlayWidth {
			overlayWidth = w
		}
	}

	for i, oLine := range overlayLines {
		row := y + i
		if row < 0 || row >= len(baseLines) {
			continue
		}
		bLine := baseLines[row]
		bWidth := ansi.StringWidth(bLine)
		left := ansi.Cut(bLine, 0, x)
		var right string
		if x+overlayWidth < bWidth {
			right = ansi.Cut(bLine, x+overlayWidth, bWidth)
		}
		baseLines[row] = left + oLine + right
	}
	return strings.Join(baseLines, "\n")
}

// box draws a rounded border around `bodyLines`, `width`x`height` cells,
// with `title` embedded in the top border and `hint` in the bottom border
// -- matching downloads-tui/src/main.rs's Block::title()/title_bottom(),
// which is what made the Ratatui version's help text read as "attached to
// the window frame" rather than floating in the content area.
func box(width, height int, title, hint string, p Palette, bodyLines []string) string {
	width = maxInt(width, 4)
	height = maxInt(height, 3)
	innerWidth := width - 2
	contentHeight := height - 2

	borderStyle := lipgloss.NewStyle().Foreground(p.Accent)
	rowStyle := lipgloss.NewStyle().Width(innerWidth).Background(p.Bg).Foreground(p.Fg)

	var b strings.Builder
	b.WriteString(borderStyle.Render("╭"+centerInLine(title, innerWidth, "─")+"╮") + "\n")
	for i := 0; i < contentHeight; i++ {
		var text string
		if i < len(bodyLines) {
			text = bodyLines[i]
		}
		b.WriteString(borderStyle.Render("│") + rowStyle.Render(text) + borderStyle.Render("│") + "\n")
	}
	b.WriteString(borderStyle.Render("╰" + centerInLine(hint, innerWidth, "─") + "╯"))
	return b.String()
}

func (m model) View() string {
	if m.quitting {
		return ""
	}
	p := m.palette
	width := m.width
	if width <= 0 {
		width = 60
	}
	height := m.height
	if height <= 0 {
		height = 20
	}

	selectedIdx := indexOfID(m.downloads, m.selectedID)
	innerWidth := maxInt(width-2, 1)

	var bodyLines []string
	if len(m.downloads) == 0 {
		// Centered both ways, dim -- an empty state, not a row, so it
		// shouldn't compete with an actual download list for attention.
		// box() already pads any content shorter than contentHeight with
		// blank lines, so only the lines *above* the message need to be
		// built here to push it down to the vertical middle.
		contentHeight := maxInt(height, 3) - 2
		topPad := maxInt((contentHeight-1)/2, 0)
		bodyLines = make([]string, 0, topPad+1)
		for i := 0; i < topPad; i++ {
			bodyLines = append(bodyLines, "")
		}
		bodyLines = append(bodyLines, lipgloss.NewStyle().
			Foreground(p.Muted).
			Width(innerWidth).
			Align(lipgloss.Center).
			Render("No downloads yet."))
	} else {
		for i, d := range m.downloads {
			glyph, color := stateGlyphAndColor(d.State, p)
			prefix := glyph
			if prefix == "" {
				prefix = " " // keeps rows lined up with glyph-bearing ones
			}
			var tail string
			if d.State == StateInProgress {
				tail = bar(progressPct(d), 20) + fmt.Sprintf(" %3d%%", int(progressPct(d)+0.5))
			} else {
				tail = stateLabel(d.State)
			}
			plain := prefix + " " + d.Filename + "  " + tail

			var line string
			// Stays highlighted through the action popup too -- selectedID
			// doesn't change while the popup has focus (only popupSelected,
			// the cursor within the popup's own action list, does), and
			// popupID is set from selectedID the moment the popup opens, so
			// checking just the index is enough regardless of m.focus. Not
			// dimming this away once the popup comes up matters here
			// because the popup no longer hides the list underneath it
			// (see placeOverlay) -- with both context and the highlight
			// visible together, it's obvious what the popup is acting on.
			if i == selectedIdx {
				// A single flat style over the whole (unstyled) line,
				// padded to the full row width -- rather than wrapping an
				// already-multi-colored, already-ANSI-terminated string in
				// an outer Reverse(). That wrap only highlighted the
				// glyph: each inner Foreground().Render() call ends with
				// its own SGR reset, which cuts the outer reverse off
				// right after the first one, leaving the rest of the row
				// (and the row-padding lipgloss adds afterward, styled
				// with a plain, non-reversed style) unhighlighted.
				//
				// p.Muted (not p.Accent) as the fill -- a dim, subtle bar
				// rather than a bright block; still reads clearly as "this
				// row" against the plain background without shouting.
				line = lipgloss.NewStyle().
					Background(p.Muted).
					Foreground(p.Fg).
					Width(innerWidth).
					Render(plain)
			} else {
				line = lipgloss.NewStyle().Foreground(color).Render(prefix+" ") + d.Filename
				if d.State == StateInProgress {
					line += "  " + lipgloss.NewStyle().Foreground(p.Accent).Render(bar(progressPct(d), 20)) +
						fmt.Sprintf(" %3d%%", int(progressPct(d)+0.5))
				} else {
					line += "  " + lipgloss.NewStyle().Foreground(color).Render(stateLabel(d.State))
				}
			}
			bodyLines = append(bodyLines, line)
		}
	}

	frame := box(width, height, "Shinto Downloads", "j/k move  ⏎ act  c clear finished  q quit", p, bodyLines)

	d, ok := downloadByID(m.downloads, m.popupID)
	if m.focus != focusPopup || !ok {
		return frame
	}

	acts := actionsFor(d.State)
	var lines []string
	for i, a := range acts {
		s := lipgloss.NewStyle()
		if i == m.popupSelected {
			s = s.Reverse(true)
		}
		lines = append(lines, s.Render(a.label))
	}
	popupBody := strings.Join(lines, "\n")
	popup := lipgloss.NewStyle().
		Foreground(p.Fg).
		Background(p.Bg).
		BorderStyle(lipgloss.RoundedBorder()).
		BorderForeground(p.Accent).
		Padding(0, 1).
		Render(lipgloss.NewStyle().Bold(true).Render("Action") + "\n" + popupBody)

	// Overlaid on the frame, not replacing it -- so the download the popup
	// is acting on (and the rest of the list) stays visible around it,
	// same as Ratatui's Clear-a-sub-rect-then-draw-on-top approach.
	popupWidth := 0
	for _, l := range strings.Split(popup, "\n") {
		if w := ansi.StringWidth(l); w > popupWidth {
			popupWidth = w
		}
	}
	popupHeight := len(strings.Split(popup, "\n"))
	x := maxInt((width-popupWidth)/2, 0)
	y := maxInt((height-popupHeight)/2, 0)
	return placeOverlay(x, y, popup, frame)
}

// progressPct is only ever meaningful for an in-progress download -- 0 for
// anything else, and for an in-progress one with an unknown total size
// (total_bytes == 0, seen for streamed/chunked downloads Chromium hasn't
// learned a Content-Length for yet).
func progressPct(d Download) float64 {
	if d.State != StateInProgress || d.TotalBytes <= 0 {
		return 0
	}
	return float64(d.ReceivedBytes) / float64(d.TotalBytes) * 100.0
}

func main() {
	p := tea.NewProgram(initialModel(), tea.WithAltScreen())
	if _, err := p.Run(); err != nil {
		fmt.Fprintln(os.Stderr, "shinto-downloads:", err)
		os.Exit(1)
	}
}
