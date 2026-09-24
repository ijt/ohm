// Builds the single QWebEngineProfile shared by every BrowserWindow, so
// cookies/localStorage/session are shared across windows the same way one
// Chromium --user-data-dir was shared across --app windows before.
#pragma once

class QObject;
class QWebEngineProfile;

namespace ohm {

class DownloadManager;

// Ownership: the returned profile is a child of `parent` (typically the
// QApplication or another long-lived owner). Do not call this more than once
// per process — QtWebEngine expects one persistent profile instance.
// `downloads` (not owned) receives every QWebEngineProfile::downloadRequested
// -- see DownloadManager::track().
QWebEngineProfile *createSharedProfile(QObject *parent, DownloadManager *downloads);

}  // namespace ohm
