// Must stay in sync with DownloadManager.h's `enum class State` and
// downloads_helper.py's copy of the same ordinals.
var STATE_IN_PROGRESS = 0
var STATE_COMPLETED = 1
var STATE_INTERRUPTED = 2
var STATE_CANCELLED = 3

function stateLabel(state) {
  switch (state) {
    case STATE_IN_PROGRESS: return "In progress"
    case STATE_COMPLETED: return "Completed"
    case STATE_INTERRUPTED: return "Failed"
    case STATE_CANCELLED: return "Cancelled"
    default: return "Unknown"
  }
}

function stateGlyph(state) {
  switch (state) {
    case STATE_IN_PROGRESS: return "●" // ●
    case STATE_COMPLETED: return "✓"   // ✓
    case STATE_INTERRUPTED: return "✗" // ✗
    case STATE_CANCELLED: return "–"   // –
    default: return "?"
  }
}

// Same action-by-state matrix as the old downloads-tui's actions.go
// actionsFor(State), minus its "Close" no-op (a mouse panel's dismiss is
// already always available).
function actionsFor(state) {
  switch (state) {
    case STATE_IN_PROGRESS: return ["reveal", "cancel"]
    case STATE_COMPLETED: return ["open", "reveal"]
    case STATE_INTERRUPTED: return ["reveal"]
    case STATE_CANCELLED: return ["reveal"]
    default: return []
  }
}

function formatBytes(bytes) {
  var value = Number(bytes || 0)
  if (!isFinite(value) || value <= 0) return "0 B"
  var units = ["B", "KB", "MB", "GB", "TB"]
  var index = 0
  while (value >= 1024 && index < units.length - 1) {
    value = value / 1024
    index++
  }
  var decimals = value >= 100 || index === 0 ? 0 : (value >= 10 ? 1 : 2)
  return value.toFixed(decimals).replace(/\.0+$/, "") + " " + units[index]
}

// total_bytes <= 0 means the response had no Content-Length yet (streamed /
// chunked) -- render received-only rather than a bogus percentage.
function progress(row) {
  var total = Number(row.total_bytes || 0)
  var received = Number(row.received_bytes || 0)
  if (total <= 0) {
    return { known: false, fraction: 0, text: formatBytes(received) }
  }
  var fraction = Math.max(0, Math.min(1, received / total))
  return {
    known: true,
    fraction: fraction,
    text: Math.round(fraction * 100) + "%  " + formatBytes(received) + " / " + formatBytes(total)
  }
}

function parseList(raw) {
  var text = String(raw || "").trim()
  if (text === "") return { ok: true, rows: [] }
  try {
    var parsed = JSON.parse(text)
    if (!Array.isArray(parsed)) return { ok: false, rows: [] }
    return { ok: true, rows: parsed }
  } catch (e) {
    return { ok: false, rows: [] }
  }
}

if (typeof module !== "undefined") {
  module.exports = {
    STATE_IN_PROGRESS: STATE_IN_PROGRESS,
    STATE_COMPLETED: STATE_COMPLETED,
    STATE_INTERRUPTED: STATE_INTERRUPTED,
    STATE_CANCELLED: STATE_CANCELLED,
    stateLabel: stateLabel,
    stateGlyph: stateGlyph,
    actionsFor: actionsFor,
    formatBytes: formatBytes,
    progress: progress,
    parseList: parseList
  }
}
