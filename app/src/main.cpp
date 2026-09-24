// CLI entry point and dual-mode dispatch: either hand a request off to an
// already-running Shinto daemon (the common case -- a warm process just
// opens another QMainWindow) or become the daemon. Replaces the bash
// `shinto` script's open_page/ensure_daemon/run_daemon and Chromium's own
// SingletonSocket.
#include <QApplication>
#include <QByteArray>
#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStringList>
#include <QSurfaceFormat>
#include <QWebEngineProfile>

#include <cstdio>
#include <unistd.h>

#include "BrowserWindow.h"
#include "DownloadManager.h"
#include "HistoryStore.h"
#include "PopularDomains.h"
#include "Shinto.h"
#include "SingletonClient.h"
#include "SingletonServer.h"
#include "ThemeLoader.h"
#include "PasskeyBroker.h"
#include "WebProfile.h"

namespace {

QString openCommand(const QString &url) {
  return url.isEmpty() ? QStringLiteral("OPEN") : QStringLiteral("OPEN ") + url;
}

// Source-tree binary is <root>/app/build/shinto-bin and the wrapper is
// <root>/shinto. A packaged binary lives in lib/shinto next to /usr/bin/shinto.
// Only a script (shebang) counts -- never re-exec this ELF.
QString shellWrapperPath() {
  const QString exe = QFileInfo(QStringLiteral("/proc/self/exe")).canonicalFilePath();
  QStringList candidates;
  QDir dir(QFileInfo(exe).absolutePath());
  if (dir.cd(QStringLiteral("../.."))) {
    candidates << dir.filePath(QStringLiteral("shinto"));
  }
  candidates << QDir::homePath() + QStringLiteral("/.local/bin/shinto");
  candidates << QStringLiteral("/usr/bin/shinto");
  for (const QString &candidate : candidates) {
    const QFileInfo info(candidate);
    if (!info.isFile() || !info.isExecutable()) continue;
    const QString canon = info.canonicalFilePath();
    if (canon.isEmpty() || canon == exe) continue;
    QFile file(canon);
    if (!file.open(QIODevice::ReadOnly)) continue;
    if (file.read(2) == "#!") return canon;
  }
  return {};
}

// `shinto-bin uninstall` (and the packaged ELF named shinto) used to hand
// the word to the daemon as a URL. Re-exec the wrapper so the command runs.
// SHINTO_SUBCOMMAND_FORWARD breaks the loop if the wrapper execs us back.
bool forwardShellCommand(char **argv, const QString &cmd) {
  if (qEnvironmentVariableIsSet("SHINTO_SUBCOMMAND_FORWARD")) {
    std::fprintf(stderr, "shinto: '%s' is a command, not a page\n", qUtf8Printable(cmd));
    return false;
  }
  const QString wrapper = shellWrapperPath();
  if (wrapper.isEmpty()) {
    std::fprintf(stderr,
                 "shinto: '%s' is a command, not a page. Run the shinto script.\n",
                 qUtf8Printable(cmd));
    return false;
  }
  qputenv("SHINTO_SUBCOMMAND_FORWARD", "1");
  const QByteArray path = wrapper.toLocal8Bit();
  execv(path.constData(), argv);
  std::fprintf(stderr, "shinto: could not run %s\n", path.constData());
  return false;
}

}  // namespace

int main(int argc, char *argv[]) {
  // Required before any QApplication exists for QtWebEngine to share GL
  // contexts correctly (documented QtWebEngine requirement; see also the
  // matching QSurfaceFormat below, from the same requirement in Qt's own
  // QtWebEngine example apps). Neither of these was the fix for the
  // startup window flicker -- that turned out to be an idle/never-
  // navigated QWebEngineView, fixed in BrowserWindow::enterEmpty() -- but
  // both are still correct baseline setup for a QtWebEngine app.
  QApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
  {
    QSurfaceFormat format;
    format.setDepthBufferSize(24);
    format.setStencilBufferSize(8);
    QSurfaceFormat::setDefaultFormat(format);
  }

  // Must be set before QtWebEngine's Chromium backend initializes, so this
  // has to happen before anything else touches it.
  {
    QByteArray flags = qgetenv("QTWEBENGINE_CHROMIUM_FLAGS");
    if (!flags.isEmpty()) flags += ' ';
    // Thin, auto-hiding scrollbars (shown only while scrolling), matching
    // stock Chrome's own default look -- Chromium's own feature, not a
    // hand-rolled CSS/JS hack. (An earlier "leftover scrollbar after
    // Ctrl+N" report survived this flag being removed entirely, so it was
    // never actually this feature's fault -- just the plain default
    // scrollbar being visible, which is normal.)
    flags += "--enable-features=OverlayScrollbar ";
    // This machine's Wayland/DRM GBM+EGL native-buffer path for GPU
    // *compositing* hits a hard Chromium-side failure (gbm_bo_import
    // returning nullptr, EGL_BAD_MATCH, then a fatal abort) -- reproduced
    // on every run with real (non-offscreen) rendering.
    // --disable-gpu-compositing avoids that crash. Full --disable-gpu was
    // tried too (also avoids it, plus was thought at the time to fix a
    // startup close+reopen flicker) but that flicker turned out to be
    // caused by something else entirely (an idle QWebEngineView, fixed in
    // BrowserWindow::enterEmpty() via about:blank) -- so --disable-gpu was
    // never actually necessary, and it's the heavier hammer: it also
    // disables hardware video decode, which made video-heavy sites (e.g.
    // YouTube) fall back to CPU-only software decoding, sluggish enough to
    // look like a hang. --disable-gpu-compositing alone leaves video
    // decode acceleration intact while still avoiding the compositing
    // crash.
    flags += "--disable-gpu-compositing";
    qputenv("QTWEBENGINE_CHROMIUM_FLAGS", flags);
  }
  // Silence a harmless xdg-desktop-portal warning
  // ("qt.qpa.services: Failed to register with host portal ... Connection
  // already associated with an application ID") that fires here but not
  // anything we can act on -- it doesn't affect behavior.
  {
    QByteArray rules = qgetenv("QT_LOGGING_RULES");
    if (!rules.isEmpty()) rules += ';';
    rules += "qt.qpa.services=false";
    qputenv("QT_LOGGING_RULES", rules);
  }

  QStringList args;
  for (int i = 1; i < argc; ++i) {
    args << QString::fromLocal8Bit(argv[i]);
  }

  // omarchy-launch-browser probes `$browser_exec --help | grep -q MOZ_LOG`
  // for every default-browser launch (Super+Shift+B), to decide whether to
  // pass --private-window (Firefox-family) or --incognito. Without this,
  // --help had no special handling and fell through to being treated as a
  // URL to open -- so every such launch silently asked the running daemon
  // to open a real window navigated to the literal string "--help", which
  // QtWebEngine renders as blank white (reproduced concretely: Super+Shift+B
  // showed nothing but blank white, while Super+Shift+Return -- which
  // launches shinto directly, no probe -- worked fine). Print and exit
  // before touching the daemon at all; stdout (not qInfo/qWarning, which
  // this Qt build routes to the journal by default, invisible to a pipe)
  // since the probe pipes it straight into grep.
  if (args.contains(QStringLiteral("--help")) || args.contains(QStringLiteral("-h"))) {
    std::fputs("Usage: shinto [url]\n"
               "Page viewer for Omarchy -- one window, one page.\n",
               stdout);
    return 0;
  }
  if (args.contains(QStringLiteral("--version"))) {
    std::fprintf(stdout, "shinto %s\n", SHINTO_VERSION);
    return 0;
  }

  // omarchy-launch-browser maps --private to --incognito/--inprivate and
  // appends them before the URL. Shinto has no private profile yet, so drop
  // the flags rather than treating them as a URL to open.
  args.removeAll(QStringLiteral("--incognito"));
  args.removeAll(QStringLiteral("--private"));
  args.removeAll(QStringLiteral("--inprivate"));

  // omarchy-launch-webapp runs the browser as `--app=URL` (Chromium's app
  // mode). Every Shinto window is already chrome-less, so an app is just
  // a page: take the URL and open it like any other.
  for (int i = 0; i < args.size(); ++i) {
    if (args.at(i).startsWith(QLatin1String("--app="))) {
      args[i] = args.at(i).mid(int(sizeof("--app=")) - 1);
    } else if (args.at(i) == QLatin1String("--app") && i + 1 < args.size()) {
      args.removeAt(i);
    }
  }

  if (args.removeOne(QStringLiteral("--theme"))) {
    // `shinto theme` (the Omarchy theme-set hook): tell an already-running
    // daemon to re-read colors.toml and re-apply it live. A no-op if
    // nothing's listening -- there's no daemon to theme.
    QCoreApplication probe(argc, argv);
    shinto::SingletonClient::tryHandoff(QStringLiteral("THEME"));
    return 0;
  }

  if (const int i = args.indexOf(QStringLiteral("--command")); i >= 0) {
    // The shortcuts panel's command palette: run a named command (see
    // BrowserWindow::runCommand) in the daemon's last-focused window.
    if (i + 1 >= args.size()) {
      std::fputs("shinto: --command needs a command name\n", stderr);
      return 2;
    }
    QCoreApplication probe(argc, argv);
    return shinto::SingletonClient::tryHandoff(QStringLiteral("COMMAND ") + args.at(i + 1)) ? 0 : 1;
  }

  const bool forceDaemon = args.removeOne(QStringLiteral("--daemon"));
  const QString url = args.isEmpty() ? QString() : args.first();

  if (!forceDaemon && shinto::isShellCommand(url)) {
    if (!forwardShellCommand(argv, url)) return 2;
  }

  if (!forceDaemon) {
    // Cheap path: a plain QCoreApplication is enough to drive the local
    // socket handoff, so a `shinto <url>` invocation against an already
    // warm daemon never touches the GUI/Wayland platform plugin at all.
    bool handedOff = false;
    {
      QCoreApplication probe(argc, argv);
      handedOff = shinto::SingletonClient::tryHandoff(openCommand(url));
    }
    if (handedOff) {
      return 0;
    }
  }

  // No daemon answered (or --daemon forces this unconditionally): this
  // process becomes the daemon.
  shinto::PasskeyBroker::registerScheme();  // Qt: before the QApplication.
  QApplication app(argc, argv);
  app.setApplicationName(QString::fromLatin1(shinto::kAppId));
  app.setDesktopFileName(QString::fromLatin1(shinto::kAppId));
  // The whole point of dropping the old hidden spare-window trick: a warm
  // daemon with zero windows open is simply a QApplication that doesn't
  // quit when the last window closes.
  app.setQuitOnLastWindowClosed(false);

  // Constructed before the profile: createSharedProfile() wires the
  // profile's downloadRequested signal straight to downloads.track().
  shinto::DownloadManager downloads;
  if (!downloads.open()) {
    qWarning() << "shinto: continuing without persistent download history";
  }
  QWebEngineProfile *profile = shinto::createSharedProfile(&app, &downloads);

  shinto::HistoryStore history;
  if (!history.open()) {
    qWarning() << "shinto: continuing without persistent typed/visited history";
  }
  shinto::PopularDomains domains;

  shinto::BrowserWindow::applyPaletteToAll(shinto::loadPalette());

  shinto::SingletonServer server;
  QObject::connect(&server, &shinto::SingletonServer::openRequested,
                    [profile, &history, &domains, &downloads](const QString &openUrl) {
                      shinto::BrowserWindow::spawn(profile, &history, &domains, &downloads, openUrl);
                    });
  QObject::connect(&server, &shinto::SingletonServer::themeReloadRequested,
                    [] { shinto::BrowserWindow::applyPaletteToAll(shinto::loadPalette()); });
  QObject::connect(&server, &shinto::SingletonServer::commandRequested,
                    &shinto::BrowserWindow::runCommand);
  QObject::connect(&server, &shinto::SingletonServer::cancelDownloadRequested, &downloads,
                    &shinto::DownloadManager::cancel);

  if (!server.listen()) {
    // Lost a race with another process becoming the daemon in the tiny
    // window since our own handoff attempt failed above.
    if (shinto::SingletonClient::tryHandoff(openCommand(url))) {
      return 0;
    }
    qWarning() << "shinto: could not become the daemon and handoff failed";
    return 1;
  }

  if (!forceDaemon) {
    shinto::BrowserWindow::spawn(profile, &history, &domains, &downloads, url);
  }
  // else: `--daemon` (the systemd unit) starts with zero windows, warm and
  // waiting for the first Super+Shift+Return handoff.

  return app.exec();
}
