#include "result_card.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QCheckBox>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QElapsedTimer>
#include <QLabel>
#include <QPushButton>
#include <QSet>
#include <QTemporaryDir>
#include <QTranslator>

namespace {

constexpr auto kSha =
    "0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF";

result_card::RecoveryEvidence recoveryEvidence(
    result_card::SourceKind source = result_card::SourceKind::YouTubePlaylist,
    uint32_t parts = 4) {
    result_card::RecoveryEvidence evidence;
    evidence.status = result_card::RecoveryStatus::RecoveredExact;
    evidence.recoveredFilePath =
        QStringLiteral("C:/Users/private/Recovered/archive.zip");
    evidence.fileSizeBytes = 842ULL * 1024 * 1024;
    evidence.profileName = QStringLiteral("high-capacity");
    evidence.partCount = parts;
    evidence.verifiedPartCount = parts;
    evidence.sourceKind = source;
    evidence.sha256 = QString::fromLatin1(kSha);
    evidence.returnedPartsExact = true;
    evidence.finalShaExact = true;
    evidence.completedAt = QDateTime::fromString(
        QStringLiteral("2026-08-11T08:12:00+03:00"), Qt::ISODate);
    evidence.appVersion = QStringLiteral("1.4.0");
    return evidence;
}

result_card::CreateEvidence createEvidence(uint32_t parts = 4) {
    result_card::CreateEvidence evidence;
    evidence.filePath = QStringLiteral("C:/Users/private/archive.zip");
    evidence.fileSizeBytes = 842ULL * 1024 * 1024;
    evidence.profileName = QStringLiteral("resilient");
    evidence.partCount = parts;
    evidence.verifiedPartCount = parts;
    evidence.allPartsCreated = true;
    evidence.localVerificationPassed = true;
    evidence.completedAt = QDateTime::fromString(
        QStringLiteral("2026-08-11T08:12:00+03:00"), Qt::ISODate);
    evidence.appVersion = QStringLiteral("1.4.0");
    return evidence;
}

int uniqueColors(const QImage &image) {
    QSet<QRgb> colors;
    for (int y = 0; y < image.height(); y += 4)
        for (int x = 0; x < image.width(); x += 4)
            colors.insert(image.pixel(x, y));
    return colors.size();
}

} // namespace

TEST(ResultCardModel, ValidCreateUsesOnlyBaseNameAndLocalEvidence) {
    const auto model = result_card::makeCreateModel(
        createEvidence(), QStringLiteral("en_US"));
    ASSERT_TRUE(model.has_value());
    EXPECT_EQ(model->type, result_card::Type::VideoSetCreated);
    EXPECT_EQ(model->fileName, QStringLiteral("archive.zip"));
    EXPECT_TRUE(model->localVerificationPassed);
    EXPECT_FALSE(model->youtubeRoundTripVerified);
}

TEST(ResultCardModel, PartialCreateIsRejected) {
    auto evidence = createEvidence();
    evidence.verifiedPartCount = 3;
    EXPECT_FALSE(result_card::makeCreateModel(
        evidence, QStringLiteral("en_US")).has_value());
}

TEST(ResultCardModel, ExactRecoveryRequiresShaAndEveryPart) {
    const auto model = result_card::makeRecoveryModel(
        recoveryEvidence(), QStringLiteral("en_US"));
    ASSERT_TRUE(model.has_value());
    EXPECT_TRUE(model->shaVerified);
    EXPECT_TRUE(model->youtubeRoundTripVerified);

    auto mismatch = recoveryEvidence();
    mismatch.status = result_card::RecoveryStatus::ShaMismatch;
    mismatch.finalShaExact = false;
    EXPECT_FALSE(result_card::makeRecoveryModel(
        mismatch, QStringLiteral("en_US")).has_value());

    auto incomplete = recoveryEvidence();
    incomplete.verifiedPartCount = 3;
    EXPECT_FALSE(result_card::makeRecoveryModel(
        incomplete, QStringLiteral("en_US")).has_value());
}

TEST(ResultCardModel, YouTubeClaimIsSpecificToReturnedPlaylistEvidence) {
    auto local = result_card::makeRecoveryModel(
        recoveryEvidence(result_card::SourceKind::Local),
        QStringLiteral("en_US"));
    ASSERT_TRUE(local.has_value());
    EXPECT_FALSE(local->youtubeRoundTripVerified);
    EXPECT_TRUE(result_card::Renderer::visibleText(*local).contains(
        QStringLiteral("Local Recovery")));
    EXPECT_FALSE(result_card::Renderer::visibleText(*local).contains(
        QStringLiteral("YouTube Round-Trip")));

    auto returnedNotExact = recoveryEvidence();
    returnedNotExact.returnedPartsExact = false;
    const auto model = result_card::makeRecoveryModel(
        returnedNotExact, QStringLiteral("en_US"));
    ASSERT_TRUE(model.has_value());
    EXPECT_FALSE(model->youtubeRoundTripVerified);
}

TEST(ResultCardPrivacy, PathsUrlsVideoIdsAndOAuthAreNeverModelText) {
    const auto model = result_card::makeRecoveryModel(
        recoveryEvidence(), QStringLiteral("en_US"));
    ASSERT_TRUE(model.has_value());
    const QString text = result_card::Renderer::visibleText(*model);
    EXPECT_TRUE(text.contains(QStringLiteral("archive.zip")));
    EXPECT_FALSE(text.contains(QStringLiteral("C:/Users")));
    EXPECT_FALSE(text.contains(QStringLiteral("private")));
    EXPECT_FALSE(text.contains(QStringLiteral("playlist?list=")));
    EXPECT_FALSE(text.contains(QStringLiteral("PL_SECRET")));
    EXPECT_FALSE(text.contains(QStringLiteral("oauth-token")));
}

TEST(ResultCardPrivacy, FilenameAndShaOptionsAreIndependent) {
    const auto model = result_card::makeRecoveryModel(
        recoveryEvidence(), QStringLiteral("en_US"));
    ASSERT_TRUE(model.has_value());
    result_card::PrivacyOptions privacy;
    privacy.showFileName = false;
    privacy.showShortSha = false;
    privacy.showFullSha = true;
    const QString text = result_card::Renderer::visibleText(*model, privacy);
    EXPECT_FALSE(text.contains(QStringLiteral("archive.zip")));
    EXPECT_TRUE(text.contains(QString::fromLatin1(kSha)));
}

TEST(ResultCardRenderer, RecoveryIsDeterministicOpaqueBrandCanvas) {
    const auto model = result_card::makeRecoveryModel(
        recoveryEvidence(), QStringLiteral("en_US"));
    ASSERT_TRUE(model.has_value());
    QElapsedTimer timer;
    timer.start();
    const QImage image = result_card::Renderer::render(*model);
    EXPECT_LT(timer.elapsed(), 500);
    EXPECT_EQ(image.size(), QSize(1600, 900));
    EXPECT_EQ(image.format(), QImage::Format_ARGB32_Premultiplied);
    EXPECT_FALSE(image.isNull());
    EXPECT_GT(uniqueColors(image), 20);
    EXPECT_EQ(qAlpha(image.pixel(0, 899)), 255);
    EXPECT_NE(image.pixelColor(20, 4), image.pixelColor(20, 20));
}

TEST(ResultCardRenderer, BrandDarkIsIndependentOfApplicationLightPalette) {
    const auto model = result_card::makeRecoveryModel(
        recoveryEvidence(), QStringLiteral("en_US"));
    ASSERT_TRUE(model.has_value());
    const QPalette original = qApp->palette();
    QPalette light = original;
    light.setColor(QPalette::Window, Qt::white);
    light.setColor(QPalette::WindowText, Qt::black);
    qApp->setPalette(light);
    const QImage lightAppImage = result_card::Renderer::render(*model);
    qApp->setPalette(original);
    const QImage restoredImage = result_card::Renderer::render(*model);
    EXPECT_EQ(lightAppImage.size(), QSize(1600, 900));
    EXPECT_EQ(lightAppImage.pixelColor(0, 899), QColor("#171715"));
    EXPECT_EQ(restoredImage.pixelColor(0, 899), QColor("#171715"));
}

TEST(ResultCardRenderer, CreateAndLargePartCountsStayAtExactDimensions) {
    for (const uint32_t parts : {1u, 4u, 12u, 100u, 1000u}) {
        const auto model = result_card::makeCreateModel(
            createEvidence(parts), QStringLiteral("en_US"));
        ASSERT_TRUE(model.has_value());
        const QImage image = result_card::Renderer::render(*model);
        EXPECT_EQ(image.size(), QSize(1600, 900));
        EXPECT_GT(uniqueColors(image), 15);
    }
}

TEST(ResultCardRenderer, LongFilenameHugeSizeAndTurkishRenderSafely) {
    auto evidence = recoveryEvidence(result_card::SourceKind::Local, 124);
    evidence.recoveredFilePath = QStringLiteral(
        "C:/private/final_super_long_project_archive_backup_2026_revision_47.zip");
    evidence.fileSizeBytes = 2ULL * 1024 * 1024 * 1024 * 1024;
    const auto model = result_card::makeRecoveryModel(
        evidence, QStringLiteral("tr_TR"));
    ASSERT_TRUE(model.has_value());
    const QImage image = result_card::Renderer::render(*model);
    EXPECT_EQ(image.size(), QSize(1600, 900));
    EXPECT_FALSE(image.isNull());
}

TEST(ResultCardLocalization, TurkishTranslatorChangesCardLabels) {
    QTranslator translator;
    ASSERT_TRUE(translator.load(QStringLiteral(":/i18n/vidstorex_tr.qm")));
    qApp->installTranslator(&translator);
    const auto model = result_card::makeRecoveryModel(
        recoveryEvidence(), QStringLiteral("tr_TR"));
    ASSERT_TRUE(model.has_value());
    const QString text = result_card::Renderer::visibleText(*model);
    EXPECT_TRUE(text.contains(QString::fromUtf8("Doğrulanmış Kurtarma")));
    EXPECT_TRUE(text.contains(QString::fromUtf8("Eşleşti")));
    qApp->removeTranslator(&translator);
}

TEST(ResultCardExport, AddsPngRejectsExistingAndProducesReadableImage) {
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const auto model = result_card::makeRecoveryModel(
        recoveryEvidence(), QStringLiteral("en_US"));
    ASSERT_TRUE(model.has_value());
    const QString withoutExtension = temporary.filePath(QStringLiteral("card"));
    const auto saved = result_card::savePng(
        result_card::Renderer::render(*model), withoutExtension);
    ASSERT_TRUE(saved.ok) << saved.error.toStdString();
    EXPECT_TRUE(saved.path.endsWith(QStringLiteral(".png")));
    const QImage loaded(saved.path);
    EXPECT_EQ(loaded.size(), QSize(1600, 900));
    const auto duplicate = result_card::savePng(
        result_card::Renderer::render(*model), saved.path);
    EXPECT_FALSE(duplicate.ok);
    EXPECT_TRUE(QFileInfo::exists(saved.path));
}

TEST(ResultCardExport, UnwritableShapeFailsWithoutChangingOperation) {
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const QString parentFile = temporary.filePath(QStringLiteral("not-a-folder"));
    QFile file(parentFile);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write("x");
    file.close();
    const auto model = result_card::makeCreateModel(
        createEvidence(), QStringLiteral("en_US"));
    ASSERT_TRUE(model.has_value());
    const auto result = result_card::savePng(
        result_card::Renderer::render(*model),
        parentFile + QStringLiteral("/card.png"));
    EXPECT_FALSE(result.ok);
}

TEST(ResultCardExport, PngContainsNoPrivateTextMetadata) {
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const auto model = result_card::makeRecoveryModel(
        recoveryEvidence(), QStringLiteral("en_US"));
    ASSERT_TRUE(model.has_value());
    const auto saved = result_card::savePng(
        result_card::Renderer::render(*model), temporary.filePath("private.png"));
    ASSERT_TRUE(saved.ok);
    QFile file(saved.path);
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    const QByteArray bytes = file.readAll();
    EXPECT_FALSE(bytes.contains("C:/Users"));
    EXPECT_FALSE(bytes.contains("playlist?list="));
    EXPECT_FALSE(bytes.contains("oauth-token"));
}

TEST(ResultCardPreview, HasAccessiblePrivacyAndActionControls) {
    const auto model = result_card::makeRecoveryModel(
        recoveryEvidence(), QStringLiteral("en_US"));
    ASSERT_TRUE(model.has_value());
    result_card::PreviewDialog dialog(*model);
    dialog.show();
    QApplication::processEvents();
    auto *preview = dialog.findChild<QLabel *>("resultCardPreviewImage");
    auto *fileName = dialog.findChild<QCheckBox *>("resultCardShowFileName");
    auto *save = dialog.findChild<QPushButton *>("resultCardSavePngButton");
    auto *copy = dialog.findChild<QPushButton *>("resultCardCopyImageButton");
    ASSERT_NE(preview, nullptr);
    ASSERT_NE(fileName, nullptr);
    ASSERT_NE(save, nullptr);
    ASSERT_NE(copy, nullptr);
    EXPECT_FALSE(preview->accessibleName().isEmpty());
    EXPECT_FALSE(preview->accessibleDescription().isEmpty());
    EXPECT_FALSE(fileName->accessibleName().isEmpty());
    EXPECT_FALSE(save->accessibleName().isEmpty());
    EXPECT_FALSE(copy->accessibleName().isEmpty());
    EXPECT_TRUE(dialog.styleSheet().contains(QStringLiteral("#201F1C")));
    fileName->setChecked(false);
    QApplication::processEvents();
    EXPECT_EQ(dialog.renderedImage().size(), QSize(1600, 900));
    dialog.close();
}
