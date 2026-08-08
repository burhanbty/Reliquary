#include "ui_theme.h"
#include "visual_components.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QFile>
#include <QImage>
#include <QLabel>
#include <QPainter>
#include <QPalette>
#include <QSet>

#include <algorithm>
#include <cmath>

namespace {

double relative_luminance(const QColor &color) {
    const auto channel = [](const int value) {
        const double normalized = value / 255.0;
        return normalized <= 0.04045
            ? normalized / 12.92
            : std::pow((normalized + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * channel(color.red()) +
           0.7152 * channel(color.green()) +
           0.0722 * channel(color.blue());
}

double contrast(const QColor &first, const QColor &second) {
    const double light = std::max(relative_luminance(first),
                                  relative_luminance(second));
    const double dark = std::min(relative_luminance(first),
                                 relative_luminance(second));
    return (light + 0.05) / (dark + 0.05);
}

QImage render(QWidget &widget, const QSize size) {
    widget.resize(size);
    widget.show();
    QApplication::processEvents();
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    widget.render(&painter);
    return image;
}

int unique_opaque_colors(const QImage &image) {
    QSet<QRgb> colors;
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x)
            if (qAlpha(image.pixel(x, y)) != 0)
                colors.insert(image.pixel(x, y));
    return colors.size();
}

} // namespace

TEST(UiVisualIdentity, LightPaletteHasReadablePrimaryAction) {
    QPalette palette;
    palette.setColor(QPalette::Window, QColor("#F4F1EB"));
    palette.setColor(QPalette::Base, QColor("#FFFDF8"));
    palette.setColor(QPalette::WindowText, QColor("#24211D"));
    const auto tokens = vidstorex_ui::themeTokens(palette);
    EXPECT_FALSE(tokens.dark);
    EXPECT_GE(contrast(tokens.accent, tokens.onAccent), 4.5);
    EXPECT_GE(contrast(tokens.textPrimary, tokens.surfaceRaised), 7.0);
}

TEST(UiVisualIdentity, DarkPaletteHasReadablePrimaryAction) {
    QPalette palette;
    palette.setColor(QPalette::Window, QColor("#20201E"));
    palette.setColor(QPalette::Base, QColor("#181816"));
    palette.setColor(QPalette::WindowText, QColor("#F1EEE7"));
    const auto tokens = vidstorex_ui::themeTokens(palette);
    EXPECT_TRUE(tokens.dark);
    EXPECT_GE(contrast(tokens.accent, tokens.onAccent), 4.5);
    EXPECT_GE(contrast(tokens.textPrimary, tokens.surfaceRaised), 7.0);
}

TEST(UiVisualIdentity, StyleSheetUsesSemanticRolesAndStates) {
    const QString style = vidstorex_ui::applicationStyleSheet(QPalette());
    EXPECT_TRUE(style.contains("vsxRole=\"primary\""));
    EXPECT_TRUE(style.contains("vsxRole=\"secondary\""));
    EXPECT_TRUE(style.contains("vsxRole=\"ghost\""));
    EXPECT_TRUE(style.contains("vsxState=\"success\""));
    EXPECT_TRUE(style.contains(":disabled"));
}

TEST(UiVisualIdentity, SignalRailRendersRealPaletteAwareBlocks) {
    VidStoreXSignalRail rail;
    const QImage image = render(rail, {320, 10});
    EXPECT_GT(unique_opaque_colors(image), 3);
    EXPECT_TRUE(rail.accessibleName().isEmpty());
    EXPECT_EQ(rail.focusPolicy(), Qt::NoFocus);
}

TEST(UiVisualIdentity, SignalRailHandlesTinyAndZeroGeometry) {
    VidStoreXSignalRail rail;
    EXPECT_NO_FATAL_FAILURE(render(rail, {1, 1}));
    rail.resize(0, 0);
    rail.update();
    QApplication::processEvents();
    EXPECT_EQ(rail.width(), 0);
    EXPECT_GE(rail.height(), rail.minimumHeight());
}

TEST(UiVisualIdentity, StepperExposesFiveGroupedStates) {
    VidStoreXStepper stepper;
    const QStringList steps{"File", "Mode", "Create Videos", "YouTube", "Done"};
    stepper.setSteps(steps, 2);
    const QImage image = render(stepper, {720, 58});
    EXPECT_EQ(stepper.steps(), steps);
    EXPECT_EQ(stepper.activeStep(), 2);
    EXPECT_GT(unique_opaque_colors(image), 4);
    EXPECT_FALSE(stepper.accessibleDescription().isEmpty());
}

TEST(UiVisualIdentity, TurkishStepperFitsMinimumSupportedWidth) {
    VidStoreXStepper stepper;
    stepper.setSteps({QString::fromUtf8("Dosya"), QString::fromUtf8("Mod"),
                      QString::fromUtf8("Videoları Oluştur"),
                      QStringLiteral("YouTube"), QString::fromUtf8("Bitti")}, 3);
    const QImage image = render(stepper, {640, 58});
    EXPECT_EQ(stepper.steps().size(), 5);
    EXPECT_GT(unique_opaque_colors(image), 4);
    EXPECT_LE(stepper.minimumSizeHint().width(), 640);
}

TEST(UiVisualIdentity, DataGlyphModesRenderWithoutExternalAssets) {
    for (const auto mode : {VidStoreXDataGlyph::Mode::FileToBlocks,
                            VidStoreXDataGlyph::Mode::BlocksToFile,
                            VidStoreXDataGlyph::Mode::Verified,
                            VidStoreXDataGlyph::Mode::Missing,
                            VidStoreXDataGlyph::Mode::Empty}) {
        VidStoreXDataGlyph glyph(mode);
        const QImage image = render(glyph, {68, 48});
        EXPECT_GE(unique_opaque_colors(image), 2);
        EXPECT_TRUE(glyph.accessibleName().isEmpty());
    }
}

TEST(UiVisualIdentity, RecentEntryLayoutKeepsTextAndStatusSeparate) {
    for (const qreal fontScale : {1.0, 1.25, 1.5}) {
        VidStoreXRecentEntry entry(
            QStringLiteral("ceng113-with-a-long-safe-archive-name.zip"),
            QString::fromUtf8("2 parça · Son açılma: 7 Ağu 12:16"),
            QString::fromUtf8("YouTube için hazır"), "success");
        QFont font = entry.font();
        font.setPointSizeF(font.pointSizeF() * fontScale);
        entry.setFont(font);
        entry.setStyleSheet(
            vidstorex_ui::applicationStyleSheet(entry.palette()));
        const QImage image = render(entry, {760, entry.sizeHint().height()});
        EXPECT_FALSE(image.isNull());
        EXPECT_LT(entry.titleLabel()->geometry().bottom(),
                  entry.metadataLabel()->geometry().top());
        EXPECT_FALSE(entry.titleLabel()->geometry().intersects(
            entry.statusLabel()->geometry()));
        EXPECT_FALSE(entry.metadataLabel()->geometry().intersects(
            entry.statusLabel()->geometry()));
        EXPECT_GE(entry.height(), 58);
        EXPECT_LE(entry.height(), 110);
    }
}

TEST(UiVisualIdentity, SurfaceHierarchyHasDistinctSemanticLevels) {
    QPalette palette;
    palette.setColor(QPalette::Window, QColor("#20201E"));
    palette.setColor(QPalette::Base, QColor("#181816"));
    palette.setColor(QPalette::WindowText, QColor("#F1EEE7"));
    const auto tokens = vidstorex_ui::themeTokens(palette);
    EXPECT_NE(tokens.surfaceBase, tokens.surfacePage);
    EXPECT_NE(tokens.surfacePage, tokens.surfaceRaised);
    EXPECT_NE(tokens.surfaceRaised, tokens.surfaceRecent);
    EXPECT_NE(tokens.surfaceTrust, tokens.surfacePage);
}

TEST(UiVisualIdentity, LayoutTokensKeepHomeAndRecentBounded) {
    EXPECT_GE(vidstorex_ui::Layout::ContentMaxWidth, 1200);
    EXPECT_LE(vidstorex_ui::Layout::ContentMaxWidth, 1500);
    EXPECT_EQ(vidstorex_ui::Layout::RecentVisibleRows, 4);
    EXPECT_GE(vidstorex_ui::Layout::HeroPadding, 18);
}

TEST(UiVisualIdentity, GuiSourceDeclaresIdentityAndRecentPrivacyContract) {
    QFile file(QStringLiteral(VIDSTOREX_SOURCE_DIR) +
               "/src/drive_manager_ui.cpp");
    ASSERT_TRUE(file.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString source = QString::fromUtf8(file.readAll());
    EXPECT_TRUE(source.contains("Store your files safely in videos"));
    EXPECT_TRUE(source.contains("videoSetSuccessSignalRail"));
    EXPECT_TRUE(source.contains("Advanced / Classic Video Set Tools"));
    EXPECT_TRUE(source.contains("Unavailable Video Set"));
    EXPECT_TRUE(source.contains("new VidStoreXRecentEntry"));
    EXPECT_TRUE(source.contains("Copy manifest location"));
    EXPECT_TRUE(source.contains("setFixedHeight(listHeight)"));
    EXPECT_FALSE(source.contains("new QGroupBox(\"Create a Video Set\")"));
    EXPECT_FALSE(source.contains(
        "display = manifest.absolutePath() +"));
}

TEST(UiVisualIdentity, AppIconIsGeneratedFromRepositoryOwnedGeometry) {
    QFile file(QStringLiteral(VIDSTOREX_SOURCE_DIR) + "/src/main_gui.cpp");
    ASSERT_TRUE(file.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString source = QString::fromUtf8(file.readAll());
    EXPECT_TRUE(source.contains("vidStoreXApplicationIcon"));
    EXPECT_TRUE(source.contains("{16, 24, 32, 48, 256}"));
    EXPECT_TRUE(source.contains("app.setWindowIcon"));
}

TEST(UiVisualIdentity, ThemeSourceAvoidsDisallowedVisualTrends) {
    QFile file(QStringLiteral(VIDSTOREX_SOURCE_DIR) + "/src/ui_theme.cpp");
    ASSERT_TRUE(file.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString source = QString::fromUtf8(file.readAll()).toLower();
    EXPECT_FALSE(source.contains("gradient"));
    EXPECT_FALSE(source.contains("blur"));
    EXPECT_FALSE(source.contains("rgba("));
    EXPECT_FALSE(source.contains("animation"));
}
