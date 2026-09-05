// Shinto's downloads view: a standalone terminal UI, launched by
// BrowserWindow::launchDownloadsTui() (see app/src/BrowserWindow.cpp) in a
// fresh terminal via `xdg-terminal-exec -- shinto-downloads`. Reads
// DownloadManager's already-persisted downloads.sqlite directly -- no IPC
// with the running Shinto daemon at all, just a shared SQLite file.

use std::env;
use std::io::{Read, Write};
use std::os::unix::net::UnixStream;
use std::path::PathBuf;
use std::process::{Command, Stdio};
use std::time::{Duration, Instant};

use crossterm::event::{self, Event, KeyCode, KeyEventKind};
use ratatui::layout::{Alignment, Constraint, Direction, Layout, Rect};
use ratatui::style::{Color, Modifier, Style};
use ratatui::text::{Line, Span};
use ratatui::widgets::{Block, Borders, Clear, List, ListItem, ListState, Paragraph};
use ratatui::Frame;

mod db;
mod theme;

use db::{Download, State};
use theme::Palette;

fn main() {
    let mut terminal = ratatui::init();
    let result = run(&mut terminal);
    ratatui::restore();
    if let Err(e) = result {
        eprintln!("shinto-downloads: {e}");
        std::process::exit(1);
    }
}

/// Focus determines which widget Enter/Escape/arrow keys act on.
enum Focus {
    List,
    ActionPopup { target: usize, selected: usize },
}

fn run(terminal: &mut ratatui::DefaultTerminal) -> Result<(), Box<dyn std::error::Error>> {
    let palette = theme::load_palette();
    let db_path = db_path();

    // A fresh window shouldn't have to stare at whatever
    // finished/failed/cancelled downloads piled up across every previous
    // session -- only what's actually still running matters at that
    // point. `c` (below) does the same thing on demand for a session
    // that's been open a while and accumulated its own finished ones.
    let _ = db::clear_finished(&db_path);

    let mut downloads = db::load_all(&db_path).unwrap_or_default();
    let mut list_state = ListState::default();
    if !downloads.is_empty() {
        list_state.select(Some(0));
    }
    let mut focus = Focus::List;
    let mut last_poll = Instant::now();

    loop {
        terminal.draw(|frame| draw(frame, &downloads, &mut list_state, &focus, &palette))?;

        // Re-read downloads.sqlite roughly every 500ms regardless of key
        // input -- the daemon is a separate process actively writing to
        // this file the whole time this view is open.
        if last_poll.elapsed() >= Duration::from_millis(500) {
            if let Ok(fresh) = db::load_all(&db_path) {
                downloads = fresh;
                if let Some(selected) = list_state.selected() {
                    if selected >= downloads.len() && !downloads.is_empty() {
                        list_state.select(Some(downloads.len() - 1));
                    }
                }
            }
            last_poll = Instant::now();
        }

        if event::poll(Duration::from_millis(250)).unwrap_or(false) {
            if let Event::Key(key) = event::read()? {
                if key.kind != KeyEventKind::Press {
                    continue;
                }
                match &mut focus {
                    Focus::List => match key.code {
                        KeyCode::Char('q') | KeyCode::Esc => return Ok(()),
                        KeyCode::Down | KeyCode::Char('j') => select_next(&mut list_state, downloads.len()),
                        KeyCode::Up | KeyCode::Char('k') => select_prev(&mut list_state, downloads.len()),
                        KeyCode::Enter => {
                            if let Some(target) = list_state.selected() {
                                if target < downloads.len() {
                                    focus = Focus::ActionPopup { target, selected: 0 };
                                }
                            }
                        }
                        KeyCode::Char('c') => {
                            let _ = db::clear_finished(&db_path);
                            downloads = db::load_all(&db_path).unwrap_or_default();
                            list_state.select(if downloads.is_empty() { None } else { Some(0) });
                        }
                        _ => {}
                    },
                    Focus::ActionPopup { target, selected } => {
                        let count = downloads
                            .get(*target)
                            .map(|d| actions_for(d.state).len())
                            .unwrap_or(1);
                        match key.code {
                            KeyCode::Esc => focus = Focus::List,
                            KeyCode::Down | KeyCode::Char('j') => *selected = (*selected + 1) % count,
                            KeyCode::Up | KeyCode::Char('k') => {
                                *selected = (*selected + count - 1) % count
                            }
                            KeyCode::Enter => {
                                if let Some(d) = downloads.get(*target) {
                                    if let Some((_, action)) = actions_for(d.state).get(*selected) {
                                        match action {
                                            Action::OpenFile => open_file(&d.path),
                                            Action::OpenFolder => open_folder(&d.path),
                                            Action::CancelDownload => cancel_download(d.id),
                                            Action::Close => {}
                                        }
                                    }
                                }
                                focus = Focus::List;
                            }
                            _ => {}
                        }
                    }
                }
            }
        }
    }
}

fn select_next(state: &mut ListState, len: usize) {
    if len == 0 {
        return;
    }
    let next = state.selected().map(|i| (i + 1).min(len - 1)).unwrap_or(0);
    state.select(Some(next));
}

fn select_prev(state: &mut ListState, len: usize) {
    if len == 0 {
        return;
    }
    let prev = state.selected().map(|i| i.saturating_sub(1)).unwrap_or(0);
    state.select(Some(prev));
}

// Every child process launched from here has its stdio nulled -- left
// inherited (the default), anything the child prints (dbus-send's own
// output, or a freshly-launched file manager's own startup warnings/logs)
// lands directly in this process's raw-mode terminal screen, corrupting
// the TUI (confirmed concretely: "log spam" appearing over the display
// after opening a containing folder).
fn open_file(path: &str) {
    let _ = Command::new("xdg-open")
        .arg(path)
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .spawn();
}

// org.freedesktop.FileManager1.ShowItems opens the folder with `path`
// highlighted (most file managers implement it); falls back to a plain
// (unselected) folder-open via xdg-open if nothing answers.
fn open_folder(path: &str) {
    let uri = format!("file://{path}");
    let status = Command::new("dbus-send")
        .args([
            "--session",
            "--print-reply",
            "--dest=org.freedesktop.FileManager1",
            "/org/freedesktop/FileManager1",
            "org.freedesktop.FileManager1.ShowItems",
            &format!("array:string:{uri}"),
            "string:",
        ])
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .status();
    let ok = matches!(status, Ok(s) if s.success());
    if !ok {
        let dir = PathBuf::from(path)
            .parent()
            .map(|p| p.to_path_buf())
            .unwrap_or_else(|| PathBuf::from("."));
        let _ = Command::new("xdg-open")
            .arg(dir)
            .stdin(Stdio::null())
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .spawn();
    }
}

// $XDG_DATA_HOME/shinto/downloads.sqlite, falling back to
// $HOME/.local/share/shinto/downloads.sqlite -- mirrors
// shinto::dataHome()/downloadsDbPath() in app/src/Shinto.h exactly.
fn db_path() -> PathBuf {
    let base = env::var("XDG_DATA_HOME")
        .map(PathBuf::from)
        .unwrap_or_else(|_| {
            let home = env::var("HOME").unwrap_or_else(|_| ".".to_string());
            PathBuf::from(home).join(".local/share")
        });
    base.join("shinto").join("downloads.sqlite")
}

// $XDG_RUNTIME_DIR/shinto.sock, falling back to /tmp/shinto.sock -- mirrors
// shinto::singletonSocketPath() in app/src/Shinto.h exactly. This is the
// one thing shinto-downloads actually needs live IPC with the daemon for:
// cancelling a still-running QWebEngineDownloadRequest can only happen in
// that process, unlike everything else here (reading downloads.sqlite,
// opening a file/folder), which needs nothing from it.
fn singleton_socket_path() -> PathBuf {
    let runtime = env::var("XDG_RUNTIME_DIR").unwrap_or_else(|_| "/tmp".to_string());
    PathBuf::from(runtime).join("shinto.sock")
}

// Best-effort: SingletonServer (app/src/SingletonServer.cpp) replies
// "OK\n" to every recognized command, but there's nothing useful to do
// here if the connection or the reply fails -- the next poll of
// downloads.sqlite (~500ms) reveals whether it actually worked regardless.
fn cancel_download(id: i64) {
    if let Ok(mut stream) = UnixStream::connect(singleton_socket_path()) {
        let _ = stream.write_all(format!("CANCEL_DOWNLOAD {id}\n").as_bytes());
        let mut buf = [0u8; 8];
        let _ = stream.read(&mut buf);
    }
}

#[derive(Clone, Copy)]
enum Action {
    OpenFile,
    OpenFolder,
    CancelDownload,
    Close,
}

// What the action popup offers depends on the download's own state --
// "Cancel download" only makes sense while it's actually running, and
// "Open file" only once it's actually finished (successfully).
fn actions_for(state: State) -> Vec<(&'static str, Action)> {
    match state {
        State::InProgress => vec![
            ("\u{1F4C1} Open containing folder", Action::OpenFolder), // 📁
            ("\u{1F5D1} Cancel download", Action::CancelDownload),    // 🗑
            ("\u{2715} Close", Action::Close),                        // ✕
        ],
        State::Completed => vec![
            ("\u{1F4C4} Open file", Action::OpenFile), // 📄
            ("\u{1F4C1} Open containing folder", Action::OpenFolder),
            ("\u{2715} Close", Action::Close),
        ],
        State::Interrupted | State::Cancelled => vec![
            ("\u{1F4C1} Open containing folder", Action::OpenFolder),
            ("\u{2715} Close", Action::Close),
        ],
    }
}

fn status_glyph_and_color(state: State, palette: &Palette) -> (&'static str, Color) {
    match state {
        State::InProgress => ("\u{2193}", palette.accent), // ↓
        State::Completed => ("\u{2713}", Color::Green),    // ✓
        State::Interrupted => ("\u{2717}", Color::Red),    // ✗
        State::Cancelled => ("-", palette.muted),
    }
}

fn state_label(state: State) -> &'static str {
    match state {
        State::InProgress => "In progress",
        State::Completed => "Completed",
        State::Interrupted => "Failed",
        State::Cancelled => "Cancelled",
    }
}

// Ratatui's Gauge/LineGauge widgets want their own Rect, not an inline
// span sized to fit inside a list row -- a hand-built block-character bar
// is the right tool for a compact per-row meter (same technique used for
// the btop-style aesthetic elsewhere: partial blocks for smooth
// sub-character resolution, not just full/empty blocks).
fn bar(pct: f64, width: usize) -> String {
    const FULL: char = '\u{2588}'; // █
    let filled = ((pct / 100.0) * width as f64).round() as usize;
    let filled = filled.min(width);
    let mut s = String::with_capacity(width);
    for _ in 0..filled {
        s.push(FULL);
    }
    for _ in filled..width {
        s.push('\u{2591}'); // ░
    }
    s
}

fn row_line(index: usize, d: &Download, palette: &Palette) -> ListItem<'static> {
    let (glyph, color) = status_glyph_and_color(d.state, palette);
    let mut spans = vec![
        Span::styled(format!("{glyph} "), Style::default().fg(color)),
        Span::raw(d.filename.clone()),
    ];
    if d.state == State::InProgress {
        let pct = if d.total_bytes > 0 {
            (d.received_bytes as f64 / d.total_bytes as f64) * 100.0
        } else {
            0.0
        };
        spans.push(Span::raw("  "));
        spans.push(Span::styled(bar(pct, 20), Style::default().fg(palette.accent)));
        spans.push(Span::raw(format!(" {:>3}%", pct.round() as i32)));
    } else {
        spans.push(Span::raw("  "));
        spans.push(Span::styled(state_label(d.state), Style::default().fg(color)));
    }
    let _ = index;
    ListItem::new(Line::from(spans))
}

fn centered_rect(width: u16, height: u16, area: Rect) -> Rect {
    let popup_layout = Layout::default()
        .direction(Direction::Vertical)
        .constraints([
            Constraint::Fill(1),
            Constraint::Length(height),
            Constraint::Fill(1),
        ])
        .split(area);
    Layout::default()
        .direction(Direction::Horizontal)
        .constraints([
            Constraint::Fill(1),
            Constraint::Length(width),
            Constraint::Fill(1),
        ])
        .split(popup_layout[1])[1]
}

fn draw(frame: &mut Frame, downloads: &[Download], list_state: &mut ListState, focus: &Focus, palette: &Palette) {
    let area = frame.area();
    let block = Block::default()
        .title(" Shinto Downloads ")
        .title_alignment(Alignment::Center)
        .title_bottom(Line::from(" j/k move  \u{23ce} act  c clear finished  q quit ").centered()) // ⏎
        .borders(Borders::ALL)
        .border_style(Style::default().fg(palette.accent))
        .style(Style::default().bg(palette.bg).fg(palette.fg));

    if downloads.is_empty() {
        let msg = Paragraph::new("No downloads yet.")
            .alignment(Alignment::Center)
            .style(Style::default().fg(palette.muted))
            .block(block);
        frame.render_widget(msg, area);
    } else {
        let items: Vec<ListItem> = downloads
            .iter()
            .enumerate()
            .map(|(i, d)| row_line(i, d, palette))
            .collect();
        let list = List::new(items)
            .block(block)
            .highlight_style(Style::default().add_modifier(Modifier::REVERSED));
        frame.render_stateful_widget(list, area, list_state);
    }

    if let Focus::ActionPopup { target, selected } = focus {
        if let Some(d) = downloads.get(*target) {
            let actions = actions_for(d.state);
            let popup_area = centered_rect(34, actions.len() as u16 + 2, area);
            frame.render_widget(Clear, popup_area);
            let items: Vec<ListItem> = actions
                .iter()
                .enumerate()
                .map(|(i, (label, _))| {
                    let style = if i == *selected {
                        Style::default().add_modifier(Modifier::REVERSED)
                    } else {
                        Style::default()
                    };
                    ListItem::new(Line::from(Span::styled(*label, style)))
                })
                .collect();
            let popup = List::new(items).block(
                Block::default()
                    .title(" Action ")
                    .borders(Borders::ALL)
                    .border_style(Style::default().fg(palette.accent))
                    .style(Style::default().bg(palette.bg).fg(palette.fg)),
            );
            frame.render_widget(popup, popup_area);
        }
    }
}
