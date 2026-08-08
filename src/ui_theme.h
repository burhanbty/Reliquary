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

struct ThemeTokens final {
    bool dark = false;
    QColor surfaceBase;
    QColor surfaceRaised;
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
