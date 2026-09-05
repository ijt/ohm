// Reads downloads.sqlite -- the schema DownloadManager::open() creates in
// app/src/DownloadManager.cpp. Read-only, same as the Rust version this
// file mirrors (downloads-tui/src/db.rs) -- this is a separate, short-lived
// process from the Shinto daemon that's actually writing to the file.
package main

import (
	"database/sql"
	"os"

	_ "modernc.org/sqlite"
)

// Mirrors DownloadManager::State's declaration order in
// app/src/DownloadManager.h exactly -- see the comment on that enum for why
// these ordinals must stay in sync by hand across every language reading
// this file.
type State int

const (
	StateInProgress  State = 0
	StateCompleted   State = 1
	StateInterrupted State = 2
	StateCancelled   State = 3
)

func stateFromInt(v int64) State {
	switch v {
	case 0:
		return StateInProgress
	case 1:
		return StateCompleted
	case 2:
		return StateInterrupted
	default:
		return StateCancelled
	}
}

type Download struct {
	ID            int64
	Filename      string
	Path          string
	TotalBytes    int64
	ReceivedBytes int64
	State         State
}

// Removes every row that isn't still InProgress. A no-op if the file
// doesn't exist yet.
func clearFinished(path string) error {
	if _, err := os.Stat(path); err != nil {
		return nil
	}
	db, err := sql.Open("sqlite", path)
	if err != nil {
		return err
	}
	defer db.Close()
	_, err = db.Exec("DELETE FROM downloads WHERE state != 0")
	return err
}

func loadAll(path string) ([]Download, error) {
	if _, err := os.Stat(path); err != nil {
		return nil, nil
	}
	db, err := sql.Open("sqlite", path)
	if err != nil {
		return nil, err
	}
	defer db.Close()

	rows, err := db.Query(
		"SELECT id, filename, path, total_bytes, received_bytes, state " +
			"FROM downloads ORDER BY started_at DESC")
	if err != nil {
		return nil, err
	}
	defer rows.Close()

	var out []Download
	for rows.Next() {
		var d Download
		var state int64
		if err := rows.Scan(&d.ID, &d.Filename, &d.Path, &d.TotalBytes, &d.ReceivedBytes, &state); err != nil {
			return nil, err
		}
		d.State = stateFromInt(state)
		out = append(out, d)
	}
	return out, rows.Err()
}
