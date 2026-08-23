#include "ui_theme.h"
#include "visual_components.h"
#include "app_branding.h"
#include "brand_intro.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QFile>
#include <QImage>
#include <QLabel>
#include <QPainter>
#include <QPalette>
#include <QKeyEvent>
#include <QPointer>
#include <QSettings>
#include <QSet>
#include <QTemporaryDir>
#include <QTimer>

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

TEST(ProductBranding, CanonicalReliquaryIdentityAndAttributionAreStable) {
    EXPECT_STREQ(vidstorex::branding::kProductName, "Reliquary");
    EXPECT_STREQ(vidstorex::branding::kAuthorName,
                 "Burhan Talha Yazıcı");
    EXPECT_STREQ(vidstorex::branding::kAuthorAlias, "BTY");
    EXPECT_STREQ(vidstorex::branding::kLinkedInUrl,
                 "https://www.linkedin.com/in/burhanbty");
    EXPECT_STREQ(vidstorex::branding::kDefinitionEnglish,
                 "A container for preserving something precious.");
    EXPECT_STREQ(vidstorex::branding::kDefinitionTurkish,
                 "Değerli bir şeyi korumak için kullanılan muhafaza.");
    EXPECT_EQ(vidstorex::branding::kBrandIntroVersion, 1);
}

TEST(ProductBranding, SettingsMigrationCopiesOnlyMissingValuesIdempotently) {
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    QSettings legacy(temporary.filePath("legacy.ini"), QSettings::IniFormat);
    QSettings target(temporary.filePath("reliquary.ini"),
                     QSettings::IniFormat);
    legacy.setValue("ui/language", "tr");
    legacy.setValue("ui/onboardingVersion", 1);
    legacy.setValue("videoSet/recentManifests",
                    QStringList{"C:/legacy/set_manifest.json"});
    legacy.setValue("encoding/reliabilityProfileId", 1);
    target.setValue("ui/language", "en");
    legacy.sync();
    target.sync();

    EXPECT_EQ(vidstorex::branding::copyMissingSettings(legacy, target), 3);
    EXPECT_EQ(target.value("ui/language").toString(), "en");
    EXPECT_EQ(target.value("ui/onboardingVersion").toInt(), 1);
    EXPECT_EQ(target.value("videoSet/recentManifests").toStringList(),
              QStringList{"C:/legacy/set_manifest.json"});
    EXPECT_EQ(target.value("encoding/reliabilityProfileId").toInt(), 1);
    EXPECT_EQ(vidstorex::branding::copyMissingSettings(legacy, target), 0);
}

TEST(ProductBranding, IntroContainsOnlyNameAndLocalizedMeaningAndCanSkip) {
    bool finished = false;
    QPointer<BrandIntroOverlay> intro = new BrandIntroOverlay(
        QString::fromLatin1(vidstorex::branding::kDefinitionEnglish));
    intro->setFinishedCallback([&finished]() { finished = true; });
    intro->resize(1280, 720);
    intro->show();
    QApplication::processEvents();
    auto *name = intro->findChild<QLabel *>("brandIntroName");
    auto *definition = intro->findChild<QLabel *>("brandIntroDefinition");
    ASSERT_NE(name, nullptr);
    ASSERT_NE(definition, nullptr);
    EXPECT_EQ(name->text(), "RELIQUARY");
    EXPECT_EQ(definition->text(),
              QString::fromLatin1(vidstorex::branding::kDefinitionEnglish));
    EXPECT_FALSE(intro->accessibleDescription().contains("data",
                                                         Qt::CaseInsensitive));
    EXPECT_GE(BrandIntroOverlay::totalDurationMs(), 3000);
    EXPECT_LE(BrandIntroOverlay::totalDurationMs(), 4000);
    QKeyEvent skip(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(intro, &skip);
    QApplication::processEvents();
    EXPECT_TRUE(finished);
}

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

TEST(UiVisualIdentity, OnboardingIllustrationsArePaletteAndDpiSafe) {
    for (const bool dark : {false, true}) {
        QPalette palette;
        palette.setColor(QPalette::Window,
                         dark ? QColor("#20201E") : QColor("#F4F1EB"));
        palette.setColor(QPalette::Base,
                         dark ? QColor("#181816") : QColor("#FFFDF8"));
        palette.setColor(QPalette::WindowText,
                         dark ? QColor("#F1EEE7") : QColor("#24211D"));
        for (const auto mode : {VidStoreXFlowIllustration::Mode::Create,
                                VidStoreXFlowIllustration::Mode::Store,
                                VidStoreXFlowIllustration::Mode::Recover}) {
            VidStoreXFlowIllustration illustration(mode);
            illustration.setPalette(palette);
            for (const QSize size : {QSize(420, 140), QSize(680, 190),
                                     QSize(1020, 285)}) {
                const QImage image = render(illustration, size);
                EXPECT_FALSE(image.isNull());
                EXPECT_GT(unique_opaque_colors(image), 4);
            }
            EXPECT_TRUE(illustration.accessibleName().isEmpty());
        }
    }
}

TEST(UiVisualIdentity, OnboardingProgressUsesThreePaintedStates) {
    VidStoreXOnboardingProgress progress;
    for (int page = 0; page < 3; ++page) {
        progress.setCurrentPage(page);
        const QImage image = render(progress, progress.sizeHint());
        EXPECT_EQ(progress.currentPage(), page);
        EXPECT_GE(unique_opaque_colors(image), 2);
        EXPECT_FALSE(progress.accessibleDescription().isEmpty());
    }
}

TEST(UiVisualIdentity, ProcessingPartGridUsesAdaptivePainterModes) {
    VidStoreXPartGrid grid;
    for (const auto &[count, expected] : {
             std::pair{0, VidStoreXPartGrid::DisplayMode::Individual},
             std::pair{1, VidStoreXPartGrid::DisplayMode::Individual},
             std::pair{12, VidStoreXPartGrid::DisplayMode::Individual},
             std::pair{13, VidStoreXPartGrid::DisplayMode::Compact},
             std::pair{40, VidStoreXPartGrid::DisplayMode::Compact},
             std::pair{41, VidStoreXPartGrid::DisplayMode::Aggregated},
             std::pair{100, VidStoreXPartGrid::DisplayMode::Aggregated},
             std::pair{500, VidStoreXPartGrid::DisplayMode::Aggregated}}) {
        grid.setParts(QVector<VidStoreXPartState>(count,
                                                  VidStoreXPartState::Pending));
        EXPECT_EQ(grid.displayMode(), expected);
        EXPECT_FALSE(render(grid, {640, 50}).isNull());
        EXPECT_TRUE(grid.findChildren<QWidget *>().isEmpty());
        EXPECT_TRUE(grid.findChildren<QTimer *>().isEmpty());
    }
}

TEST(UiVisualIdentity, ProcessingPartGridRendersEverySemanticState) {
    VidStoreXPartGrid grid;
    grid.setParts({VidStoreXPartState::Pending, VidStoreXPartState::Active,
                   VidStoreXPartState::Complete, VidStoreXPartState::Verified,
                   VidStoreXPartState::Missing, VidStoreXPartState::Corrupt,
                   VidStoreXPartState::Conflict, VidStoreXPartState::Failed,
                   VidStoreXPartState::Cancelled});
    const QImage image = render(grid, {640, 52});
    EXPECT_GT(unique_opaque_colors(image), 5);
    EXPECT_TRUE(grid.accessibleDescription().contains("need attention"));
}

TEST(UiVisualIdentity, BlockProgressSupportsHonestTerminalAndUnknownStates) {
    VidStoreXBlockProgress progress;
    for (const auto state : {VidStoreXBlockProgress::State::Determinate,
                             VidStoreXBlockProgress::State::Indeterminate,
                             VidStoreXBlockProgress::State::Success,
                             VidStoreXBlockProgress::State::Error,
                             VidStoreXBlockProgress::State::Paused}) {
        progress.setState(state);
        for (const quint64 value : {quint64{0}, quint64{1}, quint64{50},
                                    quint64{99}, quint64{100}}) {
            progress.setProgress(value, 100);
            EXPECT_FALSE(render(progress, {500, 20}).isNull());
        }
    }
    progress.setState(VidStoreXBlockProgress::State::Indeterminate);
    progress.setProgress(0, 0);
    EXPECT_TRUE(progress.findChildren<QTimer *>().isEmpty());
    EXPECT_FALSE(progress.accessibleDescription().isEmpty());
}

TEST(UiVisualIdentity, LiveDataPathCoversCreateDownloadScanRecoverAndVerify) {
    VidStoreXProcessingFlow flow;
    const QVector<VidStoreXPartState> parts{
        VidStoreXPartState::Verified, VidStoreXPartState::Active,
        VidStoreXPartState::Pending, VidStoreXPartState::Missing};
    flow.setParts(parts);
    for (const bool dark : {false, true}) {
        QPalette palette;
        palette.setColor(QPalette::Window,
                         dark ? QColor("#20201E") : QColor("#F4F1EB"));
        palette.setColor(QPalette::Base,
                         dark ? QColor("#181816") : QColor("#FFFDF8"));
        palette.setColor(QPalette::WindowText,
                         dark ? QColor("#F1EEE7") : QColor("#24211D"));
        flow.setPalette(palette);
        for (const auto mode : {VidStoreXProcessingFlow::Mode::Create,
                                VidStoreXProcessingFlow::Mode::Download,
                                VidStoreXProcessingFlow::Mode::Scan,
                                VidStoreXProcessingFlow::Mode::Recover,
                                VidStoreXProcessingFlow::Mode::Verify}) {
            flow.setMode(mode);
            flow.setFileProgress(50, 100,
                mode == VidStoreXProcessingFlow::Mode::Recover);
            for (const QSize size : {QSize(420, 78), QSize(620, 92),
                                     QSize(930, 138)}) {
                const QImage image = render(flow, size);
                EXPECT_GT(unique_opaque_colors(image), 4);
            }
        }
    }
    EXPECT_TRUE(flow.findChildren<QTimer *>().isEmpty());
    EXPECT_EQ(flow.mode(), VidStoreXProcessingFlow::Mode::Verify);
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

TEST(UiVisualIdentity, ResponsiveDensityModesAreSimpleOrderedAndBounded) {
    using vidstorex_ui::ResponsiveMode;
    EXPECT_EQ(vidstorex_ui::responsiveModeForViewport(1280, 720),
              ResponsiveMode::Compact);
    EXPECT_EQ(vidstorex_ui::responsiveModeForViewport(1366, 768),
              ResponsiveMode::Normal);
    EXPECT_EQ(vidstorex_ui::responsiveModeForViewport(1600, 900),
              ResponsiveMode::Normal);
    EXPECT_EQ(vidstorex_ui::responsiveModeForViewport(1920, 1080),
              ResponsiveMode::Wide);
    const auto compact = vidstorex_ui::densityMetrics(ResponsiveMode::Compact);
    const auto normal = vidstorex_ui::densityMetrics(ResponsiveMode::Normal);
    const auto wide = vidstorex_ui::densityMetrics(ResponsiveMode::Wide);
    EXPECT_LE(compact.pageMargin, normal.pageMargin);
    EXPECT_LE(normal.pageMargin, wide.pageMargin);
    EXPECT_LE(compact.cardPadding, normal.cardPadding);
    EXPECT_LE(normal.cardPadding, wide.cardPadding);
    EXPECT_GE(compact.buttonHeight, 32);
    EXPECT_LE(wide.pageMaxWidth, 1500);
    EXPECT_GE(wide.pageMaxWidth, 1300);
    EXPECT_LT(compact.tableMinHeight, wide.tableMinHeight);
}

TEST(UiVisualIdentity, ResponsiveHeightDensityTreatsLaptopHeightsAsShort) {
    using vidstorex_ui::HeightDensity;
    EXPECT_EQ(vidstorex_ui::heightDensityForViewport(1280, 720),
              HeightDensity::Short);
    EXPECT_EQ(vidstorex_ui::heightDensityForViewport(1366, 768),
              HeightDensity::Short);
    EXPECT_EQ(vidstorex_ui::heightDensityForViewport(1600, 900),
              HeightDensity::Regular);
    EXPECT_EQ(vidstorex_ui::heightDensityForViewport(1920, 1080),
              HeightDensity::Regular);
}

TEST(UiVisualIdentity, WorkflowStructureIsWiderThanReadableContent) {
    using vidstorex_ui::ResponsiveMode;
    for (const auto mode : {ResponsiveMode::Compact,
                            ResponsiveMode::Normal,
                            ResponsiveMode::Wide}) {
        const auto density = vidstorex_ui::densityMetrics(mode);
        EXPECT_GT(density.workflowMaxWidth, density.pageMaxWidth);
        EXPECT_GE(density.workflowMaxWidth, 1800);
        EXPECT_LE(density.workflowMaxWidth, 1920);
    }
}

TEST(UiVisualIdentity, WideDensityUsesSpaceWithoutUpscalingControls) {
    using vidstorex_ui::ResponsiveMode;
    const auto normal = vidstorex_ui::densityMetrics(ResponsiveMode::Normal);
    const auto wide = vidstorex_ui::densityMetrics(ResponsiveMode::Wide);
    EXPECT_LE(wide.controlHeight, normal.controlHeight);
    EXPECT_LE(wide.buttonHeight, normal.buttonHeight);
    EXPECT_LE(wide.navigationHeight, normal.navigationHeight);
    EXPECT_LE(wide.wizardActionHeight, normal.wizardActionHeight);
}

TEST(UiVisualIdentity, LiveDataPathHasCompactTerminalPresentation) {
    VidStoreXProcessingFlow flow;
    flow.setParts({VidStoreXPartState::Verified});
    const QSize normalHint = flow.sizeHint();
    const QImage normal = render(flow, normalHint);
    flow.setPresentationMode(
        VidStoreXProcessingFlow::PresentationMode::Compact);
    const QSize compactHint = flow.sizeHint();
    const QImage compact = render(flow, compactHint);
    EXPECT_LT(compactHint.height(), normalHint.height());
    EXPECT_GE(compactHint.height(), 58);
    EXPECT_GT(unique_opaque_colors(normal), 4);
    EXPECT_GT(unique_opaque_colors(compact), 4);
    EXPECT_EQ(flow.presentationMode(),
              VidStoreXProcessingFlow::PresentationMode::Compact);
}

TEST(UiVisualIdentity, StepperUsesCompactHeightWithoutClipping) {
    VidStoreXStepper stepper;
    stepper.setSteps({"File", "Mode", "Create", "YouTube", "Done"}, 1);
    stepper.setProperty("densityMode", "compact");
    stepper.setProperty("heightDensity", "short");
    const QImage compact = render(stepper, stepper.sizeHint());
    EXPECT_EQ(stepper.sizeHint().height(), 44);
    EXPECT_LE(stepper.minimumSizeHint().width(), 420);
    EXPECT_GT(unique_opaque_colors(compact), 4);
}

TEST(UiVisualIdentity, AdvancedColumnsStackWithoutOverlapping) {
    VidStoreXResponsiveColumns columns;
    auto *first = new QLabel("Search space");
    auto *second = new QLabel("Encoding and simulation");
    first->setMinimumHeight(80);
    second->setMinimumHeight(80);
    columns.addWidget(first);
    columns.addWidget(second);
    columns.setBreakpoint(900);

    render(columns, {720, 220});
    EXPECT_TRUE(columns.isStacked());
    EXPECT_EQ(columns.property("layoutMode").toString(), "stacked");
    EXPECT_FALSE(first->geometry().intersects(second->geometry()));

    render(columns, {1200, 180});
    EXPECT_FALSE(columns.isStacked());
    EXPECT_EQ(columns.property("layoutMode").toString(), "columns");
    EXPECT_FALSE(first->geometry().intersects(second->geometry()));
}

TEST(UiVisualIdentity, BrandingUsesCanonicalSafeLinkedInUrl) {
    using namespace vidstorex::branding;
    EXPECT_EQ(QString::fromUtf8(kAuthorName),
              QString::fromUtf8("Burhan Talha Yazıcı"));
    EXPECT_STREQ(kAuthorAlias, "BTY");
    EXPECT_EQ(QString::fromLatin1(kLinkedInUrl),
              QStringLiteral("https://www.linkedin.com/in/burhanbty"));
    const QUrl url = linkedInUrl();
    EXPECT_TRUE(url.isValid());
    EXPECT_EQ(url.scheme(), QStringLiteral("https"));
    EXPECT_EQ(url.host(), QStringLiteral("www.linkedin.com"));
    bool invoked = false;
    EXPECT_TRUE(openLinkedInProfile([&](const QUrl &opened) {
        invoked = true;
        return opened == url;
    }));
    EXPECT_TRUE(invoked);
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

TEST(UiVisualIdentity, YouTubeSyncIsExperimentalAndConsumerFlowIsManual) {
    QFile file(QStringLiteral(VIDSTOREX_SOURCE_DIR) +
               "/src/drive_manager_ui.cpp");
    ASSERT_TRUE(file.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString source = QString::fromUtf8(file.readAll());
    EXPECT_TRUE(source.contains("experimentalYouTubeSyncPage"));
    EXPECT_TRUE(source.contains("advancedYouTubeSyncAction"));
    EXPECT_TRUE(source.contains("YouTube Sync (Experimental)"));
    EXPECT_TRUE(source.contains("Upload all parts to YouTube."));
    EXPECT_TRUE(source.contains("videoSetOpenVideosFolderButton"));
    EXPECT_TRUE(source.contains("videoSetOpenYouTubeButton"));
    EXPECT_TRUE(source.contains("use the playlist link for recovery"));
    EXPECT_FALSE(source.contains(
        "uploadLayout->addWidget(youtubeSyncOperationCard)"));
}

TEST(UiVisualIdentity, OnboardingIsProductFocusedAndVersioned) {
    QFile file(QStringLiteral(VIDSTOREX_SOURCE_DIR) +
               "/src/drive_manager_ui.cpp");
    ASSERT_TRUE(file.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString source = QString::fromUtf8(file.readAll());
    const int start = source.indexOf("void DriveManagerUI::setupOnboardingPage");
    const int end = source.indexOf(
        "void DriveManagerUI::setupApplicationNavigation", start);
    ASSERT_GE(start, 0);
    ASSERT_GT(end, start);
    const QString onboarding = source.mid(start, end - start);
    EXPECT_TRUE(onboarding.contains("Turn your file into videos"));
    EXPECT_TRUE(onboarding.contains("Store the videos"));
    EXPECT_TRUE(onboarding.contains(
        "Paste the playlist. Get your file back."));
    EXPECT_TRUE(onboarding.contains("VidStoreXFlowIllustration"));
    EXPECT_TRUE(onboarding.contains("VidStoreXOnboardingProgress"));
    EXPECT_TRUE(source.contains("ui/onboardingVersion"));
    EXPECT_TRUE(source.contains("settingsShowGettingStartedButton"));
    EXPECT_TRUE(source.contains("gettingStartedAction"));
    for (const QString forbidden : {"OAuth", "Google Cloud", "API quota",
                                    "Wirehair", "VSXSET01", "packet",
                                    "frame geometry"})
        EXPECT_FALSE(onboarding.contains(forbidden))
            << forbidden.toStdString();
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
