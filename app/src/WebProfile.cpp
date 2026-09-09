#include "WebProfile.h"

#include <QDir>
#include <QWebEngineDownloadRequest>
#include <QWebEngineProfile>
#include <QWebEngineScript>
#include <QWebEngineScriptCollection>
#include <QWebEngineSettings>

#include "DownloadManager.h"
#include "Shinto.h"

namespace shinto {

namespace {

// Scrollbars stay zero-width. Chromium's OverlayScrollbar (see main.cpp)
// is supposed to overlay a bar on top of the content while scrolling, but
// produces no visible effect in this build -- and any ::-webkit-scrollbar
// rule disables that overlay path anyway, forcing classic layout-taking
// bars. An earlier version revealed the bar by toggling width 0px -> 8px
// on scroll; that reflow can itself fire another scroll (Shopify admin
// nested panels do this constantly), so the class flapped every 700ms and
// the page jiggled in place. Wheel/trackpad/keyboard still scroll.
//
// Runs at DocumentReady, not DocumentCreation (Chromium's document_start):
// an earlier attempt injected there with a documentElement-or-document
// fallback, and at that very early point the parser hasn't necessarily
// created <html>/<head> yet -- appending to `document` itself can insert
// our <style> as the document's root element ahead of the real one,
// corrupting the rest of parsing and blanking the page (reproduced on a
// real navigation). DocumentReady runs after the DOM is fully parsed, so
// document.head is guaranteed to exist for any real HTML page; non-HTML
// documents are simply skipped rather than guessed at.
void installScrollbarHidingScript(QWebEngineProfile *profile) {
  QWebEngineScript script;
  script.setName(QStringLiteral("shinto-hide-scrollbars"));
  script.setInjectionPoint(QWebEngineScript::DocumentReady);
  script.setWorldId(QWebEngineScript::MainWorld);
  script.setRunsOnSubFrames(true);
  script.setSourceCode(QStringLiteral(R"JS(
(function() {
  if (!document.head) return;
  var style = document.createElement('style');
  style.textContent = '::-webkit-scrollbar{width:0px;height:0px;background:transparent;}';
  document.head.appendChild(style);
})();
)JS"));
  profile->scripts()->insert(script);
}

// QtWebEngine's WebAuthn support is incomplete in a way that hangs pages,
// not just lacks features: PublicKeyCredential.getClientCapabilities()
// (and the older isUserVerifyingPlatformAuthenticatorAvailable() /
// isConditionalMediationAvailable() feature-detection calls) never settle
// -- the promise neither resolves nor rejects, ever -- confirmed as a
// standing QtWebEngine issue via another QtWebEngine-based browser
// (qutebrowser #8930), not something specific to one site. A page that
// awaits one of these before deciding whether to offer passkey/QR-code UI
// or fall back to a password is left permanently stuck (reproduced
// concretely: "Continue with Apple" on x.com hangs on a spinner forever
// after submitting an email, and never offers its usual "sign in via
// iPhone" option either -- both are this).
//
// Races each real call against a timeout rather than unconditionally
// stubbing them out: if a native call actually resolves (fixed in a
// future QtWebEngine, or a real platform authenticator answers), that
// real answer wins; only a call that's actually hung falls back to "no
// WebAuthn here" once the timeout elapses. This can't restore the
// QR-code/hybrid-transport flow itself -- Qt's own docs for
// QWebEngineWebAuthUxRequest list only user verification, resident
// credentials, and failure UX as supported, nothing hybrid-transport-
// shaped -- but it stops the hang, letting a site fall through to
// password auth on its own instead of freezing.
//
// Unlike the scrollbar script above, this runs at DocumentCreation
// (Chromium's document_start), not DocumentReady: it never touches the
// DOM (only static methods on the PublicKeyCredential global, which
// exists before any script runs regardless of parse state), so the
// DocumentReady-only reasoning above doesn't apply -- and it has to be in
// place before the page's own feature-detection call fires, which an
// early <head> inline script or auth SDK may do well before
// DocumentReady.
void installWebAuthnCapabilityShim(QWebEngineProfile *profile) {
  QWebEngineScript script;
  script.setName(QStringLiteral("shinto-webauthn-capability-shim"));
  script.setInjectionPoint(QWebEngineScript::DocumentCreation);
  script.setWorldId(QWebEngineScript::MainWorld);
  script.setRunsOnSubFrames(true);
  script.setSourceCode(QStringLiteral(R"JS(
(function() {
  if (typeof PublicKeyCredential === 'undefined') return;
  var TIMEOUT_MS = 2500; // generous: "hangs forever" -> "waits 2.5s" is
                          // still a huge win, and keeps the window for a
                          // false early answer narrow.

  function shim(name, fallbackValue) {
    // These are STATIC methods on the class itself per the WebAuthn
    // spec, not prototype methods.
    if (typeof PublicKeyCredential[name] !== 'function') return; // absent on this build
    var real = PublicKeyCredential[name].bind(PublicKeyCredential);
    function patched() {
      var args = arguments;
      return new Promise(function(resolve) {
        var settled = false;
        var timer = setTimeout(function() {
          if (settled) return;
          settled = true;
          resolve(fallbackValue);
        }, TIMEOUT_MS);
        var realPromise;
        try { realPromise = real.apply(PublicKeyCredential, args); }
        catch (e) {
          if (!settled) { settled = true; clearTimeout(timer); resolve(fallbackValue); }
          return;
        }
        realPromise.then(function(v) {
          if (settled) return;
          settled = true; clearTimeout(timer); resolve(v);
        }, function() {
          if (settled) return;
          settled = true; clearTimeout(timer); resolve(fallbackValue);
        });
      });
    }
    try {
      Object.defineProperty(PublicKeyCredential, name,
        { value: patched, writable: true, configurable: true, enumerable: false });
    } catch (e) { /* non-configurable on this build: leave native behavior alone */ }
  }

  shim('getClientCapabilities', {
    conditionalCreate: false, conditionalGet: false, hybridTransport: false,
    passkeyPlatformAuthenticator: false, userVerifyingPlatformAuthenticator: false,
    relatedOrigins: false, signalAllAcceptedCredentials: false,
    signalCurrentUserDetails: false, signalUnknownCredential: false
  });
  shim('isUserVerifyingPlatformAuthenticatorAvailable', false);
  shim('isConditionalMediationAvailable', false);
})();
)JS"));
  profile->scripts()->insert(script);
}

// Nothing in Shinto connected to QWebEngineProfile::downloadRequested, and
// an unhandled download just sits forever in the DownloadRequested state --
// nothing is written to disk, and there's no error either, so a user
// clicking a download link (confirmed concretely: a .dmg/.exe/.tar.gz from
// jetbrains.com) sees literally nothing happen and has no way to tell
// whether it worked. All the actual accept/dedupe/retry/tracking logic
// lives in DownloadManager::track() now (it needs to persist state and
// drive the downloads-list UI, not just this one profile-level signal);
// this is just the wiring between the two.
void installDownloadHandler(QWebEngineProfile *profile, DownloadManager *downloads) {
  QObject::connect(profile, &QWebEngineProfile::downloadRequested, downloads,
                    [downloads](QWebEngineDownloadRequest *download) { downloads->track(download); });
}

}  // namespace

QWebEngineProfile *createSharedProfile(QObject *parent, DownloadManager *downloads) {
  const QString storage = webEngineStoragePath();
  QDir().mkpath(storage);

  // Named, persistent (non-off-the-record) profile. QWebEngineProfile picks
  // sane cache/storage subpaths under persistentStoragePath by default; we
  // just point it at our own directory instead of Qt's default location.
  auto *profile = new QWebEngineProfile(QStringLiteral("shinto"), parent);
  profile->setPersistentStoragePath(storage);
  profile->setCachePath(storage + "/cache");
  profile->setPersistentCookiesPolicy(QWebEngineProfile::ForcePersistentCookies);
  profile->setHttpCacheType(QWebEngineProfile::DiskHttpCache);

  QWebEngineSettings *settings = profile->settings();
  settings->setAttribute(QWebEngineSettings::PlaybackRequiresUserGesture, false);
  settings->setAttribute(QWebEngineSettings::JavascriptCanOpenWindows, true);
  settings->setAttribute(QWebEngineSettings::ScreenCaptureEnabled, true);
  // Without this, sites (YouTube) detect no Fullscreen API and gray out
  // the button with "Fullscreen is unavailable". Accepting the matching
  // QWebEnginePage::fullScreenRequested signal is still required in
  // BrowserWindow.
  settings->setAttribute(QWebEngineSettings::FullScreenSupportEnabled, true);

  installScrollbarHidingScript(profile);
  installWebAuthnCapabilityShim(profile);
  installDownloadHandler(profile, downloads);

  return profile;
}

}  // namespace shinto
