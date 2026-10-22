.pragma library

function pad(n) {
  return n < 10 ? "0" + n : String(n)
}

// Same output as FormatDuration() in data.cpp: "1h 02m 03s", "2m 03s", "12s".
function formatDuration(seconds) {
  var total = Math.max(0, Math.floor(Number(seconds) || 0))
  var h = Math.floor(total / 3600)
  var m = Math.floor((total % 3600) / 60)
  var s = total % 60
  if (h > 0) return h + "h " + pad(m) + "m " + pad(s) + "s"
  if (m > 0) return m + "m " + pad(s) + "s"
  return s + "s"
}

// "1h 02m" / "12m" for compact labels.
function formatShort(seconds) {
  var total = Math.max(0, Math.floor(Number(seconds) || 0))
  var h = Math.floor(total / 3600)
  var m = Math.floor((total % 3600) / 60)
  return h > 0 ? h + "h " + pad(m) + "m" : m + "m"
}

// "1:02" for the bar label.
function formatClock(seconds) {
  var total = Math.max(0, Math.floor(Number(seconds) || 0))
  return Math.floor(total / 3600) + ":" + pad(Math.floor((total % 3600) / 60))
}

function activeIndex(tasks) {
  for (var i = 0; i < tasks.length; i++) {
    if (tasks[i] && tasks[i].active) return i
  }
  return -1
}

// `tt` prints one JSON object per command. Anything else is an error reply.
function parseReply(raw) {
  var text = String(raw || "").trim()
  if (text === "") return { ok: false, error: "tt did not answer" }
  try {
    var reply = JSON.parse(text)
    if (reply && typeof reply === "object") return reply
  } catch (e) {
  }
  return { ok: false, error: "Unexpected output from tt" }
}
