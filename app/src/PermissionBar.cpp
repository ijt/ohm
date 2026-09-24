#include "PermissionBar.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSettings>

#include "Shinto.h"

namespace shinto {

namespace {

using Type = QWebEnginePermission::PermissionType;

// Finishes "<site> wants to ...". Empty for types there's nothing to ask.
QString wantsTo(Type type) {
  switch (type) {
    case Type::MediaAudioCapture:
      return QStringLiteral("use your microphone");
    case Type::MediaVideoCapture:
      return QStringLiteral("use your camera");
    case Type::MediaAudioVideoCapture:
      return QStringLiteral("use your camera and microphone");
    case Type::DesktopVideoCapture:
      return QStringLiteral("share your screen");
    case Type::DesktopAudioVideoCapture:
      return QStringLiteral("share your screen and its audio");
    case Type::Notifications:
      return QStringLiteral("show notifications");
    case Type::Geolocation:
      return QStringLiteral("know your location");
    case Type::ClipboardReadWrite:
      return QStringLiteral("see what you copy to the clipboard");
    case Type::LocalFontsAccess:
      return QStringLiteral("use the fonts installed on this computer");
    case Type::MouseLock:
    case Type::Unsupported:
      break;
  }
  return {};
}

// Qt persists notifications, location, clipboard and fonts per origin, but
// never camera/microphone (QWebEnginePermission::isPersistent), so a video
// call site would ask on every call. Chrome remembers those per site; so
// do we. Screen sharing isn't remembered anywhere: the picker is the
// consent, every time.
bool isRememberedMedia(Type type) {
  return type == Type::MediaAudioCapture || type == Type::MediaVideoCapture ||
         type == Type::MediaAudioVideoCapture;
}

QString mediaKey(const QWebEnginePermission &permission) {
  // QSettings treats '/' as a group separator; percent-encode the origin.
  return QString::fromLatin1(QUrl::toPercentEncoding(permission.origin().toString())) +
         QLatin1Char('/') + QString::number(int(permission.permissionType()));
}

QSettings &mediaAnswers() {
  static QSettings settings(mediaPermissionsPath(), QSettings::IniFormat);
  return settings;
}

}  // namespace

PermissionBar::PermissionBar(QWidget *parent) : QWidget(parent) {
  setObjectName(QStringLiteral("PermissionBar"));
  setAttribute(Qt::WA_StyledBackground);
  // Clicks only: the keyboard stays with the page underneath.
  setFocusPolicy(Qt::NoFocus);
  hide();

  auto *layout = new QHBoxLayout(this);
  layout->setContentsMargins(14, 8, 10, 8);
  layout->setSpacing(8);
  text_ = new QLabel(this);
  text_->setObjectName(QStringLiteral("PermissionText"));
  text_->setTextFormat(Qt::PlainText);
  text_->setWordWrap(true);
  block_ = new QPushButton(QStringLiteral("Block"), this);
  allow_ = new QPushButton(QStringLiteral("Allow"), this);
  for (QPushButton *b : {block_, allow_}) {
    b->setFocusPolicy(Qt::NoFocus);
    b->setCursor(Qt::PointingHandCursor);
  }
  allow_->setObjectName(QStringLiteral("PermissionAllow"));
  block_->setObjectName(QStringLiteral("PermissionBlock"));
  layout->addWidget(text_, 1);
  layout->addWidget(block_);
  layout->addWidget(allow_);
  connect(block_, &QPushButton::clicked, this, [this] { answer(false); });
  connect(allow_, &QPushButton::clicked, this, [this] { answer(true); });
}

void PermissionBar::applyPalette(const Palette &palette) {
  // Numbered from %1, one argument per placeholder -- see FindBar.
  setStyleSheet(QStringLiteral(
                    "#PermissionBar { background: %1; border-bottom: 1px solid %3; }"
                    "#PermissionText { color: %2; font-family: %4; font-size: 13px; }"
                    "#PermissionBar QPushButton { color: %2; background: transparent;"
                    " border: 1px solid %3; border-radius: 4px; padding: 4px 14px;"
                    " font-family: %4; }"
                    "#PermissionBar QPushButton:hover { border-color: %5; }"
                    "#PermissionBar #PermissionAllow { color: %1; background: %5; border-color: %5; }")
                    .arg(palette.card, palette.fg, palette.muted, palette.font, palette.accent));
}

void PermissionBar::request(const QWebEnginePermission &permission) {
  const Type type = permission.permissionType();
  if (type == Type::MouseLock) {
    // Games and 3D viewers; Chrome grants it silently too, and Escape
    // always releases the pointer.
    permission.grant();
    return;
  }
  if (wantsTo(type).isEmpty()) {
    permission.deny();
    return;
  }
  if (isRememberedMedia(type)) {
    const QVariant remembered = mediaAnswers().value(mediaKey(permission));
    if (remembered.isValid()) {
      remembered.toBool() ? permission.grant() : permission.deny();
      return;
    }
  }
  // A page that asks again while its first request is still up gets one
  // prompt, and both answers.
  for (const QWebEnginePermission &queued : queue_) {
    if (queued.origin() == permission.origin() && queued.permissionType() == type) {
      queue_.append(permission);
      return;
    }
  }
  queue_.append(permission);
  if (queue_.size() == 1) showNext();
}

void PermissionBar::showNext() {
  // Requests go stale when their page navigates or closes.
  while (!queue_.isEmpty() && !queue_.first().isValid()) queue_.removeFirst();
  if (queue_.isEmpty()) {
    hide();
    emit visibilityChanged();
    return;
  }
  const QWebEnginePermission &next = queue_.first();
  text_->setText(QStringLiteral("%1 wants to %2")
                     .arg(next.origin().host(), wantsTo(next.permissionType())));
  show();
  raise();
  emit visibilityChanged();
}

void PermissionBar::answer(bool allow) {
  if (queue_.isEmpty()) return;
  const QWebEnginePermission first = queue_.takeFirst();
  if (isRememberedMedia(first.permissionType())) {
    mediaAnswers().setValue(mediaKey(first), allow);
    mediaAnswers().sync();
  }
  // Duplicates of the same question get the same answer.
  for (auto it = queue_.begin(); it != queue_.end();) {
    if (it->origin() == first.origin() && it->permissionType() == first.permissionType()) {
      allow ? it->grant() : it->deny();
      it = queue_.erase(it);
    } else {
      ++it;
    }
  }
  allow ? first.grant() : first.deny();
  showNext();
}

}  // namespace shinto
