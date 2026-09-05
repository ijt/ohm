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

// A fresh connection every poll (see openDB's callers) landing on
// SQLITE_BUSY -- the daemon holds a brief write lock while persisting a
// progress update -- used to come back as a plain error, which loadAll
// treated as "zero rows" and reload() then discarded entirely (see
// main.go), flashing "No downloads yet." over a real, populated list.
// DownloadManager.cpp now opens the file in WAL mode, which mostly
// avoids this (a WAL reader isn't blocked by a writer at all), but this
// busy_timeout is real defense in depth, not just belt-and-suspenders --
// modernc.org/sqlite's own busy_timeout still applies to WAL's brief
// writer-vs-writer/checkpoint locks, and this is a single-file constant
// so there's no real cost to keeping it.
const dsnSuffix = "?_busy_timeout=2000"

func openDB(path string) (*sql.DB, error) {
	return sql.Open("sqlite", path+dsnSuffix)
}

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
	db, err := openDB(path)
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
	db, err := openDB(path)
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
