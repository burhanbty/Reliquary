#include "ui_theme.h"

#include <QWidget>

namespace {

QColor mix(const QColor &first, const QColor &second, const qreal amount) {
    const auto channel = [amount](const int a, const int b) {
        return qRound(a * (1.0 - amount) + b * amount);
    };
    return QColor(channel(first.red(), second.red()),
                  channel(first.green(), second.green()),
                  channel(first.blue(), second.blue()));
}

QString css(const QColor &color) {
    return color.name(QColor::HexRgb);
}

} // namespace

namespace vidstorex_ui {

ThemeTokens themeTokens(const QPalette &palette) {
    ThemeTokens result;
    result.dark = palette.color(QPalette::Window).lightness() < 128;
    const QColor window = palette.color(QPalette::Window);
    const QColor base = palette.color(QPalette::Base);
    const QColor text = palette.color(QPalette::WindowText);
    const QColor anchor = result.dark ? QColor("#E3A94B")
                                      : QColor("#A85E08");
    result.surfaceBase = mix(base, window, result.dark ? 0.16 : 0.09);
    result.surfacePage = mix(window, base, result.dark ? 0.24 : 0.34);
    result.surfaceRaised = mix(base, result.dark ? QColor("#FFFFFF")
                                                : QColor("#FFF8EA"),
                               result.dark ? 0.045 : 0.22);
    result.surfaceRecent = mix(result.surfaceRaised, window,
                               result.dark ? 0.55 : 0.28);
    result.surfaceHover = mix(result.surfaceRaised, anchor,
                              result.dark ? 0.07 : 0.055);
    result.surfaceSelected = mix(result.surfaceRaised, anchor,
                                 result.dark ? 0.15 : 0.11);
    result.textPrimary = text;
    result.textSecondary = mix(text, window, result.dark ? 0.28 : 0.34);
    result.textMuted = mix(text, window, result.dark ? 0.48 : 0.54);
    result.accent = anchor;
    result.accentHover = result.dark ? anchor.lighter(112)
                                     : anchor.darker(108);
    result.accentPressed = result.dark ? anchor.darker(112)
                                       : anchor.darker(122);
    result.onAccent = result.dark ? QColor("#1D160C") : QColor("#FFFFFF");
    result.success = result.dark ? QColor("#62B989") : QColor("#237A4B");
    result.warning = result.dark ? QColor("#E5AD4F") : QColor("#9A5B05");
    result.error = result.dark ? QColor("#E17A75") : QColor("#A63D39");
    result.info = result.dark ? QColor("#72AFC2") : QColor("#36778C");
    result.surfaceTrust = mix(result.surfacePage, result.success,
                              result.dark ? 0.10 : 0.07);
    result.border = mix(text, window, result.dark ? 0.78 : 0.82);
    result.borderStrong = mix(text, window, result.dark ? 0.58 : 0.65);
    return result;
}

QString applicationStyleSheet(const QPalette &palette) {
    const ThemeTokens t = themeTokens(palette);
    return QString(R"CSS(
        QWidget { font-size: 13px; }
        QMainWindow, QWidget#centralWidget { background: palette(window); }
        QScrollArea#videoSetAssistantScrollArea QWidget#qt_scrollarea_viewport,
        QWidget#videoSetAssistantWelcomePage { background: %22; }
        QFrame#applicationHeader {
            background: %1; border: 0; border-bottom: 1px solid %13;
        }
        QLabel[brand="true"] { font-size: 25px; font-weight: 750; color: %5; }
        QLabel[eyebrow="true"] {
            color: %8; font-size: 11px; font-weight: 700; letter-spacing: 1px;
        }
        QLabel[pageTitle="true"] { font-size: 22px; font-weight: 730; color: %5; }
        QLabel[sectionTitle="true"] { font-size: 17px; font-weight: 700; color: %5; }
        QLabel[cardTitle="true"] { font-size: 16px; font-weight: 700; color: %5; }
        QLabel[metricValue="true"] { font-size: 20px; font-weight: 750; color: %5; }
        QLabel[muted="true"] { color: %7; }
        QFrame#onboardingCard { border-radius: 12px; }
        QLabel[onboardingSteps="true"] {
            color: %6; background: %1; border: 1px solid %13;
            border-radius: 7px; padding: 9px 12px;
        }
        QLabel[technical="true"], QTextEdit[technical="true"] {
            font-family: Consolas, "Cascadia Mono", monospace; font-size: 12px;
        }
        QFrame[vsxSurface="raised"], QGroupBox[vsxRole="section"],
        QFrame[vsxRole="actionCard"], QFrame[vsxRole="section"],
        QGroupBox[vsxRole="profileCard"] {
            background: %2; border: 1px solid %13; border-radius: 10px;
        }
        QFrame[vsxSurface="raised"] QLabel,
        QFrame[vsxSurface="raised"] QCheckBox { color: %5; }
        QFrame[vsxRole="actionCard"], QFrame[vsxRole="section"],
        QGroupBox[vsxRole="section"],
        QGroupBox[vsxRole="profileCard"] {
            font-size: 16px; font-weight: 700;
        }
        QGroupBox[vsxRole="actionCard"]::title,
        QGroupBox[vsxRole="section"]::title,
        QGroupBox[vsxRole="profileCard"]::title {
            subcontrol-origin: margin; left: 16px; padding: 0 4px;
        }
        QGroupBox[vsxRole="profileCard"][selected="true"] {
            background: %4; border: 2px solid %8;
        }
        QFrame#videoSetRecentGroup { background: %23; }
        QFrame#videoSetTrustStrip { background: %24; border-color: %18; }
        QListWidget#videoSetRecentList {
            background: transparent; border: 0; border-radius: 0;
        }
        QListWidget#videoSetRecentList::item {
            background: %2; border: 0; border-radius: 8px; margin-bottom: 6px;
            padding: 0;
        }
        QListWidget#videoSetRecentList::item:hover { background: %3; }
        QListWidget#videoSetRecentList::item:selected { background: %4; color: %5; }
        QLabel[recentTitle="true"] { font-size: 14px; font-weight: 700; color: %5; }
        QLabel[recentMeta="true"] { font-size: 12px; color: %7; }
        QLabel[vsxRole="dropZone"] {
            background: %1; border: 2px dashed %14; border-radius: 10px;
            padding: 18px; color: %6;
        }
        QPushButton, QToolButton {
            min-height: 30px; padding: 2px 12px; border-radius: 6px;
            border: 1px solid %13; background: %2; color: %5;
        }
        QPushButton:hover, QToolButton:hover { background: %3; border-color: %14; }
        QPushButton:pressed, QToolButton:pressed { background: %4; }
        QPushButton:focus, QToolButton:focus, QComboBox:focus,
        QLineEdit:focus, QListWidget:focus, QRadioButton:focus {
            border: 2px solid %8;
        }
        QPushButton[vsxRole="primary"] {
            min-height: 36px; padding: 2px 17px; background: %8;
            border: 1px solid %8; color: %11; font-weight: 700;
        }
        QPushButton[vsxRole="primary"]:hover { background: %9; border-color: %9; }
        QPushButton[vsxRole="primary"]:pressed { background: %10; border-color: %10; }
        QPushButton[vsxRole="secondary"] { background: %2; border-color: %14; }
        QPushButton[vsxRole="ghost"], QToolButton[vsxRole="ghost"] {
            background: transparent; border-color: transparent; color: %6;
        }
        QPushButton[vsxRole="danger"] { color: %17; border-color: %17; background: transparent; }
        QPushButton:disabled, QToolButton:disabled {
            color: %7; background: %1; border-color: %13;
        }
        QPushButton[nav="true"], QToolButton[nav="true"] {
            min-height: 34px; background: transparent; border: 0;
            border-bottom: 3px solid transparent; border-radius: 0;
            padding: 1px 11px; color: %6; font-weight: 600;
        }
        QPushButton[nav="true"]:hover, QToolButton[nav="true"]:hover {
            background: %3; color: %5;
        }
        QPushButton[nav="true"][selected="true"],
        QToolButton[nav="true"][selected="true"] {
            background: %4; color: %5; border-bottom-color: %8;
        }
        QLabel[vsxRole="badge"], QLabel[vsxState="success"],
        QLabel[vsxState="warning"], QLabel[vsxState="error"],
        QLabel[vsxState="info"] {
            border-radius: 9px; padding: 3px 8px; font-size: 11px; font-weight: 700;
        }
        QLabel[vsxRole="badge"] { background: %4; color: %8; }
        QLabel[vsxState="success"] { color: %15; background: %18; }
        QLabel[vsxState="warning"] { color: %16; background: %19; }
        QLabel[vsxState="error"] { color: %17; background: %20; }
        QLabel[vsxState="info"] { color: %12; background: %21; }
        QFrame[metricCard="true"] {
            background: %1; border: 1px solid %13; border-radius: 8px;
        }
        QFrame[metricCard="true"][vsxState="success"] { border-left: 3px solid %15; }
        QFrame[metricCard="true"][vsxState="warning"] { border-left: 3px solid %16; }
        QFrame[metricCard="true"][vsxState="error"] { border-left: 3px solid %17; }
        QListWidget, QTableWidget, QLineEdit, QComboBox, QSpinBox, QDoubleSpinBox,
        QTextEdit {
            background: %1; border: 1px solid %13; border-radius: 6px;
            color: %5; selection-background-color: %4; selection-color: %5;
        }
        QLineEdit, QComboBox, QSpinBox, QDoubleSpinBox { min-height: 32px; padding: 1px 7px; }
        QListWidget::item { padding: 10px 8px; border-bottom: 1px solid %13; }
        QListWidget::item:hover { background: %3; }
        QListWidget::item:selected { background: %4; color: %5; }
        QProgressBar { min-height: 12px; border: 0; border-radius: 6px; background: %3; text-align: center; }
        QProgressBar::chunk { border-radius: 6px; background: %8; }
        QScrollArea { background: transparent; }
        QScrollArea > QWidget > QWidget { background: transparent; }
        QScrollArea#videoSetAssistantScrollArea QScrollBar:vertical {
            width: 12px; background: transparent; margin: 2px;
        }
        QScrollArea#videoSetAssistantScrollArea QScrollBar::handle:vertical {
            background: %14; min-height: 28px; border-radius: 5px;
        }
        QScrollArea#videoSetAssistantScrollArea QScrollBar::add-line:vertical,
        QScrollArea#videoSetAssistantScrollArea QScrollBar::sub-line:vertical {
            height: 0; background: transparent;
        }
        QMenu { background: %2; border: 1px solid %13; padding: 5px; }
        QMenu::item { padding: 7px 24px 7px 10px; border-radius: 4px; }
        QMenu::item:selected { background: %4; color: %5; }
    )CSS")
        .arg(css(t.surfaceBase), css(t.surfaceRaised), css(t.surfaceHover),
             css(t.surfaceSelected), css(t.textPrimary), css(t.textSecondary),
             css(t.textMuted), css(t.accent), css(t.accentHover),
             css(t.accentPressed), css(t.onAccent), css(t.info),
             css(t.border), css(t.borderStrong), css(t.success),
             css(t.warning), css(t.error),
             css(mix(t.surfaceBase, t.success, t.dark ? 0.13 : 0.08)),
             css(mix(t.surfaceBase, t.warning, t.dark ? 0.13 : 0.08)),
             css(mix(t.surfaceBase, t.error, t.dark ? 0.13 : 0.08)),
             css(mix(t.surfaceBase, t.info, t.dark ? 0.13 : 0.08)),
             css(t.surfacePage), css(t.surfaceRecent), css(t.surfaceTrust));
}

void applyTheme(QWidget *root) {
    if (!root) return;
    root->setStyleSheet(applicationStyleSheet(root->palette()));
}

} // namespace vidstorex_ui
