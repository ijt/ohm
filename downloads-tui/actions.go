// Shell-out actions (open file/folder, cancel a live download) plus the
// two path helpers shared across them -- mirrors downloads-tui/src/main.rs'
// bottom half exactly (db_path/singleton_socket_path/open_file/open_folder/
// cancel_download).
package main

import (
	"fmt"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"time"
)

// $XDG_DATA_HOME/shinto/downloads.sqlite, falling back to
// $HOME/.local/share/shinto/downloads.sqlite -- mirrors
// shinto::dataHome()/downloadsDbPath() in app/src/Shinto.h exactly.
func dbPath() string {
	base := os.Getenv("XDG_DATA_HOME")
	if base == "" {
		home, err := os.UserHomeDir()
		if err != nil {
			home = "."
		}
		base = filepath.Join(home, ".local/share")
	}
	return filepath.Join(base, "shinto", "downloads.sqlite")
}

// $XDG_RUNTIME_DIR/shinto.sock, falling back to /tmp/shinto.sock -- mirrors
// shinto::singletonSocketPath() in app/src/Shinto.h exactly.
func singletonSocketPath() string {
	runtime := os.Getenv("XDG_RUNTIME_DIR")
	if runtime == "" {
		runtime = "/tmp"
	}
	return filepath.Join(runtime, "shinto.sock")
}

// Every child process launched from here has its stdio nulled -- see
// downloads-tui/src/main.rs's comment on open_file for why (log spam from
// an inherited stdout/stderr corrupts a raw-mode terminal).
func silentCommand(name string, args ...string) *exec.Cmd {
	cmd := exec.Command(name, args...)
	cmd.Stdin = nil
	cmd.Stdout = nil
	cmd.Stderr = nil
	return cmd
}

func openFile(path string) {
	_ = silentCommand("xdg-open", path).Start()
}

// org.freedesktop.FileManager1.ShowItems opens the folder with `path`
// highlighted; falls back to a plain (unselected) folder-open via
// xdg-open if nothing answers.
func openFolder(path string) {
	uri := "file://" + path
	cmd := silentCommand("dbus-send",
		"--session", "--print-reply",
		"--dest=org.freedesktop.FileManager1",
		"/org/freedesktop/FileManager1",
		"org.freedesktop.FileManager1.ShowItems",
		"array:string:"+uri, "string:")
	if err := cmd.Run(); err != nil {
		dir := filepath.Dir(path)
		_ = silentCommand("xdg-open", dir).Start()
	}
}

// Best-effort: SingletonServer (app/src/SingletonServer.cpp) replies
// "OK\n" to every recognized command, but there's nothing useful to do
// here if the connection or the reply fails -- the next DB poll reveals
// whether it actually worked regardless.
func cancelDownload(id int64) {
	conn, err := net.DialTimeout("unix", singletonSocketPath(), 500*time.Millisecond)
	if err != nil {
		return
	}
	defer conn.Close()
	_, _ = conn.Write([]byte(fmt.Sprintf("CANCEL_DOWNLOAD %d\n", id)))
	buf := make([]byte, 8)
	_ = conn.SetReadDeadline(time.Now().Add(500 * time.Millisecond))
	_, _ = conn.Read(buf)
}
