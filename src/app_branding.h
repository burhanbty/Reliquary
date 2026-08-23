#pragma once

#include <QUrl>

#include <functional>

class QSettings;

namespace vidstorex::branding {

inline constexpr char kProductName[] = "Reliquary";
inline constexpr char kOrganizationName[] = "Media Storage";
inline constexpr char kLegacyApplicationName[] = "YouTube Media Storage";
inline constexpr char kLegacyProductApplicationName[] = "VidStoreX";
inline constexpr int kBrandIntroVersion = 1;
inline constexpr int kOnboardingVersion = 1;
inline constexpr char kDefinitionEnglish[] =
    "A container for preserving something precious.";
inline constexpr char kDefinitionTurkish[] =
    "Değerli bir şeyi korumak için kullanılan muhafaza.";
inline constexpr char kAuthorName[] = "Burhan Talha Yazıcı";
inline constexpr char kAuthorAlias[] = "BTY";
inline constexpr char kLinkedInUrl[] =
    "https://www.linkedin.com/in/burhanbty";

[[nodiscard]] QUrl linkedInUrl();
[[nodiscard]] bool openLinkedInProfile(
    const std::function<bool(const QUrl &)> &opener = {});
[[nodiscard]] int copyMissingSettings(const QSettings &legacy,
                                      QSettings &target);
[[nodiscard]] int migrateLegacySettings();

} // namespace vidstorex::branding
