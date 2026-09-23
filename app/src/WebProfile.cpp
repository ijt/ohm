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

// QtWebEngine's WebAuthn support hangs pages rather than just lacking
// features. Two distinct hangs, both reproduced on x.com:
//
// 1. PublicKeyCredential.getClientCapabilities() (and the older
//    isUserVerifyingPlatformAuthenticatorAvailable() /
//    isConditionalMediationAvailable() probes) never settle. A page that
//    awaits one before offering passkey vs password UI spins forever
//    ("Continue with Apple" after submitting an email). Confirmed as
//    QtWebEngine, not one site, via qutebrowser #8930.
//
// 2. Even when feature detection is answered, a site that already knows
//    the account has a passkey (x.com after a username: "Sign in with
//    passkey") calls navigator.credentials.get({publicKey}) and waits on
//    that promise. Qt never shows the transport picker: its dialog
//    controller only emits webAuthUxRequested once a PIN/account/touch
//    step is pending, and hybrid/QR (the usual phone passkey) is not in
//    QWebEngineWebAuthUxRequest at all. With no authenticator UI, the
//    ceremony does not time out, and the page spinner never stops.
//
// A native "yes" from those probes is not usable here -- it just sends
// the site into hang (2) -- so this does not race the real calls. Probes
// resolve immediately to "no WebAuthn", and publicKey get/create reject
// immediately with NotSupportedError (what Chromium reports when the API
// is disabled), which sites treat as "fall back to a password" rather
// than "the user cancelled". Password-manager credentials.get({password})
// is left alone.
//
// DocumentCreation (Chromium document_start), not DocumentReady: this
// never touches the DOM, and it has to be in place before an early
// <head> inline script or auth SDK runs. A ceremony that still reaches
// Qt is cancelled in BrowserWindow so it can't hang one layer deeper.
void installWebAuthnShim(QWebEngineProfile *profile) {
  QWebEngineScript script;
  script.setName(QStringLiteral("shinto-webauthn-shim"));
  script.setInjectionPoint(QWebEngineScript::DocumentCreation);
  script.setWorldId(QWebEngineScript::MainWorld);
  script.setRunsOnSubFrames(true);
  script.setSourceCode(QStringLiteral(R"JS(
(function() {
  function notSupported() {
    return Promise.reject(new DOMException('WebAuthn is not available.', 'NotSupportedError'));
  }

  function stub(obj, name, value) {
    if (!obj || typeof obj[name] !== 'function' || obj[name].__shinto) return;
    function patched() { return Promise.resolve(value); }
    patched.__shinto = true;
    try {
      Object.defineProperty(obj, name,
        { value: patched, writable: true, configurable: true, enumerable: false });
    } catch (e) {
      try { obj[name] = patched; } catch (e2) {}
    }
  }

  // Static methods on the class itself, per the WebAuthn spec.
  if (typeof PublicKeyCredential !== 'undefined') {
    stub(PublicKeyCredential, 'getClientCapabilities', {
      conditionalCreate: false, conditionalGet: false, hybridTransport: false,
      passkeyPlatformAuthenticator: false, userVerifyingPlatformAuthenticator: false,
      relatedOrigins: false, signalAllAcceptedCredentials: false,
      signalCurrentUserDetails: false, signalUnknownCredential: false
    });
    stub(PublicKeyCredential, 'isUserVerifyingPlatformAuthenticatorAvailable', false);
    stub(PublicKeyCredential, 'isConditionalMediationAvailable', false);
  }

  function rejectPublicKey(obj, name) {
    if (!obj || typeof obj[name] !== 'function' || obj[name].__shinto) return;
    var real = obj[name];
    function patched(options) {
      if (options && options.publicKey) return notSupported();
      return real.apply(this, arguments);
    }
    patched.__shinto = true;
    try {
      Object.defineProperty(obj, name,
        { value: patched, writable: true, configurable: true });
    } catch (e) {
      try { obj[name] = patched; } catch (e2) {}
    }
  }

  var proto = (typeof CredentialsContainer !== 'undefined') ? CredentialsContainer.prototype : null;
  rejectPublicKey(proto, 'get');
  rejectPublicKey(proto, 'create');
  if (navigator.credentials) {
    rejectPublicKey(navigator.credentials, 'get');
    rejectPublicKey(navigator.credentials, 'create');
  }
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
  installWebAuthnShim(profile);
  installDownloadHandler(profile, downloads);

  return profile;
}

}  // namespace shinto
