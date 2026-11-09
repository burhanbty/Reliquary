#include "ui_theme.h"

#include <QWidget>
#include <QPainter>
#include <QPainterPath>

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

ResponsiveMode responsiveModeForViewport(const int width, const int height) {
    if (width < 1320 || height < 740) return ResponsiveMode::Compact;
    if (width >= 1800 && height >= 900) return ResponsiveMode::Wide;
    return ResponsiveMode::Normal;
}

HeightDensity heightDensityForViewport(const int, const int height) {
    return height < 800 ? HeightDensity::Short : HeightDensity::Regular;
}

DensityMetrics densityMetrics(const ResponsiveMode mode) {
    switch (mode) {
        case ResponsiveMode::Compact:
            return {14, Layout::ContentMaxWidth, Layout::WorkflowMaxWidth,
                    10, 14, 10,
                    32, 34, 8, 34, 52, 150};
        case ResponsiveMode::Wide:
            return {24, 1480, Layout::WorkflowMaxWidth,
                    14, 18, 12,
                    34, 36, 10, 36, 56, 210};
        case ResponsiveMode::Normal:
            return {18, Layout::ContentMaxWidth, Layout::WorkflowMaxWidth,
                    12, 16, 10,
                    34, 36, 9, 36, 56, 190};
    }
    return densityMetrics(ResponsiveMode::Normal);
}

QString responsiveModeName(const ResponsiveMode mode) {
    switch (mode) {
        case ResponsiveMode::Compact: return QStringLiteral("compact");
        case ResponsiveMode::Wide: return QStringLiteral("wide");
        case ResponsiveMode::Normal: return QStringLiteral("normal");
    }
    return QStringLiteral("normal");
}

ThemeTokens themeTokens(const QPalette &palette) {
    ThemeTokens result;
    const QColor base = palette.color(QPalette::Base);
    // Qt stylesheets give transparent surfaces a transparent black Window brush.
    // The opaque Base still identifies the inherited theme in that case.
    const QColor window = palette.color(QPalette::Window).alpha() == 0
        ? base : palette.color(QPalette::Window);
    result.dark = window.lightness() < 128;
    const QColor text = palette.color(QPalette::WindowText);
    if (result.dark) {
        result.surfaceBase = QColor("#000000");
        result.surfacePage = QColor("#030708");
        result.surfaceRaised = QColor("#10191D");
        result.surfaceRecent = QColor("#080E11");
        result.surfaceHover = QColor("#192B31");
        result.surfaceSelected = QColor("#17333A");
        result.textPrimary = QColor("#E6ECEE");
        result.textSecondary = QColor("#BDCBD0");
        result.textMuted = QColor("#9BAEB5");
        result.accent = QColor("#64D3DC");
        result.accentHover = QColor("#A8EDF1");
        result.accentPressed = QColor("#49B7C1");
        result.onAccent = QColor("#062027");
        result.success = QColor("#8AD7B0");
        result.warning = QColor("#E8BC75");
        result.error = QColor("#F08D92");
        result.info = result.textSecondary;
        result.surfaceTrust = QColor("#0C191C");
        result.border = QColor("#293B43");
        result.borderStrong = QColor("#667E88");
        return result;
    }

    const QColor anchor("#09636F");
    result.surfaceBase = mix(base, window, 0.09);
    result.surfacePage = mix(window, base, 0.34);
    result.surfaceRaised = mix(base, QColor("#EDF8FA"), 0.22);
    result.surfaceRecent = mix(result.surfaceRaised, window, 0.28);
    result.surfaceHover = mix(result.surfaceRaised, anchor, 0.055);
    result.surfaceSelected = mix(result.surfaceRaised, anchor, 0.11);
    result.textPrimary = text;
    result.textSecondary = mix(text, window, 0.34);
    result.textMuted = mix(text, window, 0.31);
    result.accent = anchor;
    result.accentHover = anchor.darker(108);
    result.accentPressed = anchor.darker(122);
    result.onAccent = QColor("#FFFFFF");
    result.success = QColor("#237A4B");
    result.warning = QColor("#9A5B05");
    result.error = QColor("#A63D39");
    result.info = QColor("#36778C");
    result.surfaceTrust = mix(result.surfacePage, result.success, 0.07);
    result.border = mix(text, window, 0.82);
    result.borderStrong = mix(text, window, 0.65);
    return result;
}

QPalette obsidianPalette(const QPalette &base) {
    QPalette palette(base);
    palette.setColor(QPalette::Window, QColor("#000000"));
    palette.setColor(QPalette::Base, QColor("#030708"));
    palette.setColor(QPalette::AlternateBase, QColor("#10191D"));
    palette.setColor(QPalette::Button, QColor("#10191D"));
    for (const auto role : {QPalette::WindowText, QPalette::Text,
                            QPalette::ButtonText})
        palette.setColor(role, QColor("#E6ECEE"));
    palette.setColor(QPalette::Highlight, QColor("#17333A"));
    palette.setColor(QPalette::HighlightedText, QColor("#E6ECEE"));
    palette.setColor(QPalette::Link, QColor("#64D3DC"));
    palette.setColor(QPalette::PlaceholderText, QColor("#9BAEB5"));
    palette.setColor(QPalette::ToolTipBase, QColor("#10191D"));
    palette.setColor(QPalette::ToolTipText, QColor("#E6ECEE"));
    for (const auto role : {QPalette::WindowText, QPalette::Text,
                            QPalette::ButtonText})
        palette.setColor(QPalette::Disabled, role, QColor("#80949D"));
    return palette;
}

QIcon navigationIcon(const NavigationSymbol symbol, const QPalette &palette) {
    QPixmap image(40, 40);
    image.setDevicePixelRatio(2);
    image.fill(Qt::transparent);
    QPainter p(&image);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(themeTokens(palette).textSecondary, 1.35,
                  Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    switch (symbol) {
        case NavigationSymbol::Home: {
            QPainterPath house;
            house.moveTo(3, 9); house.lineTo(10, 3); house.lineTo(17, 9);
            house.moveTo(5, 8); house.lineTo(5, 17); house.lineTo(15, 17);
            house.lineTo(15, 8);
            p.drawPath(house);
            p.drawRect(QRectF(8.5, 12, 3, 5));
            break;
        }
        case NavigationSymbol::Create:
            p.drawRoundedRect(QRectF(4, 3, 12, 14), 2, 2);
            p.drawLine(QPointF(10, 7), QPointF(10, 13));
            p.drawLine(QPointF(7, 10), QPointF(13, 10));
            break;
        case NavigationSymbol::Recover: {
            p.drawArc(QRectF(4, 4, 12, 12), 30 * 16, 295 * 16);
            QPainterPath arrow;
            arrow.moveTo(3, 3); arrow.lineTo(3, 8); arrow.lineTo(8, 8);
            p.drawPath(arrow);
            break;
        }
        case NavigationSymbol::Recent:
            p.drawRoundedRect(QRectF(3, 4, 14, 12), 2, 2);
            for (const int y : {7, 10, 13})
                p.drawLine(QPointF(6, y), QPointF(14, y));
            break;
        case NavigationSymbol::Settings:
            p.drawEllipse(QPointF(10, 10), 5, 5);
            p.drawEllipse(QPointF(10, 10), 1.5, 1.5);
            for (int angle = 0; angle < 360; angle += 45) {
                p.save(); p.translate(10, 10); p.rotate(angle);
                p.drawLine(QPointF(0, 5), QPointF(0, 7)); p.restore();
            }
            break;
    }
    p.end();
    return QIcon(image);
}

QString applicationStyleSheet(const QPalette &palette) {
    const ThemeTokens t = themeTokens(palette);
    return QString(R"CSS(
        QWidget { font-size: 13px; }
        QMainWindow { background: %1; color: %5; }
        QWidget#centralWidget { background: transparent; color: %5; }
        QMenuBar, QMenu { background: %1; color: %5; }
        QMenuBar::item:selected, QMenu::item:selected { background: %4; color: %5; }
        QMenu::item:disabled { color: %7; }
        QScrollArea#videoSetAssistantScrollArea QWidget#qt_scrollarea_viewport,
        QStackedWidget#videoSetAssistantStack,
        QWidget#videoSetAssistantWelcomePage,
        QWidget#videoSetAssistantSourcePage,
        QWidget#videoSetAssistantModePage,
        QWidget#videoSetAssistantPlanPage,
        QWidget#videoSetAssistantProgressPage,
        QWidget#videoSetAssistantUploadPage,
        QWidget#videoSetAssistantDownloadPage,
        QWidget#videoSetAssistantRecoverPage,
        QWidget#videoSetAssistantRecoveryProgressPage,
        QWidget#videoSetAssistantDonePage,
        QWidget#videoSetRecentPage,
        QWidget#settingsPage,
        QWidget#onboardingPage,
        QWidget#classicToolsContent,
        QWidget#testLabContent,
        QWidget#capacityLabContent,
        QWidget#advancedLandingContent,
        QWidget#experimentalYouTubeSyncContent {
            background: transparent;
        }
        QTabWidget#applicationPages::pane { border: 0; margin: 0; }
        QTabWidget#applicationPages > QStackedWidget,
        QWidget#videoSetPage, QFrame#videoSetWorkflowArea,
        QWidget#videoSetWorkflowMain, QStackedWidget#videoSetWizardActionStack {
            background: transparent;
        }
        QFrame#applicationHeader {
            background: transparent; border: 0; border-bottom: 1px solid %13;
        }
        QLabel { background: transparent; color: %5; }
        QLabel[brand="true"] { font-size: 23px; font-weight: 600; color: %5; }
        QLabel[eyebrow="true"] {
            color: %8; font-size: 11px; font-weight: 700; letter-spacing: 1px;
        }
        QLabel[pageTitle="true"] { font-size: 23px; font-weight: 600; color: %5; }
        QLabel[sectionTitle="true"] { font-size: 17px; font-weight: 700; color: %5; }
        QLabel[cardTitle="true"] { font-size: 16px; font-weight: 700; color: %5; }
        QWidget[workflowPage="true"] QLabel[pageTitle="true"] {
            font-size: 18px;
        }
        QWidget[workflowPage="true"] QLabel[sectionTitle="true"],
        QFrame#videoSetActivityPanel QLabel[cardTitle="true"] {
            font-size: 14px;
        }
        QLabel#videoSetActivityDescription,
        QLabel#videoSetProcessingSummary,
        QLabel#videoSetBlockProgressLabel { font-size: 12px; }
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
            background: qlineargradient(x1:0, y1:0, x2:1, y2:1,
                stop:0 %2, stop:1 %22);
            border: 1px solid %13; border-radius: 14px;
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
            subcontrol-origin: margin; left: 16px; padding: 0 4px; color: %5;
        }
        QGroupBox[vsxRole="section"] { margin-top: 12px; padding-top: 5px; }
        QGroupBox[vsxRole="profileCard"] QRadioButton {
            color: %5; background: transparent; border: 0; padding: 0;
        }
        QGroupBox[vsxRole="profileCard"] {
            background: %2; border-color: %13;
        }
        QGroupBox[vsxRole="profileCard"][selected="true"] {
            background: %4; border: 2px solid %8;
        }
        QFrame[vsxRole="actionCard"] { border-color: %14; }
        QFrame#videoSetRecentGroup { background: %23; border-color: %14; }
        QFrame#videoSetRecentFullGroup { background: %23; border-color: %14; }
        QFrame#videoSetWizardActionBar {
            background: transparent; border: 0;
            border-radius: 18px;
        }
        QFrame#videoSetTrustStrip {
            background: transparent; border: 0;
        }
        QListWidget#videoSetRecentList,
        QListWidget#videoSetRecentFullList {
            background: transparent; border: 0; border-radius: 0;
        }
        QListWidget#videoSetRecentList::item,
        QListWidget#videoSetRecentFullList::item {
            background: transparent; border: 1px solid transparent; border-radius: 10px;
            margin-bottom: 6px;
            padding: 0;
        }
        QListWidget#videoSetRecentList::item:hover,
        QListWidget#videoSetRecentFullList::item:hover { background: %3; }
        QListWidget#videoSetRecentList::item:selected,
        QListWidget#videoSetRecentFullList::item:selected {
            background: %4; color: %5; border: 1px solid %13;
        }
        QListWidget#videoSetRecentList QFrame,
        QListWidget#videoSetRecentFullList QFrame {
            background: transparent; border: 0;
        }
        QLabel[recentTitle="true"] { font-size: 14px; font-weight: 700; color: %5; }
        QLabel[recentMeta="true"] { font-size: 12px; color: %7; }
        QLabel[vsxRole="dropZone"] {
            background: %1; border: 2px dashed %8; border-radius: 10px;
            padding: 18px; color: %6;
        }
        QLabel#videoSetAssistantSourceInfo,
        QLabel#videoSetAssistantPlanSummary {
            background: %2; border: 1px solid %13;
            border-left: 3px solid %8; border-radius: 8px;
            padding: 10px 12px; color: %6;
        }
        QWidget#videoSetAssistantStepper {
            background: transparent; border: 0;
        }
        QPushButton, QToolButton {
            min-height: 30px; padding: 2px 12px; border-radius: 9px;
            border: 1px solid %13; background: %2; color: %5;
        }
        QPushButton:hover, QToolButton:hover { background: %3; border-color: %14; }
        QPushButton:pressed, QToolButton:pressed { background: %4; }
        QPushButton:focus, QToolButton:focus, QComboBox:focus,
        QLineEdit:focus, QListWidget:focus {
            border: 2px solid %8;
        }
        QRadioButton::indicator {
            width: 13px; height: 13px; background: %1;
            border: 1px solid %14; border-radius: 7px;
        }
        QRadioButton::indicator:hover { border: 2px solid %8; }
        QRadioButton::indicator:checked {
            background: %1; border: 4px solid %8;
        }
        QRadioButton::indicator:disabled {
            background: %1; border-color: %13;
        }
        QPushButton[vsxRole="primary"] {
            min-height: 36px; padding: 2px 17px;
            background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 %9, stop:1 %8);
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
            border-bottom: 1px solid transparent; border-radius: 8px;
            padding: 1px 11px; color: %6; font-weight: 600;
        }
        QToolButton#advancedNavigationButton::menu-indicator {
            image: none; width: 0px;
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
        QLabel#videoSetTrustLabel {
            color: %8; background: transparent; border: 0;
            border-radius: 0; padding: 0; font-weight: 700;
        }
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
        QLineEdit { placeholder-text-color: %7; }
        QListWidget::item { padding: 10px 8px; border-bottom: 1px solid %13; }
        QListWidget::item:hover { background: %3; }
        QListWidget::item:selected { background: %4; color: %5; }
        QTableWidget#videoSetAssistantPlanTable QHeaderView::section {
            background: %2; color: %6; border: 0;
            border-right: 1px solid %13; border-bottom: 1px solid %14;
            padding: 7px;
        }
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
        QFrame[glassSurface="true"] { background: transparent; border: 0; }
        QWidget#videoSetCollectionDetail { background: transparent; }
        QLabel#videoSetArchiveName { font-size: 20px; font-weight: 600; color: %5; }
        QLabel#videoSetArchiveMetadata { color: %7; font-size: 12px; }
        QFrame#videoSetCreateCard, QFrame#videoSetRecoverCard {
            background: transparent; border: 0;
            border-radius: 0;
        }
        QFrame#videoSetCreateCard QLabel[cardTitle="true"],
        QFrame#videoSetRecoverCard QLabel[cardTitle="true"] { font-size: 14px; font-weight: 600; }
        QFrame#videoSetCreateCard QLabel[muted="true"],
        QFrame#videoSetRecoverCard QLabel[muted="true"] { font-size: 12px; }
        QFrame#videoSetRecentEmptyState { background: transparent; border: 0; }
        QToolTip { background: %2; color: %5; border: 1px solid %14; padding: 6px; }
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
             css(t.surfacePage), css(t.surfaceRecent));
}

void applyTheme(QWidget *root) {
    if (!root) return;
    root->setStyleSheet(applicationStyleSheet(root->palette()));
}

} // namespace vidstorex_ui
