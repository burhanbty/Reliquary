#pragma once

#include <QUrl>

#include <functional>

namespace vidstorex::branding {

inline constexpr char kAuthorName[] = "Burhan Talha Yazıcı";
inline constexpr char kAuthorAlias[] = "BTY";
inline constexpr char kLinkedInUrl[] =
    "https://www.linkedin.com/in/burhanbty";

[[nodiscard]] QUrl linkedInUrl();
[[nodiscard]] bool openLinkedInProfile(
    const std::function<bool(const QUrl &)> &opener = {});

} // namespace vidstorex::branding
