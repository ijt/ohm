// Web notifications (new Notification(...), service-worker
// showNotification) as desktop notifications. QtWebEngine hands each one
// to the profile's notification presenter; without one they vanish.
//
// Shown with notify-send like Shinto's own notices (Notify.h), under the
// site's name. A click runs the page's onclick and brings the site's
// window forward; a newer notification with the same tag replaces the
// earlier one, as in Chrome.
#pragma once

#include <functional>

class QUrl;
class QWebEngineProfile;

namespace shinto {

// `focusOrigin` brings forward a window showing a page from that origin.
void installNotificationPresenter(QWebEngineProfile *profile,
                                  std::function<void(const QUrl &origin)> focusOrigin);

}  // namespace shinto
