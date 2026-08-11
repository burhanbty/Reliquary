#include "app_branding.h"

#include <QDesktopServices>

namespace vidstorex::branding {

QUrl linkedInUrl() {
    return QUrl(QString::fromLatin1(kLinkedInUrl), QUrl::StrictMode);
}

bool openLinkedInProfile(
    const std::function<bool(const QUrl &)> &opener) {
    const QUrl url = linkedInUrl();
    if (!url.isValid() || url.scheme() != QStringLiteral("https") ||
        url.host() != QStringLiteral("www.linkedin.com"))
        return false;
    return opener ? opener(url) : QDesktopServices::openUrl(url);
}

} // namespace vidstorex::branding
