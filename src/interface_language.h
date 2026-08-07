#pragma once

#include <QString>

namespace vidstorex_ui {

inline constexpr auto kEnglishLanguage = "en";
inline constexpr auto kTurkishLanguage = "tr";

[[nodiscard]] QString resolve_language(
    const QString &saved_language,
    const QString &system_locale_name);

[[nodiscard]] bool is_supported_language(const QString &language);

} // namespace vidstorex_ui
