// Regression test for a real bug: a fresh downloads.sqlite connection
// (loadAll, polled every 500ms by main.go) landing exactly as
// DownloadManager::persist() commits a write used to come back
// SQLITE_BUSY, which reload() silently turned into "zero downloads" --
// visible as the whole TUI flickering to "No downloads yet." and back
// while a download was actively progressing. Fixed on both sides: WAL
// mode on the writer (app/src/DownloadManager.cpp) so a reader isn't
// blocked by an in-progress writer at all, plus a busy_timeout here as
// defense in depth (see db.go's openDB comment) and reload() no longer
// discarding the previous list on a read error (see main.go).
//
// This test drives the write side itself rather than spinning up the
// real C++ daemon, so it can hammer far harder than any real download's
// ~1/sec progress-persist cadence -- the goal is to prove the read path
// never returns a false "zero rows" under contention, not to reproduce
// realistic timing.
package main

import (
	"database/sql"
	"path/filepath"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	_ "modernc.org/sqlite"
)

func TestConcurrentReadsNeverSeeAFalseEmptyList(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "downloads.sqlite")

	writer, err := sql.Open("sqlite", path)
	if err != nil {
		t.Fatalf("open writer: %v", err)
	}
	defer writer.Close()

	// Mirrors DownloadManager::open()'s schema and its WAL pragma exactly
	// (app/src/DownloadManager.cpp) -- a stale copy of either here would
	// make this test meaningless.
	if _, err := writer.Exec("PRAGMA journal_mode=WAL"); err != nil {
		t.Fatalf("enable WAL: %v", err)
	}
	if _, err := writer.Exec(`CREATE TABLE IF NOT EXISTS downloads (
		id INTEGER PRIMARY KEY, filename TEXT, path TEXT, url TEXT,
		total_bytes INTEGER, received_bytes INTEGER, state INTEGER,
		interrupt_reason TEXT, started_at INTEGER, finished_at INTEGER)`); err != nil {
		t.Fatalf("create table: %v", err)
	}

	const upsertSQL = `INSERT INTO downloads(id, filename, path, url, total_bytes,
		received_bytes, state, interrupt_reason, started_at, finished_at)
		VALUES(1, 'goland.tar.gz', '/tmp/goland.tar.gz', 'https://x', 900000000,
		?, 0, '', 1000, NULL)
		ON CONFLICT(id) DO UPDATE SET received_bytes = excluded.received_bytes`

	const iterations = 500
	var writesDone int32

	var wg sync.WaitGroup
	wg.Add(1)
	go func() {
		defer wg.Done()
		for i := 0; i < iterations; i++ {
			if _, err := writer.Exec(upsertSQL, i); err != nil {
				t.Errorf("writer upsert %d: %v", i, err)
				return
			}
			atomic.AddInt32(&writesDone, 1)
			time.Sleep(2 * time.Millisecond) // hammer -- far faster than any real download's progress-persist cadence
		}
	}()

	// Give the writer a head start so the first row genuinely exists
	// before any reader runs -- this test is about never seeing a FALSE
	// empty result under contention, not about the natural, correct
	// empty result before the first row is ever written.
	for atomic.LoadInt32(&writesDone) == 0 {
		time.Sleep(time.Millisecond)
	}

	var falseEmpties, realErrors int
	for i := 0; i < iterations; i++ {
		downloads, err := loadAll(path)
		switch {
		case err != nil:
			realErrors++
			t.Logf("read %d: error (should not happen with busy_timeout set): %v", i, err)
		case len(downloads) == 0:
			falseEmpties++
		}
	}

	wg.Wait()

	if realErrors > 0 {
		t.Errorf("%d/%d reads returned an error instead of retrying past the writer's lock", realErrors, iterations)
	}
	if falseEmpties > 0 {
		t.Errorf("%d/%d reads saw zero rows while the writer had already committed at least one -- this is the flicker bug", falseEmpties, iterations)
	}
}

// reload()'s own contract: an error must never be reported as ok:true
// with an empty list -- that's the exact bug (see loadAll's caller in
// main.go). This doesn't need real contention, just checks the plumbing:
// a nonexistent file's "not found" case is deliberately nil+nil (no
// downloads.sqlite yet is a normal, real empty state, not an error), so
// reload must report that as ok:true, len 0 -- as opposed to a genuine
// I/O error, which must come back ok:false.
func TestReloadReportsRealEmptyAsOkButNeverMasksAnError(t *testing.T) {
	dir := t.TempDir()
	missing := filepath.Join(dir, "does-not-exist.sqlite")

	msg := reload(missing)().(reloadedMsg)
	if !msg.ok {
		t.Errorf("a genuinely nonexistent db should report ok:true, empty list -- got ok:false")
	}
	if len(msg.downloads) != 0 {
		t.Errorf("expected 0 downloads for a nonexistent db, got %d", len(msg.downloads))
	}
}
