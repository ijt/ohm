// Phone passkeys: the WebAuthn "hybrid" transport, i.e. the QR code you
// scan with a phone's camera, then Bluetooth proves the phone is nearby and
// the phone signs in over Apple's or Google's relay. QtWebEngine has no UI
// for it (see WebProfile.cpp's shim), so Ohm routes the ceremony itself:
//
//   page JS (the shim) --fetch--> ohm-browser-passkey: scheme --> this broker
//     --stdin/stdout JSON--> ohm-browser-passkey helper (libwebauthn, Rust)
//
// The origin a credential is for is never the page's word: it comes from
// Chromium (QWebEngineUrlRequestJob::initiator()), and the helper checks
// rp.id against it and builds clientDataJSON from it.
#pragma once

#include <QHash>
#include <QPointer>
#include <QString>
#include <QWebEngineUrlSchemeHandler>

class QWebEnginePage;
class QWebEngineProfile;

namespace ohm {

class PasskeyOverlay;

class PasskeyBroker : public QWebEngineUrlSchemeHandler {
  Q_OBJECT

 public:
  // Must run before QApplication is constructed (Qt's rule for schemes).
  static void registerScheme();

  // Installs the broker on `profile`. Call once.
  static void install(QWebEngineProfile *profile);
  static PasskeyBroker *instance() { return instance_; }

  // Lets `page` use phone passkeys, with `overlay` showing its QR code.
  // Gives the page a token (a per-page script) that says which window a
  // request came from; without one, the shim keeps rejecting passkeys.
  // A no-op when the helper isn't installed.
  void attach(QWebEnginePage *page, PasskeyOverlay *overlay);

  void requestStarted(QWebEngineUrlRequestJob *job) override;

 private:
  explicit PasskeyBroker(QObject *parent);

  // Empty when there is no helper to run.
  static QString helperPath();

  static PasskeyBroker *instance_;
  QHash<QString, QPointer<PasskeyOverlay>> overlays_;
};

}  // namespace ohm
