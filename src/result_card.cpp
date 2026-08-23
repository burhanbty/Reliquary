#include "result_card.h"
#include "app_branding.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QImageWriter>
#include <QLabel>
#include <QLocale>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QScreen>
#include <QSaveFile>
#include <QStandardPaths>
#include <QVBoxLayout>

#include <algorithm>

namespace result_card {
namespace {

[[maybe_unused]] constexpr const char *kTranslationSources[]{
    QT_TRANSLATE_NOOP("VidStoreXResultCard", "Result Card"),
    QT_TRANSLATE_NOOP("VidStoreXResultCard", "Verified Recovery"),
    QT_TRANSLATE_NOOP("VidStoreXResultCard", "Your file was recovered exactly"),
    QT_TRANSLATE_NOOP("VidStoreXResultCard", "Video Set Ready"),
    QT_TRANSLATE_NOOP("VidStoreXResultCard", "Local verification complete"),
    QT_TRANSLATE_NOOP("VidStoreXResultCard", "Ready for upload"),
    QT_TRANSLATE_NOOP("VidStoreXResultCard", "Digital Archive Report"),
    QT_TRANSLATE_NOOP("VidStoreXResultCard", "Playlist"),
    QT_TRANSLATE_NOOP("VidStoreXResultCard", "File"),
    QT_TRANSLATE_NOOP("VidStoreXResultCard", "Original File"),
    QT_TRANSLATE_NOOP("VidStoreXResultCard", "Verified Parts"),
    QT_TRANSLATE_NOOP("VidStoreXResultCard", "YouTube Round-Trip"),
    QT_TRANSLATE_NOOP("VidStoreXResultCard", "Local Recovery"),
    QT_TRANSLATE_NOOP("VidStoreXResultCard", "Verified"),
    QT_TRANSLATE_NOOP("VidStoreXResultCard", "Full-file SHA-256"),
    QT_TRANSLATE_NOOP("VidStoreXResultCard", "Match"),
    QT_TRANSLATE_NOOP("VidStoreXResultCard", "%1 / %2 parts verified"),
    QT_TRANSLATE_NOOP("VidStoreXResultCard", "%1 / %2 videos created"),
    QT_TRANSLATE_NOOP("VidStoreXResultCard", "Created with Reliquary"),
    QT_TRANSLATE_NOOP("VidStoreXResultCard", "Most Reliable"),
    QT_TRANSLATE_NOOP("VidStoreXResultCard", "Fewer Videos"),
    QT_TRANSLATE_NOOP("VidStoreXResultCard", "Show file name"),
    QT_TRANSLATE_NOOP("VidStoreXResultCard", "Show file size"),
    QT_TRANSLATE_NOOP("VidStoreXResultCard", "Show profile"),
    QT_TRANSLATE_NOOP("VidStoreXResultCard", "Show shortened SHA-256"),
    QT_TRANSLATE_NOOP("VidStoreXResultCard", "Show full SHA-256"),
    QT_TRANSLATE_NOOP("VidStoreXResultCard", "Include technical details"),
    QT_TRANSLATE_NOOP("VidStoreXResultCard", "Copy Image"),
    QT_TRANSLATE_NOOP("VidStoreXResultCard", "Save PNG"),
    QT_TRANSLATE_NOOP("VidStoreXResultCard", "Close"),
    QT_TRANSLATE_NOOP("VidStoreXResultCard", "Result card saved"),
    QT_TRANSLATE_NOOP("VidStoreXResultCard", "Result card could not be saved."),
    QT_TRANSLATE_NOOP("VidStoreXResultCard", "Image copied"),
    QT_TRANSLATE_NOOP("VidStoreXResultCard", "PNG Image (*.png)"),
    QT_TRANSLATE_NOOP("VidStoreXResultCard", "Reliquary result card preview")};

QString t(const char *source) {
    return QCoreApplication::translate("VidStoreXResultCard", source);
}

struct Theme {
    QColor background{"#171715"};
    QColor surface{"#201F1C"};
    QColor raised{"#292824"};
    QColor text{"#F4EFE6"};
    QColor secondary{"#C9C1B4"};
    QColor muted{"#8F887D"};
    QColor accent{"#D58A20"};
    QColor success{"#55B78A"};
    QColor border{"#403C34"};
    QColor cell{"#35322C"};
};

void rounded(QPainter &painter, const QRectF &rect, const qreal radius,
             const QColor &fill, const QColor &border = Qt::transparent,
             const qreal borderWidth = 1.0) {
    QPainterPath path;
    path.addRoundedRect(rect, radius, radius);
    painter.fillPath(path, fill);
    if (border.alpha() != 0) {
        painter.setPen(QPen(border, borderWidth));
        painter.drawPath(path);
    }
}

QFont font(const QFont &base, const int pixels, const int weight = QFont::Normal,
           const bool spaced = false) {
    QFont result(base);
    result.setPixelSize(pixels);
    result.setWeight(static_cast<QFont::Weight>(weight));
    if (spaced) result.setLetterSpacing(QFont::AbsoluteSpacing, 2.0);
    return result;
}

QString cleanFileName(const QString &path) {
    const QString normalized = QDir::fromNativeSeparators(path.trimmed());
    return QFileInfo(normalized).fileName();
}

QString profileDisplay(const QString &profile) {
    if (profile.compare(QStringLiteral("resilient"), Qt::CaseInsensitive) == 0)
        return QStringLiteral("Resilient · ") + t("Most Reliable");
    if (profile.compare(QStringLiteral("high-capacity"), Qt::CaseInsensitive) == 0 ||
        profile.compare(QStringLiteral("High Capacity"), Qt::CaseInsensitive) == 0)
        return QStringLiteral("High Capacity · ") + t("Fewer Videos");
    return profile;
}

QString shortSha(const QString &sha) {
    const QString upper = sha.toUpper();
    return upper.size() < 16 ? upper : upper.left(8) + QChar(0x2026) + upper.right(6);
}

QString formatSize(const Model &model) {
    return QLocale(model.localeName).formattedDataSize(
        static_cast<qint64>(model.fileSizeBytes), 1,
        QLocale::DataSizeTraditionalFormat);
}

QString safeStem(QString name) {
    name = QFileInfo(name).completeBaseName();
    name.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._-]+")),
                 QStringLiteral("-"));
    name.replace(QRegularExpression(QStringLiteral("-+")), QStringLiteral("-"));
    name = name.trimmed();
    while (name.startsWith('.') || name.startsWith('-')) name.remove(0, 1);
    while (name.endsWith('.') || name.endsWith('-')) name.chop(1);
    return name.isEmpty() ? QStringLiteral("result") : name.left(80);
}

void drawBrandMark(QPainter &painter, const QRectF &rect, const Theme &theme) {
    rounded(painter, rect, 12, theme.raised, theme.border, 2);
    painter.fillRect(QRectF(rect.left() + 13, rect.top() + 13, 13,
                            rect.height() - 26), theme.text);
    for (int row = 0; row < 2; ++row)
        for (int column = 0; column < 2; ++column)
            painter.fillRect(QRectF(rect.left() + 35 + column * 14,
                                    rect.top() + 17 + row * 14, 10, 10),
                             theme.accent);
    painter.fillRect(QRectF(rect.left() + 35, rect.bottom() - 18, 24, 6),
                     theme.accent);
}

void drawFlow(QPainter &painter, const QRectF &rect, const Model &model,
              const Theme &theme, const QFont &base) {
    rounded(painter, rect, 18, theme.raised, theme.border);
    painter.setPen(theme.secondary);
    painter.setFont(font(base, 18, QFont::DemiBold, true));
    const QString source = model.type == Type::FileRecoveredExact &&
                           model.sourceKind == SourceKind::YouTubePlaylist
        ? t("Playlist") : t("File");
    painter.drawText(QRectF(rect.left() + 34, rect.top() + 30, 220, 30),
                     Qt::AlignLeft | Qt::AlignVCenter, source.toUpper());

    const qreal y = rect.center().y() + 10;
    painter.setPen(QPen(theme.accent, 4, Qt::SolidLine, Qt::RoundCap));
    painter.drawLine(QPointF(rect.left() + 185, y), QPointF(rect.left() + 275, y));
    painter.drawLine(QPointF(rect.left() + 257, y - 10), QPointF(rect.left() + 275, y));
    painter.drawLine(QPointF(rect.left() + 257, y + 10), QPointF(rect.left() + 275, y));

    const int visible = model.partCount <= 8
        ? static_cast<int>(model.partCount) : 6;
    const qreal cellWidth = 62;
    const qreal gap = 12;
    const qreal cellsStart = rect.left() + 310;
    painter.setFont(font(base, 17, QFont::DemiBold));
    for (int index = 0; index < visible; ++index) {
        const QRectF cell(cellsStart + index * (cellWidth + gap), y - 31,
                          cellWidth, 62);
        rounded(painter, cell, 10, theme.cell, theme.success, 2);
        painter.setPen(theme.text);
        painter.drawText(cell, Qt::AlignCenter,
                         QStringLiteral("%1\n✓").arg(index + 1, 2, 10, QChar('0')));
    }
    qreal flowEnd = cellsStart + visible * (cellWidth + gap);
    if (model.partCount > 8) {
        const QRectF aggregate(flowEnd, y - 31, 94, 62);
        rounded(painter, aggregate, 10, theme.accent);
        painter.setPen(QColor("#171715"));
        painter.drawText(aggregate, Qt::AlignCenter,
                         QStringLiteral("×%1").arg(model.partCount));
        flowEnd += 112;
    }

    if (model.type == Type::FileRecoveredExact) {
        painter.setPen(QPen(theme.accent, 4, Qt::SolidLine, Qt::RoundCap));
        painter.drawLine(QPointF(flowEnd + 10, y), QPointF(rect.right() - 245, y));
        painter.drawLine(QPointF(rect.right() - 263, y - 10),
                         QPointF(rect.right() - 245, y));
        painter.drawLine(QPointF(rect.right() - 263, y + 10),
                         QPointF(rect.right() - 245, y));
        painter.setPen(theme.text);
        painter.setFont(font(base, 22, QFont::Bold));
        painter.drawText(QRectF(rect.right() - 220, y - 35, 185, 70),
                         Qt::AlignCenter, t("Original File").toUpper() + "  ✓");
    } else {
        painter.setPen(theme.muted);
        painter.setFont(font(base, 17, QFont::DemiBold));
        painter.drawText(QRectF(rect.right() - 245, y - 30, 210, 60),
                         Qt::AlignCenter,
                         t("Verified Parts").toUpper());
    }
}

} // namespace

std::optional<Model> makeCreateModel(const CreateEvidence &evidence,
                                     const QString &localeName) {
    if (!evidence.allPartsCreated || !evidence.localVerificationPassed ||
        evidence.partCount == 0 ||
        evidence.verifiedPartCount != evidence.partCount)
        return std::nullopt;
    Model model;
    model.type = Type::VideoSetCreated;
    model.fileName = cleanFileName(evidence.filePath);
    model.fileSizeBytes = evidence.fileSizeBytes;
    model.profileName = evidence.profileName;
    model.partCount = evidence.partCount;
    model.verifiedPartCount = evidence.verifiedPartCount;
    model.sourceKind = SourceKind::Local;
    model.timestamp = evidence.completedAt.isValid()
        ? evidence.completedAt : QDateTime::currentDateTime();
    model.appVersion = evidence.appVersion;
    model.localVerificationPassed = true;
    model.localeName = localeName;
    return model;
}

std::optional<Model> makeRecoveryModel(const RecoveryEvidence &evidence,
                                       const QString &localeName) {
    if (evidence.status != RecoveryStatus::RecoveredExact ||
        !evidence.finalShaExact || evidence.partCount == 0 ||
        evidence.verifiedPartCount != evidence.partCount ||
        evidence.sha256.size() != 64)
        return std::nullopt;
    Model model;
    model.type = Type::FileRecoveredExact;
    model.fileName = cleanFileName(evidence.recoveredFilePath);
    model.fileSizeBytes = evidence.fileSizeBytes;
    model.profileName = evidence.profileName;
    model.partCount = evidence.partCount;
    model.verifiedPartCount = evidence.verifiedPartCount;
    model.sourceKind = evidence.sourceKind;
    model.sha256 = evidence.sha256.toUpper();
    model.shaVerified = true;
    model.timestamp = evidence.completedAt.isValid()
        ? evidence.completedAt : QDateTime::currentDateTime();
    model.appVersion = evidence.appVersion;
    model.localVerificationPassed = evidence.returnedPartsExact;
    model.youtubeRoundTripVerified =
        evidence.sourceKind == SourceKind::YouTubePlaylist &&
        evidence.returnedPartsExact && evidence.finalShaExact;
    model.localeName = localeName;
    return model;
}

QString Renderer::visibleText(const Model &model,
                              const PrivacyOptions &privacy) {
    QStringList lines;
    lines << QString::fromLatin1(vidstorex::branding::kProductName)
          << t("Digital Archive Report");
    if (model.type == Type::FileRecoveredExact)
        lines << t("Verified Recovery") << t("Your file was recovered exactly");
    else
        lines << t("Video Set Ready") << t("Local verification complete")
              << t("Ready for upload");
    if (privacy.showFileName && !model.fileName.isEmpty()) lines << model.fileName;
    if (privacy.showFileSize) lines << formatSize(model);
    if (privacy.showProfile && !model.profileName.isEmpty())
        lines << profileDisplay(model.profileName);
    lines << (model.type == Type::FileRecoveredExact
        ? t("%1 / %2 parts verified") : t("%1 / %2 videos created"))
        .arg(model.verifiedPartCount).arg(model.partCount);
    if (model.type == Type::FileRecoveredExact) {
        lines << (model.youtubeRoundTripVerified
            ? t("YouTube Round-Trip") : t("Local Recovery"))
              << t("Verified") << t("Full-file SHA-256") << t("Match");
        if (privacy.showShortSha && model.shaVerified)
            lines << shortSha(model.sha256);
        if (privacy.showFullSha && model.shaVerified) lines << model.sha256;
    }
    if (privacy.showTechnicalDetails)
        lines << QStringLiteral("%1=%2").arg(t("Verified Parts"))
                 .arg(model.verifiedPartCount);
    lines << QLocale(model.localeName).toString(
        model.timestamp, QLocale::ShortFormat);
    lines << (model.appVersion.isEmpty()
        ? QString::fromLatin1(vidstorex::branding::kProductName)
        : QStringLiteral("%1 v%2")
              .arg(QString::fromLatin1(vidstorex::branding::kProductName),
                   model.appVersion));
    lines << t("Created with Reliquary");
    return lines.join('\n');
}

QString Renderer::accessibleSummary(const Model &model) {
    const QString identity = (model.fileName.isEmpty()
        ? QString() : model.fileName + QStringLiteral(". ")) +
        (model.profileName.isEmpty() ? QString() :
            profileDisplay(model.profileName) + QStringLiteral(". "));
    if (model.type == Type::FileRecoveredExact)
        return t("Your file was recovered exactly") + QStringLiteral(". ") +
            identity +
            t("%1 / %2 parts verified").arg(model.verifiedPartCount)
                .arg(model.partCount) + QStringLiteral(". ") +
            (model.youtubeRoundTripVerified
                ? t("YouTube Round-Trip") + QStringLiteral(": ") +
                    t("Verified") + QStringLiteral(". ")
                : t("Local Recovery") + QStringLiteral(": ") +
                    t("Verified") + QStringLiteral(". ")) +
            t("Full-file SHA-256") + QStringLiteral(": ") + t("Match") + '.';
    return t("Video Set Ready") + QStringLiteral(". ") +
        identity +
        t("%1 / %2 videos created").arg(model.verifiedPartCount)
            .arg(model.partCount) + QStringLiteral(". ") +
        t("Local verification complete") + '.';
}

QImage Renderer::render(const Model &model, const PrivacyOptions &privacy) {
    const Theme theme;
    QImage image(Width, Height, QImage::Format_ARGB32_Premultiplied);
    image.fill(theme.background);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);
    const QFont base = QApplication::font();

    // Signal rail: a restrained, non-gradient data signature.
    const int railWidths[]{130, 42, 76, 26, 210, 58, 94};
    qreal railX = 0;
    for (int index = 0; index < 7; ++index) {
        painter.fillRect(QRectF(railX, 0, railWidths[index], 8),
                         index % 3 == 1 ? theme.success : theme.accent);
        railX += railWidths[index] + 12;
    }

    drawBrandMark(painter, QRectF(72, 52, 72, 72), theme);
    painter.setFont(font(base, 34, QFont::Black, true));
    painter.setPen(theme.text);
    painter.drawText(QRectF(166, 55, 500, 44), Qt::AlignVCenter,
                     QString::fromLatin1(
                         vidstorex::branding::kProductName).toUpper());
    painter.setFont(font(base, 17, QFont::DemiBold, true));
    painter.setPen(theme.muted);
    painter.drawText(QRectF(168, 99, 500, 28), Qt::AlignVCenter,
                     t("Digital Archive Report").toUpper());

    const QString version = model.appVersion.isEmpty()
        ? QString::fromLatin1(vidstorex::branding::kProductName)
        : QStringLiteral("%1 v%2")
              .arg(QString::fromLatin1(vidstorex::branding::kProductName),
                   model.appVersion);
    painter.setFont(font(base, 17, QFont::DemiBold));
    painter.setPen(theme.secondary);
    painter.drawText(QRectF(1110, 70, 418, 35),
                     Qt::AlignRight | Qt::AlignVCenter, version);

    rounded(painter, QRectF(64, 152, 1472, 670), 28,
            theme.surface, theme.border, 2);
    painter.setPen(theme.accent);
    painter.setFont(font(base, 22, QFont::Bold, true));
    painter.drawText(QRectF(112, 202, 920, 34), Qt::AlignVCenter,
        model.type == Type::FileRecoveredExact
            ? t("Verified Recovery").toUpper()
            : t("Video Set Ready").toUpper());
    painter.setPen(theme.text);
    painter.setFont(font(base, model.type == Type::FileRecoveredExact ? 50 : 46,
                         QFont::Black));
    painter.drawText(QRectF(112, 247, 1040, 66), Qt::AlignVCenter,
        model.type == Type::FileRecoveredExact
            ? t("Your file was recovered exactly")
            : t("Video Set Ready"));

    QString fileLine;
    if (privacy.showFileName && !model.fileName.isEmpty()) {
        QFont fileFont = font(base, 30, QFont::DemiBold);
        fileLine = QFontMetrics(fileFont).elidedText(
            model.fileName, Qt::ElideMiddle, 920);
        painter.setFont(fileFont);
        painter.setPen(theme.secondary);
        painter.drawText(QRectF(112, 326, 930, 46), Qt::AlignVCenter, fileLine);
    }
    QStringList metrics;
    if (privacy.showFileSize) metrics << formatSize(model);
    if (privacy.showProfile && !model.profileName.isEmpty())
        metrics << profileDisplay(model.profileName);
    metrics << (model.type == Type::FileRecoveredExact
        ? t("%1 / %2 parts verified") : t("%1 / %2 videos created"))
        .arg(model.verifiedPartCount).arg(model.partCount);
    painter.setFont(font(base, 20, QFont::Medium));
    painter.setPen(theme.muted);
    painter.drawText(QRectF(112, 372, 1200, 36), Qt::AlignVCenter,
                     metrics.join(QStringLiteral("  ·  ")));

    drawFlow(painter, QRectF(104, 430, 1392, 182), model, theme, base);

    const QRectF statusLeft(104, 638, 672, 134);
    const QRectF statusRight(800, 638, 696, 134);
    rounded(painter, statusLeft, 18, theme.raised, theme.border);
    rounded(painter, statusRight, 18, theme.raised, theme.border);
    painter.setFont(font(base, 17, QFont::DemiBold, true));
    painter.setPen(theme.muted);
    painter.drawText(statusLeft.adjusted(28, 20, -28, -76),
                     Qt::AlignLeft | Qt::AlignVCenter,
        model.type == Type::FileRecoveredExact
            ? (model.youtubeRoundTripVerified
                ? t("YouTube Round-Trip").toUpper()
                : t("Local Recovery").toUpper())
            : t("Local verification complete").toUpper());
    painter.setFont(font(base, 30, QFont::Bold));
    painter.setPen(theme.success);
    painter.drawText(statusLeft.adjusted(28, 58, -28, -18),
                     Qt::AlignLeft | Qt::AlignVCenter,
                     t("Verified").toUpper() + QStringLiteral("  ✓"));

    if (model.type == Type::FileRecoveredExact) {
        painter.setFont(font(base, 17, QFont::DemiBold, true));
        painter.setPen(theme.muted);
        painter.drawText(statusRight.adjusted(28, 18, -28, -78),
                         Qt::AlignLeft | Qt::AlignVCenter,
                         t("Full-file SHA-256").toUpper());
        painter.setFont(font(base, 29, QFont::Bold));
        painter.setPen(theme.success);
        painter.drawText(statusRight.adjusted(28, 52, -28, -28),
                         Qt::AlignLeft | Qt::AlignVCenter,
                         t("Match").toUpper() + QStringLiteral("  ✓"));
        if (privacy.showShortSha && model.shaVerified) {
            painter.setFont(font(QFontDatabase::systemFont(
                QFontDatabase::FixedFont), 19, QFont::DemiBold));
            painter.setPen(theme.secondary);
            painter.drawText(statusRight.adjusted(300, 51, -24, -28),
                             Qt::AlignRight | Qt::AlignVCenter,
                             shortSha(model.sha256));
        }
    } else {
        painter.setFont(font(base, 17, QFont::DemiBold, true));
        painter.setPen(theme.muted);
        painter.drawText(statusRight.adjusted(28, 18, -28, -78),
                         Qt::AlignLeft | Qt::AlignVCenter,
                         t("Video Set Ready").toUpper());
        painter.setFont(font(base, 31, QFont::Bold));
        painter.setPen(theme.accent);
        painter.drawText(statusRight.adjusted(28, 52, -28, -28),
                         Qt::AlignLeft | Qt::AlignVCenter,
                         t("Ready for upload"));
    }

    if (model.type == Type::FileRecoveredExact && privacy.showFullSha &&
        model.shaVerified) {
        painter.setFont(font(QFontDatabase::systemFont(
            QFontDatabase::FixedFont), 14, QFont::Normal));
        painter.setPen(theme.muted);
        painter.drawText(QRectF(112, 782, 1376, 22), Qt::AlignCenter,
                         model.sha256.left(32) + QStringLiteral("  ") +
                         model.sha256.mid(32));
    }
    if (privacy.showTechnicalDetails) {
        painter.setFont(font(QFontDatabase::systemFont(
            QFontDatabase::FixedFont), 13, QFont::Normal));
        painter.setPen(theme.muted);
        const QString source = model.sourceKind == SourceKind::YouTubePlaylist
            ? t("Playlist") : t("File");
        painter.drawText(QRectF(112, 808, 1376, 18), Qt::AlignCenter,
            QStringLiteral("%1  ·  %2 %3/%4")
                .arg(source.toUpper(), t("Verified Parts").toUpper())
                .arg(model.verifiedPartCount).arg(model.partCount));
    }

    painter.setFont(font(base, 16, QFont::Medium));
    painter.setPen(theme.muted);
    const QString timestamp = QLocale(model.localeName).toString(
        model.timestamp, QLocale::ShortFormat);
    painter.drawText(QRectF(72, 838, 600, 30), Qt::AlignVCenter, timestamp);
    painter.drawText(QRectF(930, 838, 598, 30),
                     Qt::AlignRight | Qt::AlignVCenter,
                     t("Created with Reliquary"));
    return image;
}

QString suggestedFileName(const Model &model) {
    return QStringLiteral("Reliquary_%1_%2.png")
        .arg(safeStem(model.fileName),
             model.type == Type::FileRecoveredExact
                ? QStringLiteral("recovered")
                : QStringLiteral("video-set"));
}

ExportResult savePng(const QImage &image, QString path,
                     const bool allowOverwrite) {
    ExportResult result;
    path = path.trimmed();
    if (path.isEmpty()) {
        result.error = QStringLiteral("empty output path");
        return result;
    }
    if (!path.endsWith(QStringLiteral(".png"), Qt::CaseInsensitive))
        path += QStringLiteral(".png");
    const QFileInfo target(path);
    if (target.exists() && !allowOverwrite) {
        result.path = target.absoluteFilePath();
        result.error = QStringLiteral("target already exists");
        return result;
    }
    if (!QDir().mkpath(target.absolutePath())) {
        result.path = target.absoluteFilePath();
        result.error = QStringLiteral("output directory is not writable");
        return result;
    }
    QSaveFile output(target.absoluteFilePath());
    if (!output.open(QIODevice::WriteOnly)) {
        result.path = target.absoluteFilePath();
        result.error = output.errorString();
        return result;
    }
    QImageWriter writer(&output, "png");
    if (!writer.write(image)) {
        output.cancelWriting();
        result.path = target.absoluteFilePath();
        result.error = writer.errorString();
        return result;
    }
    if (!output.commit()) {
        result.path = target.absoluteFilePath();
        result.error = output.errorString();
        return result;
    }
    result.ok = true;
    result.path = target.absoluteFilePath();
    return result;
}

PreviewDialog::PreviewDialog(Model model, QWidget *parent)
    : QDialog(parent), model_(std::move(model)) {
    setObjectName(QStringLiteral("resultCardPreviewDialog"));
    setModal(true);
    setMinimumSize(720, 480);
    QSize dialogSize(900, 600);
    if (const auto *screen = QApplication::primaryScreen()) {
        const QSize safe = screen->availableGeometry().size() - QSize(64, 64);
        dialogSize = dialogSize.boundedTo(safe).expandedTo(minimumSize());
    }
    resize(dialogSize);
    setProperty("vsxSurface", "dialog");
    setStyleSheet(QStringLiteral(
        "QDialog#resultCardPreviewDialog { background: #201F1C; color: #F4EFE6; }"
        "QDialog#resultCardPreviewDialog QLabel { color: #F4EFE6; }"
        "QLabel#resultCardPreviewTitle { font-size: 20px; font-weight: 700; }"
        "QLabel#resultCardPreviewImage { background: #171715; border: 1px solid #403C34; border-radius: 12px; }"
        "QDialog#resultCardPreviewDialog QCheckBox { color: #C9C1B4; spacing: 7px; }"
        "QDialog#resultCardPreviewDialog QPushButton { min-height: 34px; padding: 0 16px; border-radius: 6px; border: 1px solid #5A5449; background: #292824; color: #F4EFE6; }"
        "QDialog#resultCardPreviewDialog QPushButton[vsxRole=\"primary\"] { background: #D58A20; border-color: #D58A20; color: #171715; font-weight: 700; }"
        "QDialog#resultCardPreviewDialog QPushButton[vsxRole=\"ghost\"] { background: transparent; border-color: transparent; color: #C9C1B4; }"
        "QLabel#resultCardExportStatus[vsxState=\"success\"] { color: #55B78A; }"
        "QLabel#resultCardExportStatus[vsxState=\"error\"] { color: #E07A67; }"));

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(20, 18, 20, 18);
    root->setSpacing(12);
    title_ = new QLabel();
    title_->setObjectName(QStringLiteral("resultCardPreviewTitle"));
    title_->setProperty("pageTitle", true);
    root->addWidget(title_);
    preview_ = new QLabel();
    preview_->setObjectName(QStringLiteral("resultCardPreviewImage"));
    preview_->setAlignment(Qt::AlignCenter);
    preview_->setMinimumHeight(300);
    preview_->setAccessibleName(t("Reliquary result card preview"));
    preview_->setAccessibleDescription(Renderer::accessibleSummary(model_));
    root->addWidget(preview_, 1);

    auto *privacyRow = new QGridLayout();
    showFileName_ = new QCheckBox();
    showFileSize_ = new QCheckBox();
    showProfile_ = new QCheckBox();
    showShortSha_ = new QCheckBox();
    showFullSha_ = new QCheckBox();
    showTechnicalDetails_ = new QCheckBox();
    showFileName_->setObjectName(QStringLiteral("resultCardShowFileName"));
    showFileSize_->setObjectName(QStringLiteral("resultCardShowFileSize"));
    showProfile_->setObjectName(QStringLiteral("resultCardShowProfile"));
    showShortSha_->setObjectName(QStringLiteral("resultCardShowShortSha"));
    showFullSha_->setObjectName(QStringLiteral("resultCardShowFullSha"));
    showTechnicalDetails_->setObjectName(
        QStringLiteral("resultCardShowTechnicalDetails"));
    for (auto *box : {showFileName_, showFileSize_, showProfile_, showShortSha_})
        box->setChecked(true);
    int privacyIndex = 0;
    for (auto *box : {showFileName_, showFileSize_, showProfile_, showShortSha_,
                      showFullSha_, showTechnicalDetails_}) {
        privacyRow->addWidget(box, privacyIndex / 3, privacyIndex % 3);
        ++privacyIndex;
        connect(box, &QCheckBox::toggled, this,
                [this]() { rerender(); });
    }
    root->addLayout(privacyRow);

    status_ = new QLabel();
    status_->setObjectName(QStringLiteral("resultCardExportStatus"));
    status_->setProperty("muted", true);
    status_->setAccessibleName(QStringLiteral("Result card export status"));
    root->addWidget(status_);

    auto *actions = new QHBoxLayout();
    copyButton_ = new QPushButton();
    copyButton_->setObjectName(QStringLiteral("resultCardCopyImageButton"));
    copyButton_->setProperty("vsxRole", "secondary");
    saveButton_ = new QPushButton();
    saveButton_->setObjectName(QStringLiteral("resultCardSavePngButton"));
    saveButton_->setProperty("vsxRole", "primary");
    closeButton_ = new QPushButton();
    closeButton_->setObjectName(QStringLiteral("resultCardCloseButton"));
    closeButton_->setProperty("vsxRole", "ghost");
    actions->addWidget(copyButton_);
    actions->addStretch();
    actions->addWidget(closeButton_);
    actions->addWidget(saveButton_);
    root->addLayout(actions);
    connect(copyButton_, &QPushButton::clicked, this,
            [this]() { copyImage(); });
    connect(saveButton_, &QPushButton::clicked, this,
            [this]() { saveImage(); });
    connect(closeButton_, &QPushButton::clicked, this, &QDialog::reject);
    retranslate();
    rerender();
}

PrivacyOptions PreviewDialog::privacyOptions() const {
    return {showFileName_->isChecked(), showFileSize_->isChecked(),
            showProfile_->isChecked(), showShortSha_->isChecked(),
            showFullSha_->isChecked(), showTechnicalDetails_->isChecked()};
}

void PreviewDialog::retranslate() {
    setWindowTitle(t("Result Card"));
    title_->setText(t("Result Card"));
    showFileName_->setText(t("Show file name"));
    showFileSize_->setText(t("Show file size"));
    showProfile_->setText(t("Show profile"));
    showShortSha_->setText(t("Show shortened SHA-256"));
    showFullSha_->setText(t("Show full SHA-256"));
    showTechnicalDetails_->setText(t("Include technical details"));
    copyButton_->setText(t("Copy Image"));
    saveButton_->setText(t("Save PNG"));
    closeButton_->setText(t("Close"));
    for (auto *box : {showFileName_, showFileSize_, showProfile_, showShortSha_,
                      showFullSha_, showTechnicalDetails_})
        box->setAccessibleName(box->text());
    copyButton_->setAccessibleName(copyButton_->text());
    saveButton_->setAccessibleName(saveButton_->text());
    closeButton_->setAccessibleName(closeButton_->text());
}

void PreviewDialog::rerender() {
    rendered_ = Renderer::render(model_, privacyOptions());
    updatePreviewPixmap();
    preview_->setAccessibleDescription(Renderer::accessibleSummary(model_));
}

void PreviewDialog::updatePreviewPixmap() {
    if (rendered_.isNull() || !preview_) return;
    const QSize available = preview_->contentsRect().size();
    preview_->setPixmap(QPixmap::fromImage(rendered_).scaled(
        available, Qt::KeepAspectRatio, Qt::SmoothTransformation));
}

void PreviewDialog::saveImage() {
    QString path;
    bool overwriteConfirmed = false;
#ifdef VIDSTOREX_ENABLE_TEST_HOOKS
    path = QString::fromLocal8Bit(qgetenv("VIDSTOREX_RESULT_CARD_TEST_OUTPUT"));
#endif
    if (path.isEmpty()) {
        const QString folder = QStandardPaths::writableLocation(
            QStandardPaths::PicturesLocation);
        path = QFileDialog::getSaveFileName(
            this, t("Save PNG"), QDir(folder).filePath(suggestedFileName(model_)),
            t("PNG Image (*.png)"));
        if (path.isEmpty()) return;
        overwriteConfirmed = true;
    }
    const auto result = savePng(rendered_, path, overwriteConfirmed);
    if (!result.ok) {
        status_->setProperty("vsxState", "error");
        status_->setText(t("Result card could not be saved.") +
                         QStringLiteral(" ") + result.error);
#ifndef VIDSTOREX_ENABLE_TEST_HOOKS
        QMessageBox::warning(this, t("Result Card"), status_->text());
#endif
        return;
    }
    status_->setProperty("vsxState", "success");
    status_->setText(t("Result card saved") + QStringLiteral(": ") +
                     QFileInfo(result.path).fileName());
    setProperty("savedPath", result.path);
}

void PreviewDialog::copyImage() {
    QApplication::clipboard()->setImage(rendered_);
    status_->setProperty("vsxState", "success");
    status_->setText(t("Image copied"));
}

void PreviewDialog::changeEvent(QEvent *event) {
    QDialog::changeEvent(event);
    if (event->type() == QEvent::LanguageChange) {
        retranslate();
        rerender();
    }
}

void PreviewDialog::resizeEvent(QResizeEvent *event) {
    QDialog::resizeEvent(event);
    updatePreviewPixmap();
}

} // namespace result_card
