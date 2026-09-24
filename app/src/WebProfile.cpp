#include "WebProfile.h"

#include <QDir>
#include <QWebEngineDownloadRequest>
#include <QWebEngineProfile>
#include <QWebEngineScript>
#include <QWebEngineScriptCollection>
#include <QWebEngineSettings>

#include "DownloadManager.h"
#include "PasskeyBroker.h"
#include "Ohm.h"

namespace ohm {

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
  script.setName(QStringLiteral("ohm-hide-scrollbars"));
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
// 2. A site that already knows the account has a passkey (x.com after a
//    username: "Sign in with passkey") calls
//    navigator.credentials.get({publicKey}) and waits on that promise. Qt
//    never shows a transport picker, hybrid/QR (the usual phone passkey)
//    isn't in QWebEngineWebAuthUxRequest at all, and the ceremony never
//    times out, so the page spinner never stops.
//
// So Qt's WebAuthn is never reached. Probes answer at once. publicKey
// get/create go to PasskeyBroker, which runs the phone (hybrid) ceremony
// itself, when the page has a broker token (window.__ohmPasskey, set by
// a per-page script); without one they reject with NotSupportedError, what
// Chromium reports when the API is disabled, which sites treat as "fall
// back to a password" rather than "the user cancelled". Password-manager
// credentials.get({password}) is left alone.
//
// Results are built as real PublicKeyCredential/Authenticator*Response
// instances (own properties shadowing the native getters), the way
// password-manager extensions do it, so `instanceof` and toJSON() work.
//
// DocumentCreation (Chromium document_start), not DocumentReady: this
// never touches the DOM, and it has to be in place before an early
// <head> inline script or auth SDK runs -- which also means fetch and
// friends are captured before the page can replace them. A ceremony that
// still reaches Qt is cancelled in BrowserWindow so it can't hang one
// layer deeper.
void installWebAuthnShim(QWebEngineProfile *profile) {
  QWebEngineScript script;
  script.setName(QStringLiteral("ohm-webauthn-shim"));
  script.setInjectionPoint(QWebEngineScript::DocumentCreation);
  script.setWorldId(QWebEngineScript::MainWorld);
  script.setRunsOnSubFrames(true);
  script.setSourceCode(QStringLiteral(R"JS(
(function() {
  var fetch_ = window.fetch, stringify = JSON.stringify, atob_ = atob, btoa_ = btoa;
  var DOMException_ = DOMException, defineProperty = Object.defineProperty;

  // Read at call time: the per-page token script may run after this one.
  function available() { return typeof window.__ohmPasskey === 'string'; }

  function notSupported() {
    return Promise.reject(new DOMException_('WebAuthn is not available.', 'NotSupportedError'));
  }

  function install(obj, name, fn) {
    fn.__ohm = true;
    try {
      defineProperty(obj, name, { value: fn, writable: true, configurable: true, enumerable: false });
    } catch (e) {
      try { obj[name] = fn; } catch (e2) {}
    }
  }

  function stub(obj, name, value) {
    if (!obj || typeof obj[name] !== 'function' || obj[name].__ohm) return;
    install(obj, name, function() {
      return Promise.resolve(typeof value === 'function' ? value() : value);
    });
  }

  // Static methods on the class itself, per the WebAuthn spec. No
  // user-verifying platform authenticator (no Windows Hello / Touch ID
  // here), but passkeys via a phone, when the broker is there.
  if (typeof PublicKeyCredential !== 'undefined') {
    stub(PublicKeyCredential, 'getClientCapabilities', function() {
      var phone = available();
      return {
        conditionalCreate: false, conditionalGet: false, hybridTransport: phone,
        passkeyPlatformAuthenticator: phone, userVerifyingPlatformAuthenticator: false,
        relatedOrigins: false, signalAllAcceptedCredentials: false,
        signalCurrentUserDetails: false, signalUnknownCredential: false
      };
    });
    stub(PublicKeyCredential, 'isUserVerifyingPlatformAuthenticatorAvailable', false);
    stub(PublicKeyCredential, 'isConditionalMediationAvailable', false);
  }

  function toBase64url(bytes) {
    var s = '';
    for (var i = 0; i < bytes.length; i++) s += String.fromCharCode(bytes[i]);
    return btoa_(s).replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '');
  }

  function fromBase64url(s) {
    s = String(s).replace(/-/g, '+').replace(/_/g, '/');
    while (s.length % 4) s += '=';
    var bin = atob_(s), bytes = new Uint8Array(bin.length);
    for (var i = 0; i < bin.length; i++) bytes[i] = bin.charCodeAt(i);
    return bytes.buffer;
  }

  // Options with BufferSources -> the WebAuthn JSON forms (base64url).
  function encode(v) {
    if (v instanceof ArrayBuffer) return toBase64url(new Uint8Array(v));
    if (ArrayBuffer.isView(v)) return toBase64url(new Uint8Array(v.buffer, v.byteOffset, v.byteLength));
    if (Array.isArray(v)) return v.map(encode);
    if (v && typeof v === 'object') {
      var out = {};
      Object.keys(v).forEach(function(k) { if (v[k] !== undefined) out[k] = encode(v[k]); });
      return out;
    }
    return v;
  }

  // Writable and configurable: libraries patch the result the way they
  // would a native credential, whose methods live on the prototype --
  // @github/webauthn-json assigns credential.toJSON, which throws in strict
  // mode on a read-only own property (GitHub: "Authentication failed").
  function withOwn(obj, props) {
    Object.keys(props).forEach(function(k) {
      defineProperty(obj, k,
        { value: props[k], enumerable: true, writable: true, configurable: true });
    });
    return obj;
  }

  function extensionResults(ext) {
    var out = {};
    Object.keys(ext || {}).forEach(function(k) { out[k] = ext[k]; });
    if (ext && ext.prf && ext.prf.results) {
      var results = { first: fromBase64url(ext.prf.results.first) };
      if (ext.prf.results.second) results.second = fromBase64url(ext.prf.results.second);
      out.prf = { enabled: ext.prf.enabled, results: results };
    }
    if (ext && ext.largeBlob && ext.largeBlob.blob) {
      out.largeBlob = { supported: ext.largeBlob.supported, blob: fromBase64url(ext.largeBlob.blob) };
    }
    return out;
  }

  function toCredential(json) {
    var r = json.response || {};
    var response = r.attestationObject
      ? withOwn(Object.create(AuthenticatorAttestationResponse.prototype), {
          clientDataJSON: fromBase64url(r.clientDataJSON),
          attestationObject: fromBase64url(r.attestationObject),
          getTransports: function() { return (r.transports || []).slice(); },
          getAuthenticatorData: function() { return fromBase64url(r.authenticatorData); },
          getPublicKey: function() { return r.publicKey ? fromBase64url(r.publicKey) : null; },
          getPublicKeyAlgorithm: function() { return r.publicKeyAlgorithm; }
        })
      : withOwn(Object.create(AuthenticatorAssertionResponse.prototype), {
          clientDataJSON: fromBase64url(r.clientDataJSON),
          authenticatorData: fromBase64url(r.authenticatorData),
          signature: fromBase64url(r.signature),
          userHandle: r.userHandle ? fromBase64url(r.userHandle) : null
        });
    return withOwn(Object.create(PublicKeyCredential.prototype), {
      id: json.id,
      rawId: fromBase64url(json.rawId),
      type: 'public-key',
      response: response,
      authenticatorAttachment: json.authenticatorAttachment || 'cross-platform',
      getClientExtensionResults: function() { return extensionResults(json.clientExtensionResults); },
      toJSON: function() { return json; }
    });
  }

  function aborted(signal) {
    return signal.reason !== undefined ? signal.reason : new DOMException_('Aborted.', 'AbortError');
  }

  function ceremony(type, options) {
    // Passkey autofill needs UI in the page's own fields; not offered
    // (isConditionalMediationAvailable says so), but some sites try anyway.
    if (options.mediation === 'conditional') return notSupported();
    var signal = options.signal;
    if (signal && signal.aborted) return Promise.reject(aborted(signal));
    var topOrigin = null;
    try {
      var ancestors = location.ancestorOrigins;
      if (window.top !== window && ancestors && ancestors.length) {
        topOrigin = ancestors[ancestors.length - 1];
      }
    } catch (e) {}
    var body = stringify({
      token: window.__ohmPasskey, type: type, topOrigin: topOrigin,
      options: encode(options.publicKey)
    });
    return fetch_.call(window, 'ohm-passkey:ceremony',
                       { method: 'POST', body: body, signal: signal, credentials: 'omit' })
      .then(function(r) { return r.json(); })
      .then(function(reply) {
        if (reply.result) return toCredential(reply.result);
        var e = reply.error || {};
        if (e.name === 'TypeError') throw new TypeError(e.message);
        throw new DOMException_(e.message || 'The operation failed.', e.name || 'NotAllowedError');
      }, function() {
        if (signal && signal.aborted) throw aborted(signal);
        throw new DOMException_('The passkey request failed.', 'NotAllowedError');
      });
  }

  function routePublicKey(obj, name, type) {
    if (!obj || typeof obj[name] !== 'function' || obj[name].__ohm) return;
    var real = obj[name];
    install(obj, name, function(options) {
      if (options && options.publicKey) {
        return available() ? ceremony(type, options) : notSupported();
      }
      return real.apply(this, arguments);
    });
  }

  var proto = (typeof CredentialsContainer !== 'undefined') ? CredentialsContainer.prototype : null;
  routePublicKey(proto, 'get', 'get');
  routePublicKey(proto, 'create', 'create');
  if (navigator.credentials) {
    routePublicKey(navigator.credentials, 'get', 'get');
    routePublicKey(navigator.credentials, 'create', 'create');
  }
})();
)JS"));
  profile->scripts()->insert(script);
}

// Nothing in Ohm connected to QWebEngineProfile::downloadRequested, and
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
  auto *profile = new QWebEngineProfile(QStringLiteral("ohm"), parent);
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
  PasskeyBroker::install(profile);
  installDownloadHandler(profile, downloads);

  return profile;
}

}  // namespace ohm
