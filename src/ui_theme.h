#pragma once

#include <QColor>
#include <QPalette>
#include <QString>

class QWidget;

namespace vidstorex_ui {

struct Spacing final {
    static constexpr int Xs = 4;
    static constexpr int Sm = 8;
    static constexpr int Md = 12;
    static constexpr int Lg = 20;
    static constexpr int Xl = 28;
};

struct Radius final {
    static constexpr int Sm = 5;
    static constexpr int Md = 8;
    static constexpr int Lg = 12;
};

struct Control final {
    static constexpr int Compact = 30;
    static constexpr int Standard = 36;
    static constexpr int Large = 42;
};

struct Layout final {
    static constexpr int ContentMaxWidth = 1420;
    static constexpr int HeroPadding = 18;
    static constexpr int SectionGap = 18;
    static constexpr int CompactActionGap = 8;
    static constexpr int RecentVisibleRows = 4;
};

enum class ResponsiveMode { Compact, Normal, Wide };

// Application-wide logical-pixel geometry.  Pages consume these values instead
// of growing their own unrelated margins and minimum heights.
struct DensityMetrics final {
    int pageMargin;
    int pageMaxWidth;
    int sectionGap;
    int cardPadding;
    int compactCardPadding;
    int controlHeight;
    int buttonHeight;
    int formRowGap;
    int navigationHeight;
    int wizardActionHeight;
    int tableMinHeight;
};

[[nodiscard]] ResponsiveMode responsiveModeForViewport(int width, int height);
[[nodiscard]] DensityMetrics densityMetrics(ResponsiveMode mode);
[[nodiscard]] QString responsiveModeName(ResponsiveMode mode);

struct ThemeTokens final {
    bool dark = false;
    QColor surfaceBase;
    QColor surfacePage;
    QColor surfaceRaised;
    QColor surfaceRecent;
    QColor surfaceTrust;
    QColor surfaceHover;
    QColor surfaceSelected;
    QColor textPrimary;
    QColor textSecondary;
    QColor textMuted;
    QColor accent;
    QColor accentHover;
    QColor accentPressed;
    QColor onAccent;
    QColor success;
    QColor warning;
    QColor error;
    QColor info;
    QColor border;
    QColor borderStrong;
};

[[nodiscard]] ThemeTokens themeTokens(const QPalette &palette);
[[nodiscard]] QString applicationStyleSheet(const QPalette &palette);
void applyTheme(QWidget *root);

} // namespace vidstorex_ui
