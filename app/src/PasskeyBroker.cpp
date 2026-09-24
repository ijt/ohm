#include "PasskeyBroker.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>
#include <QTimer>
#include <QUuid>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineScript>
#include <QWebEngineScriptCollection>
#include <QWebEngineUrlRequestJob>
#include <QWebEngineUrlScheme>

#include "PasskeyOverlay.h"

namespace shinto {

namespace {

const QByteArray kScheme = QByteArrayLiteral("shinto-passkey");

// Long enough to find the phone and scan; Chrome's own hybrid default is
// similar. The page's own `timeout` is advisory in the spec anyway.
constexpr int kCeremonyTimeoutMs = 3 * 60 * 1000;

// scheme://host[:port] -- the serialization WebAuthn's clientDataJSON and
// rp.id checks use. Empty for opaque origins (sandboxed frames, data:).
QString serializeOrigin(const QUrl &url) {
  if (!url.isValid() || url.host().isEmpty()) return {};
  QString origin = url.scheme() + QStringLiteral("://") + url.host(QUrl::FullyEncoded);
  if (url.port() != -1) origin += QLatin1Char(':') + QString::number(url.port());
  return origin;
}

// WebAuthn only runs in secure contexts: https, or http on localhost.
bool isSecureOrigin(const QUrl &url) {
  if (url.scheme() == QLatin1String("https")) return true;
  const QString host = url.host();
  return url.scheme() == QLatin1String("http") &&
         (host == QLatin1String("localhost") || host == QLatin1String("127.0.0.1"));
}

QJsonObject domError(const QString &name, const QString &message) {
  return {{QStringLiteral("error"),
           QJsonObject{{QStringLiteral("name"), name}, {QStringLiteral("message"), message}}}};
}

void reply(QWebEngineUrlRequestJob *job, const QString &origin, const QJsonObject &body) {
  // The page fetch()es this cross-origin; without this header Chromium
  // hides the response from it.
  QMultiMap<QByteArray, QByteArray> headers;
  headers.insert(QByteArrayLiteral("Access-Control-Allow-Origin"), origin.toUtf8());
  job->setAdditionalResponseHeaders(headers);
  auto *buffer = new QBuffer(job);
  buffer->setData(QJsonDocument(body).toJson(QJsonDocument::Compact));
  buffer->open(QIODevice::ReadOnly);
  job->reply(QByteArrayLiteral("application/json"), buffer);
}

// One passkey ceremony: a helper process, and the overlay showing its QR
// code. A child of the request job, so a page that navigates away or
// aborts the fetch (AbortSignal) takes the helper down with it.
class Ceremony : public QObject {
 public:
  Ceremony(QWebEngineUrlRequestJob *job, PasskeyOverlay *overlay, const QString &origin,
           const QString &helper, const QJsonObject &request)
      : QObject(job), job_(job), overlay_(overlay), origin_(origin) {
    const bool create = request.value(QStringLiteral("type")).toString() == QLatin1String("create");
    overlay_->begin(QUrl(origin).host(), create);
    connect(overlay_, &PasskeyOverlay::cancelled, this,
            [this] { finish(domError(QStringLiteral("NotAllowedError"),
                                     QStringLiteral("The user cancelled."))); });

    QTimer::singleShot(kCeremonyTimeoutMs, this, [this] {
      finish(domError(QStringLiteral("NotAllowedError"), QStringLiteral("Timed out.")));
    });

    connect(&process_, &QProcess::readyReadStandardOutput, this, &Ceremony::readLines);
    connect(&process_, &QProcess::finished, this, [this] {
      readLines();
      finish(domError(QStringLiteral("NotAllowedError"),
                      QStringLiteral("The passkey helper stopped unexpectedly.")));
    });
    connect(&process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
      if (e != QProcess::FailedToStart) return;
      finish(domError(QStringLiteral("NotSupportedError"),
                      QStringLiteral("Could not start the passkey helper.")));
    });
    // Its log lines (and libwebauthn's) are for a terminal, not the page.
    process_.setProcessChannelMode(QProcess::ForwardedErrorChannel);
    process_.start(helper, {});
    process_.write(QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n');
    process_.closeWriteChannel();
  }

  ~Ceremony() override {
    process_.disconnect(this);
    if (process_.state() != QProcess::NotRunning) {
      process_.kill();
      process_.waitForFinished(1000);
    }
    // After finish() the overlay may already be showing a newer request.
    if (!done_ && overlay_) overlay_->end();
  }

 private:
  void readLines() {
    buffer_ += process_.readAllStandardOutput();
    for (int nl = buffer_.indexOf('\n'); nl != -1 && !done_; nl = buffer_.indexOf('\n')) {
      const QJsonObject line = QJsonDocument::fromJson(buffer_.left(nl)).object();
      buffer_.remove(0, nl + 1);
      if (line.contains(QStringLiteral("qr"))) {
        const QJsonObject qr = line.value(QStringLiteral("qr")).toObject();
        if (overlay_) {
          overlay_->showQr(qr.value(QStringLiteral("size")).toInt(),
                           qr.value(QStringLiteral("modules")).toString());
        }
      } else if (line.contains(QStringLiteral("status"))) {
        if (overlay_) overlay_->setStatus(line.value(QStringLiteral("status")).toString());
      } else if (line.contains(QStringLiteral("result")) || line.contains(QStringLiteral("error"))) {
        finish(line);
      }
    }
  }

  void finish(const QJsonObject &body) {
    if (done_) return;
    done_ = true;
    if (overlay_) overlay_->end();
    if (job_) reply(job_, origin_, body);
    // The job owns this object; it goes away when Chromium drops the job.
    // Stop the helper now rather than waiting for that.
    if (process_.state() != QProcess::NotRunning) process_.kill();
  }

  QPointer<QWebEngineUrlRequestJob> job_;
  QPointer<PasskeyOverlay> overlay_;
  QString origin_;
  QProcess process_;
  QByteArray buffer_;
  bool done_ = false;
};

}  // namespace

PasskeyBroker *PasskeyBroker::instance_ = nullptr;

void PasskeyBroker::registerScheme() {
  QWebEngineUrlScheme scheme(kScheme);
  scheme.setSyntax(QWebEngineUrlScheme::Syntax::Path);
  // Secure so https pages can reach it without mixed-content blocking;
  // CSP-ignored because nearly every site with passkeys has a strict
  // connect-src that would otherwise block the shim's fetch.
  scheme.setFlags(QWebEngineUrlScheme::SecureScheme | QWebEngineUrlScheme::CorsEnabled |
                  QWebEngineUrlScheme::FetchApiAllowed |
                  QWebEngineUrlScheme::ContentSecurityPolicyIgnored);
  QWebEngineUrlScheme::registerScheme(scheme);
}

void PasskeyBroker::install(QWebEngineProfile *profile) {
  instance_ = new PasskeyBroker(profile);
  profile->installUrlSchemeHandler(kScheme, instance_);
}

PasskeyBroker::PasskeyBroker(QObject *parent) : QWebEngineUrlSchemeHandler(parent) {}

QString PasskeyBroker::helperPath() {
  const QString dir = QCoreApplication::applicationDirPath();
  const QStringList candidates = {
      qEnvironmentVariable("SHINTO_PASSKEY_HELPER"),
      // Built and installed next to shinto-bin (see app/CMakeLists.txt).
      dir + QStringLiteral("/shinto-passkey"),
      QStandardPaths::findExecutable(QStringLiteral("shinto-passkey")),
  };
  for (const QString &path : candidates) {
    if (!path.isEmpty() && QFileInfo(path).isExecutable()) return QFileInfo(path).canonicalFilePath();
  }
  return {};
}

void PasskeyBroker::attach(QWebEnginePage *page, PasskeyOverlay *overlay) {
  if (helperPath().isEmpty()) return;
  const QString token = QUuid::createUuid().toString(QUuid::WithoutBraces);
  overlays_.insert(token, overlay);
  connect(page, &QObject::destroyed, this, [this, token] { overlays_.remove(token); });

  QWebEngineScript script;
  script.setName(QStringLiteral("shinto-passkey-token"));
  script.setInjectionPoint(QWebEngineScript::DocumentCreation);
  script.setWorldId(QWebEngineScript::MainWorld);
  script.setRunsOnSubFrames(true);
  script.setSourceCode(QStringLiteral(
      "Object.defineProperty(window, '__shintoPasskey', {value: '%1'});").arg(token));
  page->scripts().insert(script);
}

void PasskeyBroker::requestStarted(QWebEngineUrlRequestJob *job) {
  const QUrl initiator = job->initiator();
  const QString origin = serializeOrigin(initiator);
  if (origin.isEmpty() || !isSecureOrigin(initiator)) {
    // No ACAO for an origin we won't serve: the page just sees a failure.
    job->fail(QWebEngineUrlRequestJob::RequestDenied);
    return;
  }
  if (job->requestMethod() != "POST" || !job->requestBody()) {
    job->fail(QWebEngineUrlRequestJob::RequestFailed);
    return;
  }
  // Handed over closed; reading it unopened silently gives nothing.
  QIODevice *bodyDevice = job->requestBody();
  if (!bodyDevice->isOpen()) bodyDevice->open(QIODevice::ReadOnly);
  const QJsonObject body = QJsonDocument::fromJson(bodyDevice->readAll()).object();

  PasskeyOverlay *overlay = overlays_.value(body.value(QStringLiteral("token")).toString());
  const QString helper = helperPath();
  if (!overlay || helper.isEmpty()) {
    reply(job, origin, domError(QStringLiteral("NotSupportedError"),
                                QStringLiteral("Phone passkeys are not available here.")));
    return;
  }
  if (overlay->isBusy()) {
    reply(job, origin, domError(QStringLiteral("NotAllowedError"),
                                QStringLiteral("Another passkey request is in progress.")));
    return;
  }

  const QString type = body.value(QStringLiteral("type")).toString();
  if (type != QLatin1String("create") && type != QLatin1String("get")) {
    reply(job, origin, domError(QStringLiteral("TypeError"), QStringLiteral("Unknown request.")));
    return;
  }
  QJsonObject request{{QStringLiteral("type"), type},
                      {QStringLiteral("origin"), origin},
                      {QStringLiteral("options"), body.value(QStringLiteral("options"))}};
  // A frame's report of its own top-level origin can only misdescribe the
  // frame's own credentials, never another site's; rp.id is checked
  // against `origin` regardless.
  const QUrl top(body.value(QStringLiteral("topOrigin")).toString());
  const QString topOrigin = serializeOrigin(top);
  if (!topOrigin.isEmpty() && topOrigin != origin && isSecureOrigin(top)) {
    request.insert(QStringLiteral("topOrigin"), topOrigin);
  }
  new Ceremony(job, overlay, origin, helper, request);
}

}  // namespace shinto
