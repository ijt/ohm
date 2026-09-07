#include "Hyprland.h"

#include <QProcess>
#include <QString>

namespace shinto {

void ensureActiveWindowGrouped() {
  // Hyprland 0.56's dispatch surface is Lua (hl.dsp.*). A window's
  // `.group` is nil when ungrouped; togglegroup on a lone window creates
  // a one-window group. hyprctl is a local socket call -- bound so a
  // stuck compositor can't hang the UI. QProcess args, not a shell, so
  // the Lua is one argument as-is.
  QProcess proc;
  proc.start(QStringLiteral("hyprctl"),
             {QStringLiteral("eval"),
              QStringLiteral("local w = hl.get_active_window(); "
                             "if w and not w.group then "
                             "hl.dispatch(hl.dsp.group.toggle()) "
                             "end")});
  if (!proc.waitForFinished(500)) proc.kill();
}

}  // namespace shinto
