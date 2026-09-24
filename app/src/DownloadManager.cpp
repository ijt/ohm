#include "DownloadManager.h"

#include <algorithm>
#include <memory>

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTimer>
#include <QWebEngineDownloadRequest>

#include "DownloadsPanelLauncher.h"
#include "Notify.h"
#include "Shinto.h"

namespace shinto {

namespace {

// Chromium's own DownloadInterruptReason taxonomy mixes transient
// network/server hiccups in with things retrying can't fix (bad disk,
// blocked file, user cancellation). Reproduced concretely: a real
// multi-hundred-MB download from a CDN that curl pulls without a hitch
// (400MB straight through at a steady 5.8MB/s) still gets killed by
// QtWebEngine's network stack with NetworkFailed every 30-90 seconds on
// this connection -- not this specific site's fault, not GPU/QUIC flags
// (confirmed unaffected by --disable-quic), just a flaky link Chromium's
// downloader is happy to resume if asked. Only the reasons below are worth
// retrying; anything else (a real disk problem, a rejected/blocked file,
// the user hitting cancel) would just fail again identically.
bool isRetryableInterrupt(QWebEngineDownloadRequest::DownloadInterruptReason reason) {
  switch (reason) {
    case QWebEngineDownloadRequest::NetworkFailed:
    case QWebEngineDownloadRequest::NetworkTimeout:
    case QWebEngineDownloadRequest::NetworkDisconnected:
    case QWebEngineDownloadRequest::NetworkServerDown:
    case QWebEngineDownloadRequest::ServerFailed:
    case QWebEngineDownloadRequest::ServerUnreachable:
      return true;
    default:
      return false;
  }
}

DownloadManager::DownloadRecord rowFromQuery(const QSqlQuery &q) {
  DownloadManager::DownloadRecord r;
  r.id = q.value(0).toInt();
  r.filename = q.value(1).toString();
  r.path = q.value(2).toString();
  r.url = q.value(3).toString();
  r.totalBytes = q.value(4).toLongLong();
  r.receivedBytes = q.value(5).toLongLong();
  r.state = static_cast<DownloadManager::State>(q.value(6).toInt());
  r.interruptReason = q.value(7).toString();
  r.startedAt = q.value(8).toLongLong();
  r.finishedAt = q.value(9).toLongLong();
  return r;
}

// QStringLiteral needs an actual literal token at its call site (it does a
// compile-time UTF-16 conversion trick), not a variable -- QLatin1String
// converts to QString implicitly and has no such restriction, so this can
// still be shared between open() and refreshFromDisk() as one constant.
const QLatin1String kSelectAllSql(
    "SELECT id, filename, path, url, total_bytes, received_bytes, state,"
    " interrupt_reason, started_at, finished_at FROM downloads ORDER BY started_at DESC");

}  // namespace

DownloadManager::DownloadManager(QObject *parent) : QObject(parent) {}

bool DownloadManager::open() {
  db_ = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("shinto_downloads"));
  db_.setDatabaseName(downloadsDbPath());
  if (!db_.open()) {
    qWarning() << "shinto: could not open downloads db:" << db_.lastError().text();
    return false;
  }
  QSqlQuery q(db_);
  // WAL instead of the default rollback-journal mode: the downloads panel's
  // helper script (downloads-panel/downloads_helper.py, a separate process)
  // opens a fresh read connection to this same file roughly twice a second.
  // Under the default mode, a writer holds an exclusive lock for the
  // (brief) duration of each write, and a reader that lands in that window
  // gets SQLITE_BUSY immediately -- confirmed as the cause of a real
  // "flickers between the list and 'No downloads yet'" bug in the old
  // downloads-tui Go TUI this replaced, since its read path silently
  // treated that error as "zero rows". WAL lets readers proceed against
  // the last-committed snapshot instead of blocking on an in-progress
  // writer, which is exactly the read/write mix here (this process is the
  // only writer; the panel's helper is a read-mostly,
  // occasional-write-via-socket-not-SQL guest).
  q.exec(QStringLiteral("PRAGMA journal_mode=WAL"));
  q.exec(QStringLiteral(
      "CREATE TABLE IF NOT EXISTS downloads ("
      " id INTEGER PRIMARY KEY, filename TEXT, path TEXT, url TEXT,"
      " total_bytes INTEGER, received_bytes INTEGER, state INTEGER,"
      " interrupt_reason TEXT, started_at INTEGER, finished_at INTEGER)"));

  q.exec(kSelectAllSql);
  while (q.next()) {
    DownloadRecord r = rowFromQuery(q);
    // A row still InProgress from a previous daemon run can never actually
    // resume -- its real QWebEngineDownloadRequest died along with that
    // process. Fixed up in-memory here; the one bulk UPDATE below persists
    // it (avoids mutating the table mid-SELECT via a second statement on
    // the same connection).
    if (r.state == State::InProgress) {
      r.state = State::Interrupted;
      r.interruptReason = QStringLiteral("Shinto restarted before this finished");
      r.finishedAt = QDateTime::currentSecsSinceEpoch();
    }
    records_.push_back(r);
  }

  QSqlQuery fix(db_);
  fix.prepare(QStringLiteral(
      "UPDATE downloads SET state = :interrupted, interrupt_reason = :reason,"
      " finished_at = :now WHERE state = :inprogress"));
  fix.bindValue(":interrupted", static_cast<int>(State::Interrupted));
  fix.bindValue(":reason", QStringLiteral("Shinto restarted before this finished"));
  fix.bindValue(":now", QDateTime::currentSecsSinceEpoch());
  fix.bindValue(":inprogress", static_cast<int>(State::InProgress));
  if (!fix.exec()) {
    qWarning() << "shinto: could not mark stale downloads interrupted:" << fix.lastError().text();
  }
  return true;
}

void DownloadManager::cancel(int id) {
  const auto it = live_.constFind(id);
  if (it == live_.constEnd() || it.value().isNull()) return;
  // Triggers the DownloadCancelled branch in track()'s stateChanged
  // handler above (persist, live_ cleanup, file cleanup, signal) -- cancel
  // has exactly one thing to do: ask Chromium to stop, then let the
  // existing state machine handle the rest the same way it already does
  // for a completion or failure.
  it.value()->cancel();
}

void DownloadManager::refreshFromDisk() {
  QSqlQuery q(db_);
  q.exec(kSelectAllSql);
  QVector<DownloadRecord> fresh;
  while (q.next()) {
    fresh.push_back(rowFromQuery(q));
  }
  records_ = fresh;
}

void DownloadManager::persist(const DownloadRecord &record) {
  bool found = false;
  for (auto &r : records_) {
    if (r.id == record.id) {
      r = record;
      found = true;
      break;
    }
  }
  if (!found) records_.push_back(record);

  QSqlQuery query(db_);
  query.prepare(QStringLiteral(
      "INSERT INTO downloads(id, filename, path, url, total_bytes, received_bytes,"
      " state, interrupt_reason, started_at, finished_at)"
      " VALUES(:id, :filename, :path, :url, :total, :received, :state, :reason, :started, :finished)"
      " ON CONFLICT(id) DO UPDATE SET"
      "  filename = excluded.filename, path = excluded.path, url = excluded.url,"
      "  total_bytes = excluded.total_bytes, received_bytes = excluded.received_bytes,"
      "  state = excluded.state, interrupt_reason = excluded.interrupt_reason,"
      "  started_at = excluded.started_at, finished_at = excluded.finished_at"));
  query.bindValue(":id", record.id);
  query.bindValue(":filename", record.filename);
  query.bindValue(":path", record.path);
  query.bindValue(":url", record.url);
  query.bindValue(":total", record.totalBytes);
  query.bindValue(":received", record.receivedBytes);
  query.bindValue(":state", static_cast<int>(record.state));
  query.bindValue(":reason", record.interruptReason);
  query.bindValue(":started", record.startedAt);
  query.bindValue(":finished", record.finishedAt);
  if (!query.exec()) {
    qWarning() << "shinto: DownloadManager persist failed:" << query.lastError().text();
  }
}

DownloadManager::DownloadRecord DownloadManager::recordFor(int id) const {
  for (const auto &r : records_) {
    if (r.id == id) return r;
  }
  return DownloadRecord{};
}

void DownloadManager::track(QWebEngineDownloadRequest *download) {
  if (!download) return;

  const QString dir = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
  QDir().mkpath(dir);
  download->setDownloadDirectory(dir);

  // QWebEngineDownloadRequest doesn't dedupe filenames the way Chromium's
  // own save-as dialog would -- a second download of the same name would
  // silently clobber the first one without this.
  QString name = download->suggestedFileName();
  if (name.isEmpty()) name = QStringLiteral("download");
  const QFileInfo info(name);
  const QString base = info.completeBaseName();
  const QString ext = info.suffix();
  QString candidate = name;
  for (int n = 1; QFile::exists(dir + QLatin1Char('/') + candidate); ++n) {
    candidate = ext.isEmpty() ? QStringLiteral("%1 (%2)").arg(base).arg(n)
                               : QStringLiteral("%1 (%2).%3").arg(base).arg(n).arg(ext);
  }
  download->setDownloadFileName(candidate);
  download->accept();

  const int id = static_cast<int>(download->id());
  DownloadRecord record;
  record.id = id;
  record.filename = candidate;
  record.path = dir + QLatin1Char('/') + candidate;
  record.url = download->url().toString();
  record.totalBytes = download->totalBytes();
  record.state = State::InProgress;
  record.startedAt = QDateTime::currentSecsSinceEpoch();
  persist(record);
  live_[id] = download;
  emit downloadAdded(id);

  // Emitted signal throttled to ~4/sec -- receivedBytesChanged fires far
  // more often than any in-process UI needs to redraw. The SQLite write is
  // throttled separately and much more coarsely (~1/sec): the downloads
  // panel (a whole separate process, unlike an in-process signal listener)
  // has no way to see progress *except* through this file, so it can't be
  // skipped the way the original "in-memory only" version did here --
  // confirmed concretely with the old downloads-tui Go TUI this replaced:
  // its bar sat frozen at its initial 0% until the download finished,
  // since nothing ever wrote a byte count to disk before that.
  auto lastEmitMs = std::make_shared<qint64>(0);
  auto lastPersistMs = std::make_shared<qint64>(0);
  connect(download, &QWebEngineDownloadRequest::receivedBytesChanged, this,
          [this, download, id, lastEmitMs, lastPersistMs]() {
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            if (now - *lastEmitMs < 250) return;
            *lastEmitMs = now;
            DownloadRecord r = recordFor(id);
            if (r.id == 0) return;  // already finished and possibly re-tracked elsewhere
            r.receivedBytes = download->receivedBytes();
            r.totalBytes = download->totalBytes();
            for (auto &cached : records_) {
              if (cached.id == id) {
                cached.receivedBytes = r.receivedBytes;
                cached.totalBytes = r.totalBytes;
                break;
              }
            }
            if (now - *lastPersistMs >= 1000) {
              *lastPersistMs = now;
              persist(r);
            }
            emit downloadProgress(id, r.receivedBytes, r.totalBytes);
          });

  // Retries silently (no notification) up to this many times before
  // reporting failure -- observed concretely needing more than one or two
  // on this network for a ~1.2GB file, since each interruption costs
  // nothing but a resume() (the CDN supports Range requests, so this
  // continues rather than restarting from scratch), not a full redownload.
  auto retriesLeft = std::make_shared<int>(8);
  connect(
      download, &QWebEngineDownloadRequest::stateChanged, this,
      [this, download, candidate, id, retriesLeft](QWebEngineDownloadRequest::DownloadState state) {
        switch (state) {
          case QWebEngineDownloadRequest::DownloadCompleted: {
            notifyClickable(QStringLiteral("Download complete"), candidate, [] { showDownloadsPanel(); });
            DownloadRecord r = recordFor(id);
            r.state = State::Completed;
            r.receivedBytes = download->receivedBytes();
            r.finishedAt = QDateTime::currentSecsSinceEpoch();
            persist(r);
            live_.remove(id);
            emit downloadStateChanged(id, State::Completed);
            break;
          }
          case QWebEngineDownloadRequest::DownloadInterrupted: {
            const auto reason = download->interruptReason();
            if (isRetryableInterrupt(reason) && *retriesLeft > 0) {
              --*retriesLeft;
              // A moment's grace before resuming rather than hammering the
              // connection back immediately.
              QPointer<QWebEngineDownloadRequest> guarded(download);
              QTimer::singleShot(1000, [guarded]() {
                if (guarded) guarded->resume();
              });
              break;
            }
            notify(QStringLiteral("Download failed"),
                   candidate + QStringLiteral(": ") + download->interruptReasonString(),
                   /*critical=*/true);
            DownloadRecord r = recordFor(id);
            r.state = State::Interrupted;
            r.interruptReason = download->interruptReasonString();
            r.finishedAt = QDateTime::currentSecsSinceEpoch();
            persist(r);
            live_.remove(id);
            emit downloadStateChanged(id, State::Interrupted);
            break;
          }
          case QWebEngineDownloadRequest::DownloadCancelled: {
            DownloadRecord r = recordFor(id);
            r.state = State::Cancelled;
            r.finishedAt = QDateTime::currentSecsSinceEpoch();
            persist(r);
            live_.remove(id);
            // Chromium's own cancel handling should already delete its
            // partial file, but that's not guaranteed across every
            // QtWebEngine version/interruption path -- a stray partial
            // file left behind after the user explicitly asked to cancel
            // is exactly the kind of thing worth being defensive about.
            // Both the final name and Chromium's own in-flight ".download"
            // suffix are tried; QFile::remove() on a nonexistent path is
            // just a harmless no-op.
            QFile::remove(r.path);
            QFile::remove(r.path + QStringLiteral(".download"));
            emit downloadStateChanged(id, State::Cancelled);
            break;
          }
          default:
            break;
        }
      });
}

QVector<DownloadManager::DownloadRecord> DownloadManager::all() const {
  QVector<DownloadRecord> out = records_;
  std::sort(out.begin(), out.end(), [](const DownloadRecord &a, const DownloadRecord &b) {
    return a.startedAt > b.startedAt;
  });
  return out;
}

bool DownloadManager::hasActive() const {
  for (const auto &r : records_) {
    if (r.state == State::InProgress) return true;
  }
  return false;
}

DownloadManager::DownloadRecord DownloadManager::latestActive() const {
  DownloadRecord best;
  bool found = false;
  for (const auto &r : records_) {
    if (r.state != State::InProgress) continue;
    if (!found || r.startedAt > best.startedAt) {
      best = r;
      found = true;
    }
  }
  return best;
}

}  // namespace shinto
