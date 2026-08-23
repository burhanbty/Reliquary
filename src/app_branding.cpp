#include "app_branding.h"

#include <QDesktopServices>
#include <QCoreApplication>
#include <QSettings>

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

int copyMissingSettings(const QSettings &legacy, QSettings &target) {
    int copied = 0;
    for (const QString &key : legacy.allKeys()) {
        if (target.contains(key)) continue;
        target.setValue(key, legacy.value(key));
        ++copied;
    }
    if (copied > 0) target.sync();
    return copied;
}

int migrateLegacySettings() {
    QSettings target(QSettings::defaultFormat(), QSettings::UserScope,
                     QString::fromLatin1(kOrganizationName),
                     QString::fromLatin1(kProductName));
    int copied = 0;
    for (const char *legacyName : {kLegacyApplicationName,
                                   kLegacyProductApplicationName}) {
        QSettings legacy(QSettings::defaultFormat(), QSettings::UserScope,
                         QString::fromLatin1(kOrganizationName),
                         QString::fromLatin1(legacyName));
        copied += copyMissingSettings(legacy, target);
    }
    return copied;
}

} // namespace vidstorex::branding
