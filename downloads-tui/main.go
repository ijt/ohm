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
	"unicode/utf8"

	tea "github.com/charmbracelet/bubbletea"
	"github.com/charmbracelet/lipgloss"
	"github.com/charmbracelet/x/ansi"
	"github.com/muesli/termenv"
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
			{"Open containing folder", actionOpenFolder},
			{"Cancel download", actionCancelDownload},
			{"Close", actionClose},
		}
	case StateCompleted:
		return []actionItem{
			{"Open file", actionOpenFile},
			{"Open containing folder", actionOpenFolder},
			{"Close", actionClose},
		}
	default: // Interrupted, Cancelled
		return []actionItem{
			{"Open containing folder", actionOpenFolder},
			{"Close", actionClose},
		}
	}
}

// Leading status marker for finished/failed/cancelled rows. In-progress
// rows use a live progress bar instead of a glyph.
func stateGlyphAndColor(s State, p Palette) (string, lipgloss.Color) {
	switch s {
	case StateInProgress:
		return "●", p.Accent
	case StateCompleted:
		return "✓", p.Success
	case StateInterrupted:
		return "✗", p.Danger
	default:
		return "–", p.Muted
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

// Eighth-block partial fill -- denser than plain █/░, closer to btop's
// braille meters without needing a full graph history.
var barEighths = []string{"", "▏", "▎", "▍", "▌", "▋", "▊", "▉"}

func barPlain(pct float64, width int) string {
	if width <= 0 {
		return ""
	}
	if pct < 0 {
		pct = 0
	}
	if pct > 100 {
		pct = 100
	}
	filled := pct / 100.0 * float64(width)
	full := int(filled)
	frac := int((filled - float64(full)) * 8)
	if full > width {
		full = width
		frac = 0
	}
	var b strings.Builder
	b.WriteString(strings.Repeat("█", full))
	if full < width {
		partial := barEighths[frac]
		if partial == "" {
			partial = "░"
			b.WriteString(partial)
			b.WriteString(strings.Repeat("░", width-full-1))
		} else {
			b.WriteString(partial)
			b.WriteString(strings.Repeat("░", width-full-1))
		}
	}
	return b.String()
}

func formatBytes(n int64) string {
	if n < 0 {
		n = 0
	}
	const unit = 1024
	if n < unit {
		return fmt.Sprintf("%d B", n)
	}
	div, exp := int64(unit), 0
	for v := n / unit; v >= unit; v /= unit {
		div *= unit
		exp++
	}
	return fmt.Sprintf("%.1f %cB", float64(n)/float64(div), "KMGTPE"[exp])
}

func truncate(s string, width int) string {
	if width <= 0 {
		return ""
	}
	if lipgloss.Width(s) <= width {
		return s
	}
	if width == 1 {
		return "…"
	}
	runes := []rune(s)
	for len(runes) > 0 && lipgloss.Width(string(runes)+"…") > width {
		runes = runes[:len(runes)-1]
	}
	if len(runes) == 0 {
		return "…"
	}
	return string(runes) + "…"
}

// Right-hand column: progress meter + sizes for active rows, size + status
// label for finished ones. Plain (no ANSI) so width math stays honest.
func rightPlain(d Download, barWidth int) string {
	switch d.State {
	case StateInProgress:
		if d.TotalBytes <= 0 {
			// Streamed/chunked -- no Content-Length yet. Show received only.
			return fmt.Sprintf("%s  %s", strings.Repeat("─", barWidth), formatBytes(d.ReceivedBytes))
		}
		pct := progressPct(d)
		return fmt.Sprintf("%s %3d%%  %s/%s",
			barPlain(pct, barWidth),
			int(pct+0.5),
			formatBytes(d.ReceivedBytes),
			formatBytes(d.TotalBytes))
	default:
		size := ""
		if d.TotalBytes > 0 {
			size = formatBytes(d.TotalBytes) + "  "
		} else if d.ReceivedBytes > 0 {
			size = formatBytes(d.ReceivedBytes) + "  "
		}
		return size + stateLabel(d.State)
	}
}

func barWidthFor(innerWidth int) int {
	// Keep the meter readable without crowding out the filename on narrow
	// terminals; grow a little when there's room.
	switch {
	case innerWidth >= 100:
		return 16
	case innerWidth >= 80:
		return 14
	case innerWidth >= 60:
		return 12
	default:
		return 8
	}
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

func countActive(ds []Download) int {
	n := 0
	for _, d := range ds {
		if d.State == StateInProgress {
			n++
		}
	}
	return n
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
	scroll        int

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

func (m *model) clampScroll() {
	contentHeight := maxInt(m.height-2, 1)
	maxScroll := maxInt(len(m.downloads)-contentHeight, 0)
	if m.scroll > maxScroll {
		m.scroll = maxScroll
	}
	if m.scroll < 0 {
		m.scroll = 0
	}
	idx := indexOfID(m.downloads, m.selectedID)
	if idx < 0 {
		return
	}
	if idx < m.scroll {
		m.scroll = idx
	}
	if idx >= m.scroll+contentHeight {
		m.scroll = idx - contentHeight + 1
	}
	if m.scroll > maxScroll {
		m.scroll = maxScroll
	}
	if m.scroll < 0 {
		m.scroll = 0
	}
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
		m.clampScroll()
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
		// Pick up Omarchy theme switches without restarting the window.
		m.palette = loadPalette()
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
		m.clampScroll()
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
				m.clampScroll()
			case "up", "k":
				if i := indexOfID(m.downloads, m.selectedID); i > 0 {
					m.selectedID = m.downloads[i-1].ID
				}
				m.clampScroll()
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
// padInner pads `text` to `width` cells with background-colored spaces.
// Unlike wrapping the whole line in a lipgloss Width()/Background() style,
// this leaves any ANSI already in `text` (selection bar, progress colors)
// intact -- an outer Background().Render() was flattening those.
func padInner(text string, width int, p Palette) string {
	w := lipgloss.Width(text)
	if w > width {
		return ansi.Cut(text, 0, width)
	}
	if w == width {
		return text
	}
	pad := lipgloss.NewStyle().Background(p.Bg).Render(strings.Repeat(" ", width-w))
	return text + pad
}

func box(width, height int, title, hint string, p Palette, bodyLines []string) string {
	width = maxInt(width, 4)
	height = maxInt(height, 3)
	innerWidth := width - 2
	contentHeight := height - 2

	borderStyle := lipgloss.NewStyle().Foreground(p.Accent)

	var b strings.Builder
	b.WriteString(borderStyle.Render("╭"+centerInLine(title, innerWidth, "─")+"╮") + "\n")
	for i := 0; i < contentHeight; i++ {
		var text string
		if i < len(bodyLines) {
			text = bodyLines[i]
		}
		b.WriteString(borderStyle.Render("│") + padInner(text, innerWidth, p) + borderStyle.Render("│") + "\n")
	}
	b.WriteString(borderStyle.Render("╰" + centerInLine(hint, innerWidth, "─") + "╯"))
	return b.String()
}

func frameTitle(ds []Download) string {
	if n := countActive(ds); n > 0 {
		return fmt.Sprintf("Downloads · %d active", n)
	}
	if len(ds) == 0 {
		return "Downloads"
	}
	return fmt.Sprintf("Downloads · %d", len(ds))
}

// renderRow builds one list row. Selected rows are a single flat style over
// an unstyled string padded to full width -- wrapping already-colored ANSI
// in an outer style only highlights the first segment (each inner
// Foreground().Render() resets SGR). Unselected rows keep per-segment color.
func renderRow(d Download, selected bool, innerWidth int, p Palette) string {
	glyph, color := stateGlyphAndColor(d.State, p)
	bw := barWidthFor(innerWidth)
	right := rightPlain(d, bw)

	// "● name ……  ████ 45%  12MB/90MB" -- glyph + spaces + name + gap + right
	prefix := glyph + " "
	gap := "  "
	nameWidth := innerWidth - lipgloss.Width(prefix) - lipgloss.Width(gap) - lipgloss.Width(right)
	if nameWidth < 4 {
		// Ultra-narrow: drop the right column before crushing the name to
		// nothing useful.
		right = ""
		gap = ""
		nameWidth = innerWidth - lipgloss.Width(prefix)
	}
	name := truncate(d.Filename, maxInt(nameWidth, 1))
	// Pad the name so the right column stays right-aligned across rows.
	namePad := maxInt(nameWidth-lipgloss.Width(name), 0)
	plain := prefix + name + strings.Repeat(" ", namePad) + gap + right
	if lipgloss.Width(plain) > innerWidth {
		plain = truncate(plain, innerWidth)
	}

	if selected {
		// p.Selection (not Reverse/Accent) -- a quiet bar that still reads
		// as "this row" against the list background, matching Omarchy's
		// selection token used elsewhere in the desktop.
		return lipgloss.NewStyle().
			Background(p.Selection).
			Foreground(p.Fg).
			Width(innerWidth).
			Render(plain)
	}

	var b strings.Builder
	b.WriteString(lipgloss.NewStyle().Foreground(color).Render(prefix))
	b.WriteString(lipgloss.NewStyle().Foreground(p.Fg).Render(name))
	b.WriteString(strings.Repeat(" ", namePad))
	b.WriteString(gap)
	if d.State == StateInProgress {
		b.WriteString(styleProgressRight(d, bw, p))
	} else {
		b.WriteString(lipgloss.NewStyle().Foreground(color).Render(right))
	}
	return b.String()
}

func styleProgressRight(d Download, barWidth int, p Palette) string {
	muted := lipgloss.NewStyle().Foreground(p.Muted)
	accent := lipgloss.NewStyle().Foreground(p.Accent)
	if d.TotalBytes <= 0 {
		return accent.Render(strings.Repeat("─", barWidth)) + muted.Render("  "+formatBytes(d.ReceivedBytes))
	}
	pct := progressPct(d)
	plain := barPlain(pct, barWidth)
	// Color filled cells accent, empty muted -- walk the plain bar so
	// partial eighth-blocks stay accent too (they're "filled").
	var barStyled strings.Builder
	empty := false
	for _, r := range plain {
		ch := string(r)
		if ch == "░" {
			empty = true
		}
		if empty {
			barStyled.WriteString(muted.Render(ch))
		} else {
			barStyled.WriteString(accent.Render(ch))
		}
	}
	meta := fmt.Sprintf(" %3d%%  %s/%s", int(pct+0.5), formatBytes(d.ReceivedBytes), formatBytes(d.TotalBytes))
	return barStyled.String() + muted.Render(meta)
}

func emptyStateLines(innerWidth, contentHeight int, p Palette) []string {
	lines := []string{
		lipgloss.NewStyle().Foreground(p.Muted).Width(innerWidth).Align(lipgloss.Center).
			Render("No downloads yet"),
		lipgloss.NewStyle().Foreground(p.Muted).Width(innerWidth).Align(lipgloss.Center).
			Render("Files you save in Shinto show up here"),
	}
	topPad := maxInt((contentHeight-len(lines))/2, 0)
	out := make([]string, 0, topPad+len(lines))
	for i := 0; i < topPad; i++ {
		out = append(out, "")
	}
	return append(out, lines...)
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
	contentHeight := maxInt(height-2, 1)

	var bodyLines []string
	if len(m.downloads) == 0 {
		bodyLines = emptyStateLines(innerWidth, contentHeight, p)
	} else {
		start := m.scroll
		if start > len(m.downloads) {
			start = 0
		}
		end := start + contentHeight
		if end > len(m.downloads) {
			end = len(m.downloads)
		}
		for i := start; i < end; i++ {
			bodyLines = append(bodyLines, renderRow(m.downloads[i], i == selectedIdx, innerWidth, p))
		}
	}

	hint := "j/k move  ⏎ act  c clear finished  q quit"
	if len(m.downloads) > contentHeight {
		// Scroll affordance in the bottom border without eating a content row.
		shown := fmt.Sprintf("%d–%d/%d", m.scroll+1, minInt(m.scroll+contentHeight, len(m.downloads)), len(m.downloads))
		hint = shown + "  ·  " + hint
	}

	frame := box(width, height, frameTitle(m.downloads), hint, p, bodyLines)

	d, ok := downloadByID(m.downloads, m.popupID)
	if m.focus != focusPopup || !ok {
		return frame
	}

	acts := actionsFor(d.State)
	labelWidth := utf8.RuneCountInString("Cancel download") // widest action label
	for _, a := range acts {
		if w := lipgloss.Width(a.label); w > labelWidth {
			labelWidth = w
		}
	}
	var lines []string
	for i, a := range acts {
		style := lipgloss.NewStyle().Width(labelWidth).Foreground(p.Fg)
		if i == m.popupSelected {
			style = style.Background(p.Selection).Foreground(p.Fg)
		}
		lines = append(lines, style.Render(a.label))
	}
	subtitle := truncate(d.Filename, maxInt(labelWidth, 8))
	popup := lipgloss.NewStyle().
		Foreground(p.Fg).
		Background(p.Card).
		BorderStyle(lipgloss.RoundedBorder()).
		BorderForeground(p.Accent).
		Padding(0, 1).
		Render(
			lipgloss.NewStyle().Bold(true).Foreground(p.Accent).Render("Action") + "\n" +
				lipgloss.NewStyle().Foreground(p.Muted).Render(subtitle) + "\n" +
				strings.Join(lines, "\n"),
		)

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

func minInt(a, b int) int {
	if a < b {
		return a
	}
	return b
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

func init() {
	// Force truecolor so Omarchy's hex palette isn't crushed to 256-color
	// approximations (or stripped entirely when stdout isn't a tty during
	// tests/pipes). The downloads window is always launched in a real
	// terminal via xdg-terminal-exec.
	lipgloss.SetColorProfile(termenv.TrueColor)
}

func main() {
	p := tea.NewProgram(initialModel(), tea.WithAltScreen())
	if _, err := p.Run(); err != nil {
		fmt.Fprintln(os.Stderr, "shinto-downloads:", err)
		os.Exit(1)
	}
}
