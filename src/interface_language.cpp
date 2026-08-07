#include "interface_language.h"

namespace vidstorex_ui {

bool is_supported_language(const QString &language) {
    return language == QLatin1String(kEnglishLanguage) ||
        language == QLatin1String(kTurkishLanguage);
}

QString resolve_language(const QString &saved_language,
                         const QString &system_locale_name) {
    const QString saved = saved_language.trimmed().toLower();
    if (is_supported_language(saved)) return saved;
    if (!saved.isEmpty()) return QString::fromLatin1(kEnglishLanguage);

    const QString system = system_locale_name.trimmed().toLower();
    return system.startsWith(QLatin1String("tr"))
        ? QString::fromLatin1(kTurkishLanguage)
        : QString::fromLatin1(kEnglishLanguage);
}

} // namespace vidstorex_ui
