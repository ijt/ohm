// CLI-side half of the singleton handoff -- replaces
// control-server.py's /open endpoint plus bash's ensure_daemon/
// wait_for_singleton dance and Chromium's own SingletonSocket race.
#pragma once

#include <QString>

namespace ohm {

class SingletonClient {
 public:
  // Tries to hand `commandLine` (e.g. "OPEN https://example.com", "OPEN",
  // "THEME") off to an already-running daemon. Returns true if a daemon
  // accepted the connection and the command was written -- callers should
  // treat false as "no daemon is running, become the daemon."
  static bool tryHandoff(const QString &commandLine, int timeoutMs = 200);
};

}  // namespace ohm
