// This file is part of yt-media-storage, a tool for encoding media.
// Copyright (C) 2026 Brandon Li <https://brandonli.me/>
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#include <QApplication>
#include <QAction>
#include <QDateTime>
#include <QComboBox>
#include <QClipboard>
#include <QDebug>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QLineEdit>
#include <QDir>
#include <QDialog>
#include <QFile>
#include <QFrame>
#include <QGraphicsEffect>
#include <QIcon>
#include <QKeyEvent>
#include <QListWidget>
#include <QPainter>
#include <QPixmap>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSettings>
#include <QSet>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QStyleFactory>
#include <QStatusBar>
#include <QTextDocument>

#include <algorithm>
#include <filesystem>
#include <memory>

#include "drive_manager_ui.h"
#include "app_branding.h"
#include "ui_theme.h"
#include "visual_components.h"
#include "youtube_auth.h"
#include "youtube_sync_state.h"

namespace {

QIcon vidStoreXApplicationIcon() {
    QIcon icon;
    for (const int size : {16, 24, 32, 48, 256}) {
        QPixmap pixmap(size, size);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing, size >= 32);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor("#262522"));
        painter.drawRoundedRect(QRectF(1, 1, size - 2, size - 2),
                                size * 0.16, size * 0.16);
        const qreal unit = size / 8.0;
        painter.setBrush(QColor("#F3EEE5"));
        painter.drawRect(QRectF(unit * 1.4, unit * 1.6,
                                unit * 2.4, unit * 4.8));
        painter.setBrush(QColor("#D58A20"));
        for (int row = 0; row < 2; ++row)
            for (int column = 0; column < 2; ++column)
                painter.drawRect(QRectF(unit * (4.6 + column * 1.25),
                                        unit * (2.2 + row * 1.25),
                                        unit, unit));
        painter.drawRect(QRectF(unit * 4.6, unit * 5.2,
                                unit * 2.25, unit * 0.65));
        icon.addPixmap(pixmap);
    }
    return icon;
}

#ifdef VIDSTOREX_ENABLE_TEST_HOOKS
class FakeYouTubeApi final : public QObject {
public:
    explicit FakeYouTubeApi(QObject *parent = nullptr) : QObject(parent) {
        connect(&server_, &QTcpServer::newConnection, this, [this]() {
            while (auto *socket = server_.nextPendingConnection()) {
                connect(socket, &QTcpSocket::readyRead, this,
                        [this, socket]() { consume(socket); });
            }
        });
    }

    bool start() {
        return server_.listen(QHostAddress::LocalHost, 0);
    }

    QString apiBase() const {
        return QStringLiteral("http://127.0.0.1:%1/youtube/v3")
            .arg(server_.serverPort());
    }

    QString uploadBase() const {
        return QStringLiteral("http://127.0.0.1:%1/upload/youtube/v3")
            .arg(server_.serverPort());
    }

    bool complete(const int expectedParts) const {
        return playlistCreates_ == 1 && sessionCreates_ == expectedParts &&
            completedUploads_ == expectedParts &&
            playlistInserts_ == expectedParts && processingChecks_ == 1 &&
            authorizationValid_ && metadataPrivate_;
    }

private:
    void consume(QTcpSocket *socket) {
        auto &buffer = buffers_[socket];
        buffer += socket->readAll();
        const int headerEnd = buffer.indexOf("\r\n\r\n");
        if (headerEnd < 0) return;
        const QByteArray headers = buffer.left(headerEnd + 4);
        const QRegularExpression contentLength(
            QStringLiteral("(?:^|\\r\\n)Content-Length:\\s*(\\d+)"),
            QRegularExpression::CaseInsensitiveOption);
        const auto match = contentLength.match(QString::fromLatin1(headers));
        const qint64 bodyLength = match.hasMatch()
            ? match.captured(1).toLongLong() : 0;
        if (buffer.size() < headerEnd + 4 + bodyLength) return;
        const QByteArray body = buffer.mid(headerEnd + 4, bodyLength);
        const QList<QByteArray> requestLine = headers.left(
            headers.indexOf("\r\n")).split(' ');
        if (requestLine.size() < 2) {
            respond(socket, 400, "Bad Request", "{}");
            return;
        }
        const QByteArray method = requestLine.at(0);
        const QByteArray target = requestLine.at(1);
        authorizationValid_ = authorizationValid_ &&
            headers.contains("Authorization: Bearer gui-e2e-access");
        if (method == "POST" && target.startsWith(
                "/youtube/v3/playlists?")) {
            ++playlistCreates_;
            respond(socket, 200, "OK",
                R"({"id":"PL_FAKE_SYNC","status":{"privacyStatus":"unlisted"}})");
        } else if (method == "POST" && target.startsWith(
                       "/upload/youtube/v3/videos?")) {
            if (body.contains("source.bin") || body.contains("\\\\") ||
                body.contains(":/"))
                metadataPrivate_ = false;
            const int session = ++sessionCreates_;
            respond(socket, 200, "OK", {}, {{"Location",
                QStringLiteral("http://127.0.0.1:%1/session/%2")
                    .arg(server_.serverPort()).arg(session).toLatin1()}});
        } else if (method == "PUT" && target.startsWith("/session/")) {
            const QRegularExpression range(
                QStringLiteral("Content-Range:\\s*bytes\\s+(\\d+)-(\\d+)/(\\d+)"),
                QRegularExpression::CaseInsensitiveOption);
            const auto rangeMatch = range.match(QString::fromLatin1(headers));
            if (!rangeMatch.hasMatch()) {
                respond(socket, 400, "Bad Request", "{}");
                return;
            }
            const quint64 last = rangeMatch.captured(2).toULongLong();
            const quint64 total = rangeMatch.captured(3).toULongLong();
            if (last + 1 < total) {
                respond(socket, 308, "Resume Incomplete", {},
                    {{"Range", QByteArray("bytes=0-") + QByteArray::number(last)}});
            } else {
                const QByteArray session = target.mid(target.lastIndexOf('/') + 1);
                const QByteArray id = "fake-video-" + session;
                uploadedVideos_ << QString::fromLatin1(id);
                ++completedUploads_;
                respond(socket, 201, "Created",
                    QByteArray("{\"id\":\"") + id +
                    "\",\"status\":{\"privacyStatus\":\"unlisted\"}}");
            }
        } else if (method == "POST" && target.startsWith(
                       "/youtube/v3/playlistItems?")) {
            ++playlistInserts_;
            respond(socket, 200, "OK",
                QByteArray("{\"id\":\"playlist-item-") +
                QByteArray::number(playlistInserts_) + "\"}");
        } else if (method == "GET" && target.startsWith(
                       "/youtube/v3/videos?")) {
            ++processingChecks_;
            QByteArray items;
            for (const auto &id : uploadedVideos_) {
                if (!items.isEmpty()) items += ',';
                items += QByteArray("{\"id\":\"") + id.toLatin1() +
                    "\",\"processingDetails\":{\"processingStatus\":\"succeeded\","
                    "\"processingProgress\":{\"partsProcessed\":1,\"partsTotal\":1}},"
                    "\"status\":{\"privacyStatus\":\"unlisted\"}}";
            }
            respond(socket, 200, "OK", QByteArray("{\"items\":[") +
                items + "]}");
        } else {
            respond(socket, 404, "Not Found", "{}");
        }
    }

    static void respond(QTcpSocket *socket, const int status,
                        const QByteArray &reason, const QByteArray &body,
                        const QList<QPair<QByteArray, QByteArray>> &extra = {}) {
        QByteArray response = "HTTP/1.1 " + QByteArray::number(status) + ' ' +
            reason + "\r\nContent-Type: application/json\r\nConnection: close\r\n";
        for (const auto &[name, value] : extra)
            response += name + ": " + value + "\r\n";
        response += "Content-Length: " + QByteArray::number(body.size()) +
            "\r\n\r\n" + body;
        socket->write(response);
        socket->disconnectFromHost();
    }

    QTcpServer server_;
    QHash<QTcpSocket *, QByteArray> buffers_;
    QStringList uploadedVideos_;
    int playlistCreates_ = 0;
    int sessionCreates_ = 0;
    int completedUploads_ = 0;
    int playlistInserts_ = 0;
    int processingChecks_ = 0;
    bool authorizationValid_ = true;
    bool metadataPrivate_ = true;
};

bool exerciseResultCardPreview(
    QPushButton *openButton, const QString &outputPath,
    const QStringList &requiredSummary,
    const QStringList &forbiddenSummary,
    const QString &previewScreenshot, QString *error) {
    if (!openButton || !openButton->isVisible() || !openButton->isEnabled()) {
        if (error) *error = QStringLiteral("result card button is unavailable");
        return false;
    }
    QFile::remove(outputPath);
    qputenv("VIDSTOREX_RESULT_CARD_TEST_OUTPUT", outputPath.toUtf8());
    bool handled = false;
    QString nestedError;
    QTimer interaction;
    interaction.setSingleShot(true);
    QObject::connect(&interaction, &QTimer::timeout, qApp, [&]() {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (!dialog || dialog->objectName() !=
                QStringLiteral("resultCardPreviewDialog")) {
            nestedError = QStringLiteral("result card preview did not open");
            if (dialog) dialog->reject();
            handled = true;
            return;
        }
        auto *preview = dialog->findChild<QLabel *>(
            "resultCardPreviewImage");
        auto *save = dialog->findChild<QPushButton *>(
            "resultCardSavePngButton");
        auto *copy = dialog->findChild<QPushButton *>(
            "resultCardCopyImageButton");
        auto *close = dialog->findChild<QPushButton *>(
            "resultCardCloseButton");
        if (!preview || !save || !copy || !close) {
            nestedError = QStringLiteral("result card preview controls are missing");
            dialog->reject();
            handled = true;
            return;
        }
        const QString summary = preview->accessibleDescription();
        for (const auto &required : requiredSummary) {
            if (!summary.contains(required, Qt::CaseInsensitive)) {
                nestedError = QStringLiteral("preview summary is missing: ") +
                    required + QStringLiteral("; actual: ") + summary;
                dialog->reject();
                handled = true;
                return;
            }
        }
        for (const auto &forbidden : forbiddenSummary) {
            if (summary.contains(forbidden, Qt::CaseInsensitive)) {
                nestedError = QStringLiteral("preview summary leaked: ") +
                    forbidden;
                dialog->reject();
                handled = true;
                return;
            }
        }
        if (!previewScreenshot.isEmpty()) dialog->grab().save(previewScreenshot);
        copy->click();
        if (QApplication::clipboard()->image().size() != QSize(1600, 900)) {
            nestedError = QStringLiteral("copied result card has wrong dimensions");
            dialog->reject();
            handled = true;
            return;
        }
        save->click();
        const QImage saved(outputPath);
        if (saved.size() != QSize(1600, 900)) {
            nestedError = QStringLiteral("saved result card has wrong dimensions");
            dialog->reject();
            handled = true;
            return;
        }
        handled = true;
        close->click();
    });
    QTimer fallback;
    fallback.setSingleShot(true);
    QObject::connect(&fallback, &QTimer::timeout, qApp, [&]() {
        if (handled) return;
        nestedError = QStringLiteral("result card preview interaction timed out");
        handled = true;
        if (auto *dialog = qobject_cast<QDialog *>(
                QApplication::activeModalWidget()))
            dialog->reject();
    });
    interaction.start(75);
    fallback.start(3000);
    openButton->click();
    interaction.stop();
    fallback.stop();
    qunsetenv("VIDSTOREX_RESULT_CARD_TEST_OUTPUT");
    if (!handled || !nestedError.isEmpty()) {
        if (error) *error = nestedError.isEmpty()
            ? QStringLiteral("result card preview did not complete")
            : nestedError;
        return false;
    }
    return true;
}
#endif

} // namespace

int main(int argc, char *argv[]) {
    bool smokeTest = false;
    bool closeDuringEstimate = false;
    QString preflightSmokeInput;
    QString preflightSmokeOutput;
    QString videoSetAssistantSmokeRoot;
    QString videoSetAssistantFakeYtDlp;
    QString instantRecoverySmokeRoot;
    QString instantRecoveryFakeYtDlp;
    QString instantRecoveryFixtureVideos;
    QString youtubeSyncSmokeRoot;
    QString youtubeSyncManifest;
    QString onboardingSmokeRoot;
    QString settingsMigrationSmokeRoot;
    for (int i = 1; i < argc; ++i) {
        const QString argument = QString::fromLocal8Bit(argv[i]);
        if (argument == "--smoke-test") {
            smokeTest = true;
        } else if (argument == "--close-during-estimate") {
            closeDuringEstimate = true;
        } else if (argument == "--preflight-smoke-input" &&
                   i + 1 < argc) {
            preflightSmokeInput =
                QString::fromLocal8Bit(argv[++i]);
        } else if (argument == "--preflight-smoke-output" &&
                   i + 1 < argc) {
            preflightSmokeOutput =
                QString::fromLocal8Bit(argv[++i]);
        } else if (argument == "--video-set-assistant-smoke-root" &&
                   i + 1 < argc) {
            videoSetAssistantSmokeRoot =
                QString::fromLocal8Bit(argv[++i]);
        } else if (argument == "--video-set-assistant-fake-ytdlp" &&
                   i + 1 < argc) {
            videoSetAssistantFakeYtDlp =
                QString::fromLocal8Bit(argv[++i]);
        } else if (argument == "--instant-recovery-smoke-root" &&
                   i + 1 < argc) {
            instantRecoverySmokeRoot = QString::fromLocal8Bit(argv[++i]);
        } else if (argument == "--instant-recovery-fake-ytdlp" &&
                   i + 1 < argc) {
            instantRecoveryFakeYtDlp = QString::fromLocal8Bit(argv[++i]);
        } else if (argument == "--instant-recovery-fixture-videos" &&
                   i + 1 < argc) {
            instantRecoveryFixtureVideos = QString::fromLocal8Bit(argv[++i]);
        } else if (argument == "--onboarding-smoke-root" && i + 1 < argc) {
            onboardingSmokeRoot = QString::fromLocal8Bit(argv[++i]);
        } else if (argument == "--settings-migration-smoke-root" &&
                   i + 1 < argc) {
            settingsMigrationSmokeRoot = QString::fromLocal8Bit(argv[++i]);
#ifdef VIDSTOREX_ENABLE_TEST_HOOKS
        } else if (argument == "--youtube-sync-smoke-root" && i + 1 < argc) {
            youtubeSyncSmokeRoot = QString::fromLocal8Bit(argv[++i]);
        } else if (argument == "--youtube-sync-manifest" && i + 1 < argc) {
            youtubeSyncManifest = QString::fromLocal8Bit(argv[++i]);
#endif
        }
    }
    const bool isolatedUiRun = !videoSetAssistantSmokeRoot.isEmpty() ||
        !instantRecoverySmokeRoot.isEmpty() || !youtubeSyncSmokeRoot.isEmpty() ||
        !onboardingSmokeRoot.isEmpty() ||
        !settingsMigrationSmokeRoot.isEmpty() || smokeTest ||
        closeDuringEstimate;
    if (isolatedUiRun) {
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(
            QSettings::IniFormat, QSettings::UserScope,
            !videoSetAssistantSmokeRoot.isEmpty()
                ? QDir(videoSetAssistantSmokeRoot).filePath("settings")
                : !onboardingSmokeRoot.isEmpty()
                ? QDir(onboardingSmokeRoot).filePath("settings")
                : !settingsMigrationSmokeRoot.isEmpty()
                ? QDir(settingsMigrationSmokeRoot).filePath("settings")
                : !instantRecoverySmokeRoot.isEmpty()
                ? QDir(instantRecoverySmokeRoot).filePath("settings")
                : !youtubeSyncSmokeRoot.isEmpty()
                ? QDir(youtubeSyncSmokeRoot).filePath("settings")
                : QDir(QDir::tempPath()).filePath(
                    "vidstorex-gui-smoke-settings"));
    }
    QApplication app(argc, argv);

#ifdef VIDSTOREX_ENABLE_TEST_HOOKS
    std::unique_ptr<FakeYouTubeApi> fakeYouTubeApi;
    if (!youtubeSyncSmokeRoot.isEmpty()) {
        fakeYouTubeApi = std::make_unique<FakeYouTubeApi>(&app);
        if (!fakeYouTubeApi->start()) {
            qCritical() << "YouTube Sync fake API could not listen";
            return 91;
        }
        qputenv("VIDSTOREX_YOUTUBE_API_BASE",
                fakeYouTubeApi->apiBase().toUtf8());
        qputenv("VIDSTOREX_YOUTUBE_UPLOAD_BASE",
                fakeYouTubeApi->uploadBase().toUtf8());
        qputenv("VIDSTOREX_CREDENTIALS_ROOT",
                QDir(youtubeSyncSmokeRoot).filePath("credentials").toUtf8());
    }
#endif
    
    // Set application properties and preserve every legacy user setting.
    QApplication::setOrganizationName(
        vidstorex::branding::kOrganizationName);
    QApplication::setOrganizationDomain("brandonli.me");
    if (!settingsMigrationSmokeRoot.isEmpty()) {
        QSettings legacy(
            QSettings::defaultFormat(), QSettings::UserScope,
            QString::fromLatin1(vidstorex::branding::kOrganizationName),
            QString::fromLatin1(
                vidstorex::branding::kLegacyApplicationName));
        QSettings target(
            QSettings::defaultFormat(), QSettings::UserScope,
            QString::fromLatin1(vidstorex::branding::kOrganizationName),
            QString::fromLatin1(vidstorex::branding::kProductName));
        legacy.clear();
        target.clear();
        legacy.setValue("ui/language", "tr");
        legacy.setValue("ui/onboardingVersion", 1);
        legacy.setValue("videoSet/recentManifests",
                        QStringList{"legacy-set-manifest"});
        legacy.setValue("ui/defaultVideoSetOutputFolder",
                        QDir(settingsMigrationSmokeRoot)
                            .filePath("legacy-output"));
        legacy.setValue("encoding/reliabilityProfileId", 1);
        target.setValue("ui/language", "en");
        legacy.sync();
        target.sync();
    }
    const int migratedSettings = vidstorex::branding::migrateLegacySettings();
    QApplication::setApplicationName(vidstorex::branding::kProductName);
    QApplication::setApplicationDisplayName(
        vidstorex::branding::kProductName);
    QApplication::setApplicationVersion(ms_version());
    if (migratedSettings > 0)
        qInfo() << "Preserved" << migratedSettings
                << "legacy settings during the Reliquary migration";
    if (isolatedUiRun) {
        QSettings settings;
        if (settingsMigrationSmokeRoot.isEmpty()) {
            settings.clear();
            settings.setValue("ui/language",
                              onboardingSmokeRoot.isEmpty() ? "en" : "tr");
            settings.setValue("ui/rememberRecentSets", true);
            settings.setValue("ui/showAdvancedTools", true);
            if (onboardingSmokeRoot.isEmpty()) {
                settings.setValue("ui/onboardingVersion", 1);
                settings.setValue("ui/brandIntroVersion", 1);
            } else {
                settings.setValue("ui/defaultVideoSetOutputFolder",
                                  QDir(onboardingSmokeRoot)
                                      .filePath("preserved-output"));
                settings.setValue("videoSet/recentManifests",
                                  QStringList{"preserved-recent-manifest"});
                settings.setValue("encoding/reliabilityProfileId", 1);
                settings.setValue("onboarding/stateSentinel",
                                  "preserve-user-state");
            }
            const QString fakeYtDlp = !videoSetAssistantFakeYtDlp.isEmpty()
                ? videoSetAssistantFakeYtDlp : instantRecoveryFakeYtDlp;
            if (!fakeYtDlp.isEmpty())
                settings.setValue("videoSet/ytdlpPath",
                                  fakeYtDlp);
#ifdef VIDSTOREX_ENABLE_TEST_HOOKS
            if (!youtubeSyncSmokeRoot.isEmpty()) {
                settings.setValue("videoSet/recentManifests",
                                  QStringList{youtubeSyncManifest});
                settings.setValue("youtube/connected", true);
                settings.setValue("youtube/defaultPrivacy", "unlisted");
                settings.setValue("youtube/privacyFriendlyTitles", true);
                settings.setValue("youtube/autoDownload", false);
                youtube_sync::TokenRecord token;
                token.access_token = "gui-e2e-access";
                token.refresh_token = "gui-e2e-refresh";
                token.expires_at_epoch_seconds =
                    QDateTime::currentSecsSinceEpoch() + 3600;
                auto store = youtube_sync::make_platform_credential_store();
                store->save("youtube-oauth",
                            youtube_sync::serialize_token_record(token));
            }
#endif
        }
    }
    
    app.setWindowIcon(vidStoreXApplicationIcon());
    
    // Enable high DPI scaling (deprecated in Qt6, but kept for compatibility)
    // app.setAttribute(Qt::AA_EnableHighDpiScaling);
    // app.setAttribute(Qt::AA_UseHighDpiPixmaps);
    
    // Set style to a modern look if available
    if (QStyleFactory::keys().contains("Fusion")) {
        QApplication::setStyle("Fusion");
    }

    if (!settingsMigrationSmokeRoot.isEmpty()) {
        auto *first = new DriveManagerUI();
        first->resize(1280, 720);
        first->show();
        QTimer::singleShot(0, &app, [&, first]() {
            auto *intro = first->findChild<QWidget *>("brandIntroOverlay");
            auto *home = first->findChild<QWidget *>(
                "videoSetAssistantWelcomePage");
            if (!intro || !intro->isVisible() ||
                intro->geometry() != first->rect()) {
                qCritical() << "Migrated legacy user did not receive brand intro";
                first->close();
                app.exit(127);
                return;
            }
            first->grab().save(QDir(settingsMigrationSmokeRoot)
                                   .filePath("settings-migration-intro.png"));
            QKeyEvent skip(QEvent::KeyPress, Qt::Key_Enter, Qt::NoModifier);
            QApplication::sendEvent(intro, &skip);
            QApplication::processEvents();
            const QSettings settings;
            if (!home || !home->isVisible() ||
                settings.value("ui/brandIntroVersion").toInt() != 1 ||
                settings.value("ui/onboardingVersion").toInt() != 1 ||
                settings.value("ui/language").toString() != "en" ||
                settings.value("videoSet/recentManifests").toStringList() !=
                    QStringList{"legacy-set-manifest"} ||
                settings.value("encoding/reliabilityProfileId").toInt() != 1 ||
                qApp->applicationName() != QStringLiteral("Reliquary") ||
                qApp->applicationDisplayName() != QStringLiteral("Reliquary") ||
                first->windowTitle() != QStringLiteral("Reliquary")) {
                qCritical() << "Legacy settings migration invariant failed";
                first->close();
                app.exit(128);
                return;
            }
            first->grab().save(QDir(settingsMigrationSmokeRoot)
                                   .filePath("settings-migration-home.png"));
            first->close();
            delete first;
            auto *second = new DriveManagerUI();
            second->resize(1280, 720);
            second->show();
            QApplication::processEvents();
            auto *secondIntro = second->findChild<QWidget *>(
                "brandIntroOverlay");
            auto *secondHome = second->findChild<QWidget *>(
                "videoSetAssistantWelcomePage");
            if (secondIntro || !secondHome || !secondHome->isVisible()) {
                qCritical() << "Migrated user restart did not open Home directly";
                second->close();
                app.exit(129);
                return;
            }
            second->close();
            qInfo() << "Legacy settings migration qwindows E2E complete";
            app.exit(0);
        });
        return app.exec();
    }

    if (!onboardingSmokeRoot.isEmpty()) {
        auto *first = new DriveManagerUI();
        first->resize(1280, 720);
        first->show();
        QTimer::singleShot(50, &app, [&, first]() {
            auto *intro = first->findChild<QWidget *>("brandIntroOverlay");
            auto *name = first->findChild<QLabel *>("brandIntroName");
            auto *definition = first->findChild<QLabel *>(
                "brandIntroDefinition");
            if (!intro || !intro->isVisible() || !name || !definition ||
                name->text() != QStringLiteral("RELIQUARY") ||
                definition->text() != QString::fromUtf8(
                    "Değerli bir şeyi korumak için kullanılan muhafaza.") ||
                definition->text().contains(QStringLiteral("veriniz"),
                                             Qt::CaseInsensitive)) {
                qCritical() << "Reliquary initial brand intro invariant failed";
                first->close();
                app.exit(108);
                return;
            }
            first->grab().save(QDir(onboardingSmokeRoot)
                                   .filePath("brand-intro-initial.png"));
        });
        QTimer::singleShot(900, &app, [&, first]() {
            auto *name = first->findChild<QLabel *>("brandIntroName");
            auto *definition = first->findChild<QLabel *>(
                "brandIntroDefinition");
            if (!name || !definition || !name->graphicsEffect() ||
                !definition->graphicsEffect() ||
                name->graphicsEffect()->property("opacity").toDouble() < 0.95 ||
                definition->graphicsEffect()->property("opacity").toDouble() > 0.05) {
                qCritical() << "Reliquary name did not precede the definition";
                first->close();
                app.exit(109);
                return;
            }
            first->grab().save(QDir(onboardingSmokeRoot)
                                   .filePath("brand-intro-name.png"));
        });
        QTimer::singleShot(1700, &app, [&, first]() {
            auto *name = first->findChild<QLabel *>("brandIntroName");
            auto *definition = first->findChild<QLabel *>(
                "brandIntroDefinition");
            if (!name || !definition || !name->graphicsEffect() ||
                !definition->graphicsEffect() ||
                name->graphicsEffect()->property("opacity").toDouble() < 0.95 ||
                definition->graphicsEffect()->property("opacity").toDouble() < 0.95) {
                qCritical() << "Reliquary definition fade-in invariant failed";
                first->close();
                app.exit(110);
                return;
            }
            for (const QSize size : {QSize(1280, 720), QSize(1366, 768),
                                     QSize(1920, 1080)}) {
                first->resize(size);
                QApplication::processEvents();
                if (definition->sizeHint().width() > definition->width()) {
                    qCritical() << "Reliquary definition clipped at" << size;
                    first->close();
                    app.exit(107);
                    return;
                }
                first->grab().save(QDir(onboardingSmokeRoot).filePath(
                    QStringLiteral("brand-intro-definition-%1x%2.png")
                        .arg(size.width()).arg(size.height())));
            }
        });
        QTimer::singleShot(3400, &app, [&, first]() {
            const auto fail = [&](const int code, const QString &message) {
                qCritical().noquote() << message;
                QFile diagnostic(QDir(onboardingSmokeRoot).filePath(
                    "onboarding-failure.txt"));
                if (diagnostic.open(QIODevice::WriteOnly | QIODevice::Text))
                    diagnostic.write(message.toUtf8());
                first->close();
                app.exit(code);
                return false;
            };
            auto *page = first->findChild<QWidget *>("onboardingPage");
            auto *stack = first->findChild<QStackedWidget *>("onboardingStack");
            auto *title1 = first->findChild<QLabel *>("onboardingTitle1");
            auto *title2 = first->findChild<QLabel *>("onboardingTitle2");
            auto *illustration1 = first->findChild<QWidget *>(
                "onboardingIllustration1");
            auto *back = first->findChild<QPushButton *>(
                "onboardingBackButton");
            auto *next = first->findChild<QPushButton *>(
                "onboardingNextButton");
            auto *skip = first->findChild<QPushButton *>(
                "onboardingSkipButton");
            auto *author = first->findChild<QLabel *>(
                "onboardingAuthorLabel");
            auto *linkedIn = first->findChild<QPushButton *>(
                "onboardingLinkedInButton");
            auto *language = first->findChild<QComboBox *>("uiLanguageCombo");
            if (!page || !stack || !title1 || !title2 || !illustration1 ||
                !back || !next || !skip || !author || !linkedIn ||
                !language || !page->isVisible() ||
                stack->currentIndex() != 0 || back->isVisible() ||
                title1->text() != QString::fromUtf8(
                    "Dosyanızı videolara dönüştürün") ||
                illustration1->size().isEmpty() ||
                !author->text().contains(
                    QString::fromUtf8("Burhan Talha Yazıcı")) ||
                linkedIn->property("externalUrl").toString() !=
                    QStringLiteral("https://www.linkedin.com/in/burhanbty")) {
                fail(111, QStringLiteral(
                    "Onboarding first-run page 1 invariant failed: "
                    "page=%1 index=%2 back=%3 title=%4 illustration=%5 "
                    "author=%6 url=%7")
                    .arg(page && page->isVisible())
                    .arg(stack ? stack->currentIndex() : -1)
                    .arg(back && back->isVisible())
                    .arg(title1 ? title1->text() : QStringLiteral("<missing>"))
                    .arg(illustration1
                        ? QStringLiteral("%1x%2")
                              .arg(illustration1->width())
                              .arg(illustration1->height())
                        : QStringLiteral("<missing>"))
                    .arg(author ? author->text() : QStringLiteral("<missing>"))
                    .arg(linkedIn ? linkedIn->property(
                        "externalUrl").toString() : QStringLiteral("<missing>")));
                return;
            }

            const QList<QSize> sizes{{1280, 720}, {1366, 768},
                                     {1600, 900}, {1920, 1080}};
            for (const auto &size : sizes) {
                first->resize(size);
                QApplication::processEvents();
                if (!title1->isVisible() || !illustration1->isVisible() ||
                    !next->isVisible() || !author->isVisible() ||
                    !linkedIn->isVisible() ||
                    !first->rect().contains(next->mapTo(
                        first, next->rect().center()))) {
                    fail(112, QStringLiteral(
                        "Onboarding layout clipped at %1x%2")
                        .arg(size.width()).arg(size.height()));
                    return;
                }
                first->grab().save(QDir(onboardingSmokeRoot).filePath(
                    QStringLiteral("onboarding-%1x%2.png")
                        .arg(size.width()).arg(size.height())));
            }

            QPalette light;
            light.setColor(QPalette::Window, QColor("#F4F1EB"));
            light.setColor(QPalette::Base, QColor("#FFFDF8"));
            light.setColor(QPalette::WindowText, QColor("#24211D"));
            app.setPalette(light);
            first->setPalette(light);
            vidstorex_ui::applyTheme(first->QMainWindow::centralWidget());
            QApplication::processEvents();
            first->grab().save(QDir(onboardingSmokeRoot)
                                   .filePath("onboarding-light.png"));
            QPalette dark;
            dark.setColor(QPalette::Window, QColor("#20201E"));
            dark.setColor(QPalette::Base, QColor("#181816"));
            dark.setColor(QPalette::WindowText, QColor("#F1EEE7"));
            app.setPalette(dark);
            first->setPalette(dark);
            vidstorex_ui::applyTheme(first->QMainWindow::centralWidget());
            QApplication::processEvents();
            first->grab().save(QDir(onboardingSmokeRoot)
                                   .filePath("onboarding-dark.png"));

            next->click();
            QApplication::processEvents();
            if (stack->currentIndex() != 1 || !back->isVisible() ||
                title2->text() != QString::fromUtf8("Videoları saklayın")) {
                fail(113, "Onboarding page 2 navigation failed");
                return;
            }
            first->grab().save(QDir(onboardingSmokeRoot)
                                   .filePath("onboarding-page-2.png"));
            language->setCurrentIndex(language->findData("en"));
            QApplication::processEvents();
            if (stack->currentIndex() != 1 ||
                title2->text() != "Store the videos") {
                fail(114, "Onboarding TR to EN switch lost the active page");
                return;
            }
            language->setCurrentIndex(language->findData("tr"));
            QApplication::processEvents();
            if (stack->currentIndex() != 1 ||
                title2->text() != QString::fromUtf8("Videoları saklayın")) {
                fail(115, "Onboarding EN to TR switch lost the active page");
                return;
            }
            next->click();
            QApplication::processEvents();
            if (stack->currentIndex() != 2 ||
                next->text() != QString::fromUtf8(
                    "Reliquary'yi Kullanmaya Başla")) {
                fail(116, "Onboarding page 3/start action failed");
                return;
            }
            first->grab().save(QDir(onboardingSmokeRoot)
                                   .filePath("onboarding-page-3.png"));
            back->click();
            QApplication::processEvents();
            if (stack->currentIndex() != 1) {
                fail(117, "Onboarding Back failed");
                return;
            }
            next->click();
            next->click();
            QApplication::processEvents();
            auto *home = first->findChild<QWidget *>(
                "videoSetAssistantWelcomePage");
            const QSettings completed;
            if (!home || !home->isVisible() ||
                completed.value("ui/onboardingVersion").toInt() != 1 ||
                completed.value("ui/brandIntroVersion").toInt() != 1 ||
                completed.value("onboarding/stateSentinel").toString() !=
                    "preserve-user-state" ||
                completed.value("videoSet/recentManifests").toStringList() !=
                    QStringList{"preserved-recent-manifest"}) {
                fail(118, "Onboarding completion/state preservation failed");
                return;
            }

            first->close();
            delete first;
            auto *second = new DriveManagerUI();
            second->resize(1280, 720);
            second->show();
            QApplication::processEvents();
            auto *secondOnboarding = second->findChild<QWidget *>(
                "onboardingPage");
            auto *secondHome = second->findChild<QWidget *>(
                "videoSetAssistantWelcomePage");
            if (!secondOnboarding || !secondHome ||
                secondOnboarding->isVisible() || !secondHome->isVisible()) {
                qCritical() << "Completed onboarding did not restart at Home";
                second->close();
                app.exit(119);
                return;
            }
            auto *settingsNav = second->findChild<QPushButton *>(
                "settingsNavigationButton");
            auto *reopen = second->findChild<QPushButton *>(
                "settingsShowGettingStartedButton");
            settingsNav->click();
            QApplication::processEvents();
            if (!reopen->isEnabled()) {
                qCritical() << "Settings onboarding action is unavailable";
                second->close();
                app.exit(120);
                return;
            }
            reopen->click();
            QApplication::processEvents();
            auto *secondSkip = second->findChild<QPushButton *>(
                "onboardingSkipButton");
            if (!secondOnboarding->isVisible() ||
                second->property("uiLanguage").toString() != "tr") {
                qCritical() << "Settings did not reopen onboarding safely";
                second->close();
                app.exit(121);
                return;
            }
            secondSkip->click();
            QApplication::processEvents();
            auto *helpAction = second->findChild<QAction *>(
                "gettingStartedAction");
            helpAction->trigger();
            QApplication::processEvents();
            if (!secondOnboarding->isVisible()) {
                qCritical() << "Help menu did not reopen onboarding";
                second->close();
                app.exit(122);
                return;
            }
            secondSkip->click();
            second->close();
            delete second;

            QSettings().setValue("ui/brandIntroVersion", 0);
            QSettings().setValue("ui/onboardingVersion", 1);
            auto *legacyUser = new DriveManagerUI();
            legacyUser->resize(1280, 720);
            legacyUser->show();
            QApplication::processEvents();
            auto *legacyIntro = legacyUser->findChild<QWidget *>(
                "brandIntroOverlay");
            auto *legacyHome = legacyUser->findChild<QWidget *>(
                "videoSetAssistantWelcomePage");
            if (!legacyIntro || !legacyIntro->isVisible() ||
                legacyIntro->geometry() != legacyUser->rect()) {
                qCritical() << "Legacy user did not receive one brand intro";
                legacyUser->close();
                app.exit(125);
                return;
            }
            QKeyEvent skipIntro(QEvent::KeyPress, Qt::Key_Space,
                                Qt::NoModifier);
            QApplication::sendEvent(legacyIntro, &skipIntro);
            QApplication::processEvents();
            if (!legacyHome || !legacyHome->isVisible() ||
                QSettings().value("ui/brandIntroVersion").toInt() != 1) {
                qCritical() << "Brand intro keyboard skip did not open Home";
                legacyUser->close();
                app.exit(126);
                return;
            }
            legacyUser->close();
            delete legacyUser;

            QSettings().setValue("ui/onboardingVersion", 0);
            auto *versionZero = new DriveManagerUI();
            versionZero->show();
            QApplication::processEvents();
            auto *zeroPage = versionZero->findChild<QWidget *>(
                "onboardingPage");
            auto *zeroSkip = versionZero->findChild<QPushButton *>(
                "onboardingSkipButton");
            if (!zeroPage || !zeroPage->isVisible()) {
                qCritical() << "onboardingVersion 0 did not show onboarding";
                versionZero->close();
                app.exit(123);
                return;
            }
            zeroSkip->click();
            QApplication::processEvents();
            if (QSettings().value("ui/onboardingVersion").toInt() != 1) {
                qCritical() << "Skip did not complete onboarding version 1";
                versionZero->close();
                app.exit(124);
                return;
            }
            versionZero->close();
            qInfo() << "Onboarding qwindows E2E complete at scale"
                    << qApp->devicePixelRatio();
            app.exit(0);
        });
        return app.exec();
    }
    
    // Create and show the main window
    DriveManagerUI window;
    window.show();

#ifdef VIDSTOREX_ENABLE_TEST_HOOKS
    if (!youtubeSyncSmokeRoot.isEmpty()) {
        auto *recent = window.findChild<QListWidget *>("videoSetRecentList");
        auto *continueButton = window.findChild<QPushButton *>(
            "videoSetRecentContinueButton");
        auto *sync = window.findChild<QPushButton *>("youtubeSyncStartButton");
        auto *status = window.findChild<QLabel *>("youtubeSyncStatus");
        auto *progress = window.findChild<QProgressBar *>("youtubeSyncProgress");
        auto *experimentalAction = window.findChild<QAction *>(
            "advancedYouTubeSyncAction");
        auto *experimentalPage = window.findChild<QWidget *>(
            "experimentalYouTubeSyncPage");
        if (!recent || !continueButton || !sync || !status || !progress ||
            !experimentalAction || !experimentalPage || recent->count() != 1) {
            qCritical() << "YouTube Sync GUI controls were not found";
            return 92;
        }
        recent->setCurrentRow(0);
        continueButton->click();
        QApplication::processEvents();
        experimentalAction->trigger();
        QApplication::processEvents();
        if (!sync->isEnabled()) {
            qCritical() << "YouTube Sync did not open a verified recent set";
            return 93;
        }
        if (!experimentalPage->isVisible() || !sync->isVisible()) {
            qCritical() << "YouTube Sync is not isolated under Experimental";
            return 97;
        }
        sync->click();
        auto *timer = new QTimer(&window);
        timer->setInterval(50);
        auto *deadline = new qint64(
            QDateTime::currentMSecsSinceEpoch() + 120000);
        QObject::connect(timer, &QTimer::timeout, &window,
            [&, timer, deadline, status, progress]() {
            const QString sidecar = QDir(QFileInfo(youtubeSyncManifest)
                .absolutePath()).filePath("youtube_sync_state.json");
            if (status->text().contains("playlist is ready",
                                        Qt::CaseInsensitive)) {
                try {
                    const auto state = youtube_sync::read_sync_state(
                        std::filesystem::path(sidecar.toStdWString()));
                    const bool partsReady = !state.parts.empty() &&
                        std::all_of(state.parts.begin(), state.parts.end(),
                            [](const youtube_sync::PartState &part) {
                            return part.upload_state ==
                                       youtube_sync::UploadState::Uploaded &&
                                part.processing_state ==
                                       youtube_sync::ProcessingState::Succeeded &&
                                !part.youtube_video_id.empty() &&
                                !part.playlist_item_id.empty();
                        });
                    if (!partsReady || !state.playlist_created ||
                        state.actual_privacy != youtube_sync::Privacy::Unlisted ||
                        progress->value() != 100 ||
                        !fakeYouTubeApi->complete(
                            static_cast<int>(state.parts.size()))) {
                        qCritical() << "YouTube Sync GUI state invariant failed";
                        app.exit(94);
                    } else {
                        window.grab().save(QDir(youtubeSyncSmokeRoot)
                            .filePath("youtube-sync-ready.png"));
                        qInfo() << "YouTube Sync qwindows E2E complete";
                        app.exit(0);
                    }
                } catch (const std::exception &error) {
                    qCritical() << "YouTube Sync sidecar validation failed:"
                                << error.what();
                    app.exit(95);
                }
                timer->stop();
                window.close();
                delete deadline;
            } else if (QDateTime::currentMSecsSinceEpoch() > *deadline) {
                qCritical() << "YouTube Sync GUI E2E timeout:" << status->text();
                timer->stop();
                window.close();
                app.exit(96);
                delete deadline;
            }
        });
        timer->start();
    }
#endif

    if (!instantRecoverySmokeRoot.isEmpty()) {
#ifdef Q_OS_WIN
        if (QGuiApplication::platformName() != QStringLiteral("windows")) {
            qCritical() << "Instant Recovery result card E2E requires qwindows";
            return 80;
        }
#endif
        qputenv("VIDSTOREX_FAKE_YTDLP_SOURCE",
                instantRecoveryFixtureVideos.toUtf8());
        qputenv("VIDSTOREX_FAKE_YTDLP_DELAY_MS", "1200");
        qputenv("VIDSTOREX_RECOVERY_JOBS_ROOT",
                QDir(instantRecoverySmokeRoot).filePath(
                    "RecoveryJobs").toUtf8());
        // Ensure the deterministic adapter selected in isolated settings wins
        // over any developer-machine yt-dlp installation.
        qputenv("PATH", QByteArray{});
        auto *recoverNavigation = window.findChild<QPushButton *>(
            "recoverNavigationButton");
        auto *playlist = window.findChild<QLineEdit *>("instantPlaylistUrl");
        auto *output = window.findChild<QLineEdit *>(
            "videoSetAssistantRecoveryOutput");
        auto *start = window.findChild<QPushButton *>(
            "instantPlaylistRecoverButton");
        auto *status = window.findChild<QLabel *>(
            "instantPlaylistRecoveryStatus");
        auto *success = window.findChild<QLabel *>(
            "videoSetAssistantExactSuccess");
        auto *resultCard = window.findChild<QPushButton *>(
            "videoSetRecoveryResultCardButton");
        auto *activityPanel = window.findChild<QFrame *>(
            "videoSetActivityPanel");
        auto *activityTitle = window.findChild<QLabel *>(
            "videoSetActivityTitle");
        auto *activityDescription = window.findChild<QLabel *>(
            "videoSetActivityDescription");
        auto *activityFlow = static_cast<VidStoreXProcessingFlow *>(
            window.findChild<QWidget *>("videoSetLiveDataPath"));
        auto *activityDetailsButton = window.findChild<QToolButton *>(
            "videoSetActivityDetailsToggle");
        auto *activityDetails = window.findChild<QWidget *>(
            "videoSetActivityDetails");
        auto *applicationHeader = window.findChild<QFrame *>(
            "applicationHeader");
        auto *workflowStepper = static_cast<VidStoreXStepper *>(
            window.findChild<QWidget *>("videoSetAssistantStepper"));
        auto *language = window.findChild<QComboBox *>("uiLanguageCombo");
        if (!recoverNavigation || !playlist || !output || !start ||
            !status || !success || !resultCard || !activityPanel || !activityTitle ||
            !activityDescription || !activityFlow || !activityDetailsButton ||
            !activityDetails || !applicationHeader || !workflowStepper ||
            !language) {
            qCritical() << "Instant Recovery widgets were not found";
            return 81;
        }
        const QSettings instantSettings;
        if (instantSettings.value("youtube/connected", false).toBool() ||
            !instantSettings.value(
                "youtube/oauthClientConfigPath").toString().isEmpty()) {
            qCritical() << "Instant Recovery test is not OAuth-independent";
            return 85;
        }
        const QString recovered = QDir(instantRecoverySmokeRoot)
            .filePath("recovered-instant");
        QDir().mkpath(recovered);
        language->setCurrentIndex(language->findData("tr"));
        QApplication::processEvents();
        recoverNavigation->click();
        output->setText(recovered);
        playlist->setText(
            "https://www.youtube.com/playlist?list=PL_FAKE_E2E&feature=share");
        QApplication::processEvents();
        if (!start->isEnabled()) {
            qCritical() << "Instant Recovery start button remained disabled:"
                        << status->text();
            return 84;
        }
        start->click();
        auto *timer = new QTimer(&window);
        timer->setInterval(100);
        auto *elapsed = new int(0);
        auto *observedPhases = new QSet<QString>();
        QObject::connect(timer, &QTimer::timeout, &window,
            [&app, &window, status, success, recovered, timer, elapsed,
             observedPhases, activityPanel, activityTitle,
             activityDescription, activityFlow, instantRecoverySmokeRoot,
             resultCard, activityDetailsButton, activityDetails,
             applicationHeader, workflowStepper, playlist, output, start]() {
            *elapsed += 100;
            const auto savePhase = [&](const QString &phase,
                                       const QString &filename) {
                if (observedPhases->contains(phase)) return;
                observedPhases->insert(phase);
                window.grab().save(QDir(instantRecoverySmokeRoot)
                    .filePath(filename));
            };
            if (activityPanel->property("observedDownload").toBool() &&
                activityFlow->mode() ==
                    VidStoreXProcessingFlow::Mode::Download &&
                !observedPhases->contains("download")) {
                const auto substantiallyVisible = [&window](QWidget *widget) {
                    const QRect rect(widget->mapTo(&window, QPoint()),
                                     widget->size());
                    const QRect visible = rect.intersected(window.rect());
                    return widget->isVisible() && visible.width() > 0 &&
                        visible.height() >= qMax(1, widget->height() / 2);
                };
                const QList<QPair<QSize, QString>> sizes{
                    {{1366, 768}, "1366x768"},
                    {{1600, 900}, "1600x900"}};
                for (const auto &[size, suffix] : sizes) {
                    window.resize(size);
                    QApplication::processEvents();
                    if (applicationHeader->height() > 76 ||
                        workflowStepper->height() > 50 ||
                        activityPanel->height() >= window.height() / 4 ||
                        activityDetailsButton->isChecked() ||
                        activityDetails->isVisible() ||
                        activityFlow->isVisible() ||
                        !substantiallyVisible(playlist) ||
                        !substantiallyVisible(output) ||
                        !substantiallyVisible(start) ||
                        !window.grab().save(QDir(instantRecoverySmokeRoot)
                            .filePath("e2e-recover-active-download-" +
                                      suffix + ".png"))) {
                        qCritical() << "Instant Recovery active download "
                                       "layout audit failed" << suffix;
                        app.exit(130); timer->stop(); return;
                    }
                }
                observedPhases->insert("download");
                window.grab().save(QDir(instantRecoverySmokeRoot)
                    .filePath("instant-download.png"));
                auto *language = window.findChild<QComboBox *>("uiLanguageCombo");
                if (language)
                    language->setCurrentIndex(language->findData("en"));
            }
            if (activityPanel->property("observedScan").toBool() &&
                activityFlow->mode() == VidStoreXProcessingFlow::Mode::Scan) {
                if (!activityDescription->text().contains(
                        "not being rebuilt yet", Qt::CaseInsensitive)) {
                    qCritical() << "Instant scan implied file rebuilding";
                    app.exit(86); timer->stop(); return;
                }
                savePhase("scan", "instant-scan.png");
            }
            if (activityPanel->property("observedRecovery").toBool() &&
                activityFlow->mode() == VidStoreXProcessingFlow::Mode::Recover)
                savePhase("recover", "instant-recover.png");
            if (activityPanel->property("observedFinalHash").toBool() &&
                (activityFlow->mode() == VidStoreXProcessingFlow::Mode::Verify ||
                 activityTitle->text().contains("verification",
                                                Qt::CaseInsensitive)))
                savePhase("verify", "instant-verify.png");
            if (success->isVisible() && success->text().contains(
                    "recovered exactly", Qt::CaseInsensitive)) {
                if (!QFileInfo::exists(QDir(recovered).filePath("source.bin")) ||
                    !observedPhases->contains("download") ||
                    !observedPhases->contains("scan") ||
                    !observedPhases->contains("recover") ||
                    !activityPanel->property("observedFinalHash").toBool()) {
                    qCritical() << "Instant Recovery exact output is missing";
                    app.exit(82);
                } else {
                    QString cardError;
                    const QString cardPath = QDir(instantRecoverySmokeRoot)
                        .filePath("instant-result-card.png");
                    if (!exerciseResultCardPreview(
                            resultCard, cardPath,
                            {QStringLiteral("source.bin"),
                             QStringLiteral("High Capacity"),
                             QStringLiteral("YouTube Round-Trip"),
                             QStringLiteral("SHA-256"),
                             QStringLiteral("Match")},
                            {QDir::fromNativeSeparators(instantRecoverySmokeRoot),
                             QStringLiteral("playlist?list="),
                             QStringLiteral("PL_FAKE_E2E")},
                            QDir(instantRecoverySmokeRoot).filePath(
                                "instant-result-card-preview.png"),
                            &cardError)) {
                        qCritical() << "Instant Recovery result card failed:"
                                    << cardError;
                        QFile diagnostics(QDir(instantRecoverySmokeRoot)
                            .filePath("instant-card-error.txt"));
                        if (diagnostics.open(
                                QIODevice::WriteOnly | QIODevice::Text))
                            diagnostics.write(cardError.toUtf8());
                        app.exit(87);
                        timer->stop();
                        window.close();
                        delete elapsed;
                        delete observedPhases;
                        return;
                    }
                    qInfo() << "Instant Recovery qwindows E2E complete:"
                            << status->text();
                    app.exit(0);
                }
                timer->stop();
                window.close();
                delete elapsed;
                delete observedPhases;
            } else if (*elapsed > 120000) {
                QFile diagnostics(QDir(recovered).filePath(
                    "instant-e2e-status.txt"));
                if (diagnostics.open(QIODevice::WriteOnly | QIODevice::Text))
                    diagnostics.write(status->text().toUtf8());
                qCritical() << "Instant Recovery E2E timeout:" << status->text();
                timer->stop();
                window.close();
                app.exit(83);
                delete elapsed;
                delete observedPhases;
            }
        });
        timer->start();
    }

    if (smokeTest) {
        auto *profiles = window.findChild<QComboBox *>(
            "reliabilityProfileCombo");
        auto *repair = window.findChild<QDoubleSpinBox *>(
            "repairPercentSpinBox");
        auto *help = window.findChild<QLabel *>(
            "reliabilityHelpLabel");
        auto *videoSetValidation = window.findChild<QLabel *>(
            "videoSetValidationLabel");
        if (!profiles || !repair || !help || !videoSetValidation) {
            qCritical() << "profile controls were not found";
            return 2;
        }
        if (!videoSetValidation->text().contains("6/6") ||
            !videoSetValidation->text().contains("4/4") ||
            !videoSetValidation->text().contains("SHA-256")) {
            qCritical() << "Video Set validation notice invariant failed";
            return 2;
        }
        auto *assistantStack = window.findChild<QStackedWidget *>(
            "videoSetAssistantStack");
        auto *assistantScroll = window.findChild<QScrollArea *>(
            "videoSetAssistantScrollArea");
        auto *createChoice = window.findChild<QPushButton *>(
            "videoSetAssistantCreateChoice");
        auto *recoverChoice = window.findChild<QPushButton *>(
            "videoSetAssistantRecoverChoice");
        auto *resilientChoice = window.findChild<QRadioButton *>(
            "videoSetResilientChoice");
        auto *highCapacityChoice = window.findChild<QRadioButton *>(
            "videoSetHighCapacityChoice");
        auto *advancedToggle = window.findChild<QToolButton *>(
            "videoSetAdvancedSettingsToggle");
        auto *advancedPanel = window.findChild<QWidget *>(
            "videoSetAdvancedSettingsPanel");
        auto *classicTools = window.findChild<QGroupBox *>(
            "videoSetClassicTools");
        auto *activityPanel = window.findChild<QFrame *>(
            "videoSetActivityPanel");
        auto *activityTitle = window.findChild<QLabel *>(
            "videoSetActivityTitle");
        auto *activityProgress = static_cast<VidStoreXBlockProgress *>(
            window.findChild<QWidget *>("videoSetBlockProgress"));
        auto *activityFlow = static_cast<VidStoreXProcessingFlow *>(
            window.findChild<QWidget *>("videoSetLiveDataPath"));
        auto *activityDetailsButton = window.findChild<QToolButton *>(
            "videoSetActivityDetailsToggle");
        auto *activityDetails = window.findChild<QWidget *>(
            "videoSetActivityDetails");
        auto *activityParts = static_cast<VidStoreXPartGrid *>(
            window.findChild<QWidget *>("videoSetPartGrid"));
        auto *technicalToggle = window.findChild<QToolButton *>(
            "videoSetTechnicalLogToggle");
        auto *technicalLog = window.findChild<QTextEdit *>(
            "videoSetTechnicalLog");
        auto *homeNavigation = window.findChild<QPushButton *>(
            "homeNavigationButton");
        auto *settingsNavigation = window.findChild<QPushButton *>(
            "settingsNavigationButton");
        auto *settingsAuthor = window.findChild<QLabel *>(
            "settingsAboutAuthor");
        auto *settingsLinkedIn = window.findChild<QPushButton *>(
            "settingsLinkedInButton");
        auto *language = window.findChild<QComboBox *>(
            "uiLanguageCombo");
        auto *settingsLanguage = window.findChild<QComboBox *>(
            "settingsLanguageCombo");
        auto *settingsPage = window.findChild<QWidget *>("settingsPage");
        auto *advancedNavigation = window.findChild<QToolButton *>(
            "advancedNavigationButton");
        auto *trustLabel = window.findChild<QLabel *>(
            "videoSetTrustLabel");
        auto *homeHeading = window.findChild<QLabel *>(
            "videoSetPageHeading0");
        auto *createRail = window.findChild<QWidget *>(
            "videoSetCreateSignalRail");
        auto *recoverRail = window.findChild<QWidget *>(
            "videoSetRecoverSignalRail");
        auto *stepper = window.findChild<QWidget *>(
            "videoSetAssistantStepper");
        auto *recentEmpty = window.findChild<QFrame *>(
            "videoSetRecentEmptyState");
        auto *createCard = window.findChild<QFrame *>("videoSetCreateCard");
        auto *recoverCard = window.findChild<QFrame *>("videoSetRecoverCard");
        auto *createCardTitle = window.findChild<QLabel *>(
            "videoSetCreateCardTitle");
        auto *recoverCardTitle = window.findChild<QLabel *>(
            "videoSetRecoverCardTitle");
        auto *recentGroup = window.findChild<QFrame *>("videoSetRecentGroup");
        auto *classicAction = window.findChild<QAction *>(
            "advancedClassicVideoSetAction");
        auto *youtubeSyncAction = window.findChild<QAction *>(
            "advancedYouTubeSyncAction");
        auto *youtubeSyncPage = window.findChild<QWidget *>(
            "experimentalYouTubeSyncPage");
        auto *oauthConfig = window.findChild<QLineEdit *>(
            "youtubeOAuthConfigPath");
        auto *youtubeSyncCard = window.findChild<QFrame *>(
            "youtubeSyncCard");
        if (!assistantStack || !assistantScroll || !createChoice ||
            !recoverChoice || !resilientChoice || !highCapacityChoice ||
            !advancedToggle || !advancedPanel || !classicTools ||
            !activityPanel || !activityTitle || !activityProgress ||
            !activityFlow || !activityDetailsButton || !activityDetails ||
            !activityParts ||
            !technicalToggle || !technicalLog || !homeNavigation ||
            !settingsNavigation || !language || !settingsLanguage ||
            !settingsPage || !advancedNavigation || !trustLabel ||
            !homeHeading || !createRail || !recoverRail || !stepper ||
            !recentEmpty || !classicAction || !youtubeSyncAction ||
            !youtubeSyncPage || !oauthConfig || !youtubeSyncCard ||
            !createCard || !recoverCard ||
            !createCardTitle || !recoverCardTitle || !recentGroup) {
            qCritical() << "Video Set Assistant controls were not found";
            return 6;
        }
        window.resize(1280, 720);
        QApplication::processEvents();
        if (assistantStack->currentIndex() != 0 ||
            !assistantScroll->widgetResizable() ||
            window.minimumHeight() > 720 ||
            createChoice->focusPolicy() == Qt::NoFocus ||
            recoverChoice->focusPolicy() == Qt::NoFocus ||
            assistantScroll->isAncestorOf(activityPanel) ||
            activityDetailsButton->isChecked() ||
            !activityDetails->isHidden() ||
            technicalLog->document()->maximumBlockCount() != 5000 ||
            homeHeading->text() == QStringLiteral("Reliquary") ||
            (!homeHeading->text().contains("safely") &&
             !homeHeading->text().contains(QString::fromUtf8("güvenle"))) ||
            !trustLabel->text().contains("YouTube") ||
            createRail->height() < 1 || recoverRail->height() < 1 ||
            createRail->width() > 100 || recoverRail->width() > 100 ||
            createCardTitle->geometry().top() <= 0 ||
            recoverCardTitle->geometry().top() <= 0 ||
            qAbs(createCard->height() - recoverCard->height()) > 1 ||
            !recentEmpty->isVisible() ||
            (advancedNavigation->text() != QStringLiteral("Advanced") &&
             advancedNavigation->text() != QString::fromUtf8("Gelişmiş"))) {
            qCritical() << "Video Set Assistant welcome/scroll/focus invariant failed";
            return 6;
        }
        for (auto *label : assistantStack->currentWidget()
                               ->findChildren<QLabel *>()) {
            if (label->isVisible() &&
                (label->text().contains("YouTube Sync", Qt::CaseInsensitive) ||
                 label->text().contains("OAuth", Qt::CaseInsensitive) ||
                 label->text().contains("Google Cloud", Qt::CaseInsensitive))) {
                qCritical() << "Consumer Home exposes experimental setup text"
                            << label->text();
                return 20;
            }
        }
        window.resize(1920, 1080);
        QApplication::processEvents();
        const int heroContentWidth = recoverCard->geometry().right() -
            createCard->geometry().left() + 1;
        if (assistantScroll->verticalScrollBar()->isVisible() ||
            !window.statusBar()->isHidden() || qApp->windowIcon().isNull() ||
            heroContentWidth > 1450) {
            qCritical() << "Consumer Home sizing, status bar, or app icon invariant failed"
                        << "window" << window.size()
                        << "hero" << heroContentWidth
                        << "scroll" << assistantScroll->verticalScrollBar()->isVisible();
            return 19;
        }
        window.resize(1280, 720);
        QApplication::processEvents();
        int strongHomeActions = 0;
        for (auto *button : assistantStack->currentWidget()
                                ->findChildren<QPushButton *>()) {
            if (button->isVisible() &&
                button->property("vsxRole").toString() ==
                    QStringLiteral("primary"))
                ++strongHomeActions;
        }
        int visibleBrandLabels = 0;
        for (auto *label : window.findChildren<QLabel *>())
            if (label->isVisible() && label->text() == QStringLiteral("Reliquary"))
                ++visibleBrandLabels;
        if (strongHomeActions != 2 || visibleBrandLabels != 1 ||
            createChoice->property("vsxRole").toString() !=
                QStringLiteral("primary") ||
            !settingsNavigation->property("nav").toBool()) {
            qCritical() << "Visual hierarchy or single-brand invariant failed"
                        << strongHomeActions << visibleBrandLabels;
            return 18;
        }
        const QList<QSize> supportedSizes{
            QSize(1280, 720), QSize(1366, 768), QSize(1920, 1080)};
        for (const auto &size : supportedSizes) {
            window.resize(size);
            QApplication::processEvents();
            for (auto *button : {homeNavigation, settingsNavigation,
                                 createChoice, recoverChoice}) {
                if (button->isVisible() &&
                    (button->width() < button->minimumSizeHint().width() ||
                     button->height() < button->minimumSizeHint().height())) {
                    qCritical() << "Critical control is clipped at" << size
                                << button->objectName() << "actual"
                                << button->size() << "minimum hint"
                                << button->minimumSizeHint();
                    return 14;
                }
            }
        }
        QSet<QString> customObjectNames;
        const QStringList customPrefixes{
            "videoSet", "homeNavigation", "createNavigation",
            "recoverNavigation", "recentNavigation", "advancedNavigation",
            "settingsNavigation", "brand", "uiLanguage",
            "defaultVideoSet", "rememberRecent", "showAdvanced"};
        for (auto *widget : window.findChildren<QWidget *>()) {
            const QString name = widget->objectName();
            bool custom = false;
            for (const auto &prefix : customPrefixes)
                custom = custom || name.startsWith(prefix);
            if (!custom || name.isEmpty()) continue;
            if (customObjectNames.contains(name)) {
                qCritical() << "Duplicate custom objectName" << name;
                return 15;
            }
            customObjectNames.insert(name);
        }
        if (createChoice->accessibleName().isEmpty() ||
            createChoice->accessibleDescription().isEmpty() ||
            recoverChoice->accessibleName().isEmpty() ||
            recoverChoice->accessibleDescription().isEmpty() ||
            homeNavigation->accessibleName().isEmpty()) {
            qCritical() << "Critical accessibility metadata is missing";
            return 16;
        }
        settingsNavigation->click();
        QApplication::processEvents();
        if (!settingsPage->isVisible() ||
            settingsLanguage->currentData() != language->currentData() ||
            settingsPage->isAncestorOf(oauthConfig) || oauthConfig->isVisible()) {
            qCritical() << "Settings page language selector is not synchronized";
            return 17;
        }
        youtubeSyncAction->trigger();
        QApplication::processEvents();
        if (!youtubeSyncPage->isVisible() || !oauthConfig->isVisible() ||
            !youtubeSyncCard->isVisible()) {
            qCritical() << "Experimental YouTube Sync page is incomplete";
            return 21;
        }
        homeNavigation->click();
        QApplication::processEvents();
        createChoice->click();
        QApplication::processEvents();
        if (assistantStack->currentIndex() != 1 ||
            resilientChoice->isChecked() || !highCapacityChoice->isChecked()) {
            qCritical() << "Create flow or High Capacity default invariant failed";
            return 7;
        }
        advancedToggle->setChecked(false);
        technicalToggle->setChecked(true);
        QApplication::processEvents();
        if (technicalLog->isHidden()) {
            qCritical() << "Technical Video Set log did not expand";
            return 12;
        }
        technicalToggle->setChecked(false);
        QApplication::processEvents();
        if (!technicalLog->isHidden()) {
            qCritical() << "Technical Video Set log did not collapse";
            return 13;
        }
        QApplication::processEvents();
        if (!advancedPanel->isHidden()) {
            qCritical() << "Advanced settings did not collapse";
            return 9;
        }
        advancedToggle->setChecked(true);
        QApplication::processEvents();
        if (advancedPanel->isHidden()) {
            qCritical() << "Advanced settings did not expand";
            return 10;
        }
        advancedToggle->setChecked(false);
        classicAction->trigger();
        QApplication::processEvents();
        if (classicTools->isHidden() || classicTools->isCheckable()) {
            qCritical() << "Classic Video Set tools are inaccessible";
            return 11;
        }
        recoverChoice->click();
        QApplication::processEvents();
        if (assistantStack->currentIndex() != 7) {
            qCritical() << "Recover flow did not open shared scan page";
            return 8;
        }
        int highCapacityIndex = -1;
        int customIndex = -1;
        int resilientIndex = -1;
        int highCapacityCount = 0;
        for (int index = 0; index < profiles->count(); ++index) {
            if (profiles->itemData(index).toInt() ==
                    static_cast<int>(ReliabilityProfile::HighCapacity)) {
                highCapacityIndex = index;
                ++highCapacityCount;
            }
            if (profiles->itemData(index).toInt() < 0)
                customIndex = index;
            if (profiles->itemData(index).toInt() ==
                    static_cast<int>(ReliabilityProfile::Local))
                resilientIndex = index;
        }
        if (highCapacityCount != 1 || highCapacityIndex < 0 ||
            customIndex < 0 || resilientIndex < 0) {
            qCritical() << "profile list invariant failed";
            return 3;
        }
        profiles->setCurrentIndex(resilientIndex);
        QApplication::processEvents();
        if (
            profiles->currentData().toInt() !=
                static_cast<int>(ReliabilityProfile::Local)) {
            qCritical() << "Resilient GUI selection invariant failed";
            return 3;
        }
        profiles->setCurrentIndex(highCapacityIndex);
        QApplication::processEvents();
        if (repair->value() != 5.0 || repair->isEnabled() ||
            !help->text().contains("6/6 exact")) {
            qCritical() << "High Capacity GUI selection invariant failed";
            return 4;
        }
        profiles->setCurrentIndex(customIndex);
        QApplication::processEvents();
        if (!repair->isEnabled()) {
            qCritical() << "Custom repair editing was not preserved";
            return 5;
        }
    }

    if (!preflightSmokeInput.isEmpty() &&
        !preflightSmokeOutput.isEmpty()) {
        window.findChild<QLineEdit *>("inputFileEdit")
            ->setText(preflightSmokeInput);
        window.findChild<QLineEdit *>("outputFileEdit")
            ->setText(preflightSmokeOutput);
    }

    if (!videoSetAssistantSmokeRoot.isEmpty()) {
#ifdef Q_OS_WIN
        if (QGuiApplication::platformName() != QStringLiteral("windows")) {
            qCritical() << "Assistant E2E requires the real qwindows platform plugin; active platform is"
                        << QGuiApplication::platformName();
            return 29;
        }
        qInfo() << "Assistant E2E platform: qwindows.dll (Qt key: windows)";
#endif
        const QString root = QDir(videoSetAssistantSmokeRoot).absolutePath();
        const QString source = QDir(root).filePath("source.bin");
        const QString sets = QDir(root).filePath("sets");
        const QString returned = QDir(root).filePath("returned-renamed");
        const QString recovered = QDir(root).filePath("recovered");
        const QString scopedPrefix =
            QDir::fromNativeSeparators(root) + "/";
        if (!QFileInfo(source).isFile() ||
            !QDir::fromNativeSeparators(
                QFileInfo(source).absoluteFilePath()).startsWith(
                    scopedPrefix, Qt::CaseInsensitive)) {
            qCritical() << "Assistant smoke source is outside its test root";
            return 30;
        }

        auto *stack = window.findChild<QStackedWidget *>(
            "videoSetAssistantStack");
        auto *create = window.findChild<QPushButton *>(
            "videoSetAssistantCreateChoice");
        auto *recoverChoice = window.findChild<QPushButton *>(
            "videoSetAssistantRecoverChoice");
        auto *input = window.findChild<QLineEdit *>(
            "videoSetAssistantSourcePath");
        auto *output = window.findChild<QLineEdit *>(
            "videoSetAssistantOutputRoot");
        auto *sourceContinue = window.findChild<QPushButton *>(
            "videoSetAssistantSourceContinue");
        auto *highCapacity = window.findChild<QRadioButton *>(
            "videoSetHighCapacityChoice");
        auto *resilient = window.findChild<QRadioButton *>(
            "videoSetResilientChoice");
        auto *target = window.findChild<QSpinBox *>(
            "videoSetAssistantTargetDuration");
        auto *maximumSize = window.findChild<QSpinBox *>(
            "videoSetAssistantMaximumSize");
        auto *calculate = window.findChild<QPushButton *>(
            "videoSetAssistantCalculatePlan");
        auto *planSummary = window.findChild<QLabel *>(
            "videoSetAssistantPlanSummary");
        auto *createVideos = window.findChild<QPushButton *>(
            "videoSetAssistantCreateVideos");
        auto *progressContinue = window.findChild<QPushButton *>(
            "videoSetAssistantProgressContinue");
        auto *progressPart = window.findChild<QLabel *>(
            "videoSetAssistantCurrentPart");
        auto *createResultCard = window.findChild<QPushButton *>(
            "videoSetCreateResultCardButton");
        auto *uploaded = window.findChild<QPushButton *>(
            "videoSetAssistantUploaded");
        auto *recoveryInput = window.findChild<QLineEdit *>(
            "videoSetAssistantReturnedPath");
        auto *recoveryOutput = window.findChild<QLineEdit *>(
            "videoSetAssistantRecoveryOutput");
        auto *scan = window.findChild<QPushButton *>(
            "videoSetAssistantScan");
        auto *recover = window.findChild<QPushButton *>(
            "videoSetAssistantRecover");
        auto *scanSummary = window.findChild<QLabel *>(
            "videoSetAssistantScanSummary");
        auto *success = window.findChild<QLabel *>(
            "videoSetAssistantExactSuccess");
        auto *recoveryResultCard = window.findChild<QPushButton *>(
            "videoSetRecoveryResultCardButton");
        auto *recent = window.findChild<QListWidget *>(
            "videoSetRecentList");
        auto *recentFull = window.findChild<QListWidget *>(
            "videoSetRecentFullList");
        auto *recentNavigation = window.findChild<QPushButton *>(
            "recentNavigationButton");
        auto *recentPage = window.findChild<QWidget *>(
            "videoSetRecentPage");
        auto *actionBar = window.findChild<QFrame *>(
            "videoSetWizardActionBar");
        auto *sourceBack = window.findChild<QPushButton *>(
            "videoSetAssistantSourceBack");
        auto *modeBack = window.findChild<QPushButton *>(
            "videoSetAssistantModeBack");
        auto *planBack = window.findChild<QPushButton *>(
            "videoSetAssistantPlanBack");
        auto *activityPanel = window.findChild<QFrame *>(
            "videoSetActivityPanel");
        auto *activityTitle = window.findChild<QLabel *>(
            "videoSetActivityTitle");
        auto *activityDescription = window.findChild<QLabel *>(
            "videoSetActivityDescription");
        auto *activityElapsed = window.findChild<QLabel *>(
            "videoSetElapsedTime");
        auto *activityProgressLabel = window.findChild<QLabel *>(
            "videoSetBlockProgressLabel");
        auto *activityProgress = static_cast<VidStoreXBlockProgress *>(
            window.findChild<QWidget *>("videoSetBlockProgress"));
        auto *activityFlow = static_cast<VidStoreXProcessingFlow *>(
            window.findChild<QWidget *>("videoSetLiveDataPath"));
        auto *activityDetailsButton = window.findChild<QToolButton *>(
            "videoSetActivityDetailsToggle");
        auto *activityDetails = window.findChild<QWidget *>(
            "videoSetActivityDetails");
        auto *activityParts = static_cast<VidStoreXPartGrid *>(
            window.findChild<QWidget *>("videoSetPartGrid"));
        auto *activitySource = window.findChild<QLabel *>(
            "videoSetProcessingSummary");
        auto *activityLog = window.findChild<QTextEdit *>(
            "videoSetTechnicalLog");
        auto *language = window.findChild<QComboBox *>(
            "uiLanguageCombo");
        auto *homeNavigation = window.findChild<QPushButton *>(
            "homeNavigationButton");
        auto *settingsNavigation = window.findChild<QPushButton *>(
            "settingsNavigationButton");
        auto *settingsAuthor = window.findChild<QLabel *>(
            "settingsAboutAuthor");
        auto *settingsAboutHeading = window.findChild<QLabel *>(
            "settingsAboutHeading");
        auto *settingsAboutVersion = window.findChild<QLabel *>(
            "settingsAboutVersion");
        auto *settingsAboutDefinition = window.findChild<QLabel *>(
            "settingsAboutDefinition");
        auto *settingsLinkedInUrl = window.findChild<QLabel *>(
            "settingsLinkedInUrl");
        auto *settingsLinkedIn = window.findChild<QPushButton *>(
            "settingsLinkedInButton");
        auto *advancedNavigation = window.findChild<QToolButton *>(
            "advancedNavigationButton");
        auto *brandSubtitle = window.findChild<QLabel *>(
            "brandSubtitle");
        auto *applicationHeader = window.findChild<QFrame *>(
            "applicationHeader");
        auto *workflowStepper = static_cast<VidStoreXStepper *>(
            window.findChild<QWidget *>("videoSetAssistantStepper"));
        auto *planHeading = window.findChild<QLabel *>(
            "videoSetPageHeading3");
        auto *planSubtitle = window.findChild<QLabel *>(
            "videoSetPageSubtitle3");
        auto *sourceHeading = window.findChild<QLabel *>(
            "videoSetPageHeading1");
        auto *recoverHeading = window.findChild<QLabel *>(
            "videoSetPageHeading7");
        auto *planMetrics = window.findChild<QLabel *>(
            "videoSetAssistantPlanMetrics");
        auto *playlistEdit = window.findChild<QLineEdit *>(
            "videoSetPlaylistUrl");
        auto *downloadReturned = window.findChild<QPushButton *>(
            "videoSetDownloadReturnedVideos");
        auto *downloadStatus = window.findChild<QLabel *>(
            "videoSetDownloadStatus");
        auto *instantPlaylistEdit = window.findChild<QLineEdit *>(
            "instantPlaylistUrl");
        auto *resilientCard = window.findChild<QGroupBox *>(
            "videoSetResilientCard");
        auto *highCapacityCard = window.findChild<QGroupBox *>(
            "videoSetHighCapacityCard");
        auto *homeHeading = window.findChild<QLabel *>(
            "videoSetPageHeading0");
        auto *trustLabel = window.findChild<QLabel *>(
            "videoSetTrustLabel");
        auto *classicTools = window.findChild<QGroupBox *>(
            "videoSetClassicTools");
        auto *successRail = window.findChild<QWidget *>(
            "videoSetSuccessSignalRail");
        auto *capacityHeading = window.findChild<QLabel *>(
            "capacityLabPageHeading");
        auto *capacityAction = window.findChild<QAction *>(
            "advancedCapacityLabAction");
        auto *landingAction = window.findChild<QAction *>(
            "advancedLandingAction");
        auto *testLabAction = window.findChild<QAction *>(
            "advancedTestLabAction");
        auto *classicAction = window.findChild<QAction *>(
            "advancedClassicVideoSetAction");
        auto *capacitySearch = window.findChild<QGroupBox *>(
            "capacitySearchSpaceSection");
        auto *capacitySimulation = window.findChild<QGroupBox *>(
            "capacitySimulationSection");
        auto *testLabGenerate = window.findChild<QGroupBox *>(
            "testLabGenerateSection");
        auto *testLabAnalysis = window.findChild<QGroupBox *>(
            "testLabAnalysisSection");
        auto *youtubeSyncAction = window.findChild<QAction *>(
            "advancedYouTubeSyncAction");
        auto *youtubeSyncPage = window.findChild<QWidget *>(
            "experimentalYouTubeSyncPage");
        auto *youtubeSyncHeading = window.findChild<QLabel *>(
            "experimentalYouTubeSyncHeading");
        auto *youtubeSyncWarning = window.findChild<QLabel *>(
            "experimentalYouTubeSyncWarning");
        auto *oauthConfig = window.findChild<QLineEdit *>(
            "youtubeOAuthConfigPath");
        auto *syncCard = window.findChild<QFrame *>("youtubeSyncCard");
        auto *openVideos = window.findChild<QPushButton *>(
            "videoSetOpenVideosFolderButton");
        auto *openYouTube = window.findChild<QPushButton *>(
            "videoSetOpenYouTubeButton");
        auto *uploadNotice = window.findChild<QLabel *>(
            "videoSetUploadPrivacyNotice");
        if (!stack || !create || !input || !output || !sourceContinue ||
            !highCapacity || !resilient || !target || !maximumSize ||
            !calculate ||
            !planSummary || !createVideos || !progressContinue ||
            !createResultCard || !recoveryResultCard ||
            !progressPart || !uploaded || !recoveryInput ||
            !recoveryOutput || !scan || !recover || !scanSummary ||
            !success || !recent || !recentFull || !recentNavigation ||
            !recentPage || !actionBar || !sourceBack || !modeBack ||
            !planBack || !activityPanel || !activityTitle ||
            !activityDescription || !activityElapsed ||
            !activityProgressLabel || !activityProgress ||
            !activityFlow || !activityDetailsButton || !activityDetails ||
            !activityParts || !activitySource || !activityLog || !language ||
            !homeNavigation || !brandSubtitle || !resilientCard ||
            !highCapacityCard || !recoverChoice || !settingsNavigation ||
            !settingsAuthor || !settingsAboutHeading ||
            !settingsAboutVersion || !settingsAboutDefinition ||
            !settingsLinkedInUrl || !settingsLinkedIn ||
            !advancedNavigation || !homeHeading || !trustLabel ||
            !applicationHeader || !workflowStepper || !planHeading ||
            !planSubtitle ||
            !planMetrics || !sourceHeading || !recoverHeading ||
            !playlistEdit || !downloadReturned || !downloadStatus ||
            !instantPlaylistEdit ||
            !classicTools || !successRail || !capacityHeading ||
            !capacityAction || !landingAction || !testLabAction ||
            !classicAction || !capacitySearch || !capacitySimulation ||
            !testLabGenerate || !testLabAnalysis ||
            !youtubeSyncAction || !youtubeSyncPage ||
            !youtubeSyncHeading || !youtubeSyncWarning || !oauthConfig ||
            !syncCard || !openVideos || !openYouTube || !uploadNotice) {
            qCritical() << "Assistant E2E smoke controls were not found";
            return 31;
        }

        language->setCurrentIndex(language->findData("tr"));
        QApplication::processEvents();
        window.resize(1280, 720);
        QApplication::processEvents();
        int visibleBrands = 0;
        for (auto *label : window.findChildren<QLabel *>())
            if (label->isVisible() && label->text() == QStringLiteral("Reliquary"))
                ++visibleBrands;
        if (window.property("uiLanguage").toString() != "tr" ||
            homeHeading->text() != QString::fromUtf8(
                "Dosyalarınızı videolarda güvenle saklayın") ||
            !trustLabel->text().contains(QStringLiteral("YouTube")) ||
            visibleBrands != 1 ||
            create->property("vsxRole").toString() != "primary" ||
            recoverChoice->property("vsxRole").toString() != "primary" ||
            advancedNavigation->text() != QString::fromUtf8("Gelişmiş") ||
            classicTools->title() != QString::fromUtf8(
                "Klasik Video Set Araçları")) {
            qCritical() << "Turkish visual identity home invariant failed";
            return 57;
        }
        for (auto *label : window.findChildren<QLabel *>()) {
            if (label->isVisible() &&
                (label->text().contains("YouTube Sync", Qt::CaseInsensitive) ||
                 label->text().contains("OAuth", Qt::CaseInsensitive) ||
                 label->text().contains("Google Cloud", Qt::CaseInsensitive))) {
                qCritical() << "Turkish consumer Home exposes experimental text"
                            << label->text();
                return 67;
            }
        }
        auto *assistantScroll = window.findChild<QScrollArea *>(
            "videoSetAssistantScrollArea");
        const QList<QPair<QSize, QString>> homeSizes{
            {{1280, 720}, "1280x720"}, {{1366, 768}, "1366x768"},
            {{1600, 900}, "1600x900"}, {{1920, 1080}, "1920x1080"}};
        for (const auto &[size, suffix] : homeSizes) {
            window.resize(size);
            QApplication::processEvents();
            if (!window.grab().save(QDir(root).filePath(
                    "e2e-home-tr-light-" + suffix + ".png"))) {
                qCritical() << "Could not save Turkish Home visual audit" << suffix;
                return 58;
            }
            if (size.height() >= 768 && assistantScroll &&
                assistantScroll->verticalScrollBar()->isVisible()) {
                qCritical() << "Home has an unnecessary vertical scrollbar" << suffix;
                return 62;
            }
        }
        recentNavigation->click();
        QApplication::processEvents();
        if (stack->currentIndex() != 10 || !recentPage->isVisible() ||
            homeHeading->isVisible() || trustLabel->isVisible() ||
            recentFull->count() != recent->count() ||
            !activityPanel->isHidden()) {
            qCritical() << "Home and Recent are not isolated presentation pages";
            return 83;
        }
        for (const auto &[size, suffix] : homeSizes) {
            window.resize(size);
            QApplication::processEvents();
            if (!window.grab().save(QDir(root).filePath(
                    "e2e-recent-tr-light-" + suffix + ".png"))) {
                qCritical() << "Could not save dedicated Recent audit" << suffix;
                return 84;
            }
        }
        homeNavigation->click();
        QApplication::processEvents();
        if (stack->currentIndex() != 0 || !homeHeading->isVisible() ||
            !trustLabel->isVisible() || recentPage->isVisible()) {
            qCritical() << "Recent did not route back to the distinct Home page";
            return 85;
        }
        const auto actionIsPinnedAndVisible =
            [&window, actionBar, assistantScroll](QPushButton *button) {
            if (!button || !button->isVisible() || !button->isEnabled() ||
                !actionBar->isVisible() ||
                assistantScroll->isAncestorOf(actionBar) ||
                assistantScroll->isAncestorOf(button))
                return false;
            const QRect buttonRect(button->mapTo(&window, QPoint()),
                                   button->size());
            return window.rect().contains(buttonRect) &&
                actionBar->geometry().bottom() <=
                    window.QMainWindow::centralWidget()->height();
        };
        const auto substantiallyVisibleInWorkflow =
            [assistantScroll](QWidget *widget) {
            if (!assistantScroll || !widget || !widget->isVisible())
                return false;
            const QRect rect(widget->mapTo(assistantScroll->viewport(),
                                           QPoint()), widget->size());
            const QRect visible = rect.intersected(
                assistantScroll->viewport()->rect());
            return visible.width() > 0 &&
                visible.height() >= qMax(1, widget->height() / 2);
        };
        create->click();
        input->setText(source);
        output->setText(sets);
        QApplication::processEvents();
        for (const auto &[size, suffix] : homeSizes) {
            window.resize(size);
            QApplication::processEvents();
            if (stack->currentIndex() != 1 || !activityPanel->isHidden() ||
                applicationHeader->height() > 76 ||
                workflowStepper->height() > 50 ||
                sourceHeading->mapTo(&window, QPoint()).x() > 80 ||
                !actionIsPinnedAndVisible(sourceContinue) ||
                !window.grab().save(QDir(root).filePath(
                    "e2e-create-step1-" + suffix + ".png"))) {
                qCritical() << "Create Step 1 pinned action audit failed" << suffix;
                return 86;
            }
        }
        sourceContinue->click();
        QApplication::processEvents();
        for (const auto &[size, suffix] : homeSizes) {
            window.resize(size);
            QApplication::processEvents();
            if (stack->currentIndex() != 2 || !activityPanel->isHidden() ||
                !actionIsPinnedAndVisible(calculate) ||
                !window.grab().save(QDir(root).filePath(
                    "e2e-create-step2-" + suffix + ".png"))) {
                qCritical() << "Create Step 2 pinned action audit failed" << suffix;
                return 87;
            }
        }
        recoveryInput->setText(root);
        recoveryOutput->setText(recovered);
        recoverChoice->click();
        QApplication::processEvents();
        for (const auto &[size, suffix] : homeSizes) {
            window.resize(size);
            QApplication::processEvents();
            if (stack->currentIndex() != 7 || !activityPanel->isHidden() ||
                applicationHeader->height() > 76 ||
                workflowStepper->height() > 50 ||
                recoverHeading->mapTo(&window, QPoint()).x() > 80 ||
                !substantiallyVisibleInWorkflow(instantPlaylistEdit) ||
                !actionIsPinnedAndVisible(scan) ||
                !window.grab().save(QDir(root).filePath(
                    "e2e-recover-initial-" + suffix + ".png"))) {
                qCritical() << "Recover pinned action audit failed" << suffix;
                return 88;
            }
        }
        create->click();
        QApplication::processEvents();
        if (stack->currentIndex() == 2) modeBack->click();
        homeNavigation->click();
        QApplication::processEvents();
        if (stack->isAncestorOf(classicTools)) {
            qCritical() << "Classic tools leaked into consumer Home";
            return 68;
        }
        const auto auditAdvancedPage = [&](QAction *action,
                                            const QString &prefix,
                                            QWidget *first,
                                            QWidget *second) {
            action->trigger();
            QApplication::processEvents();
            for (const auto &[size, suffix] : homeSizes) {
                window.resize(size);
                QApplication::processEvents();
                if (!window.grab().save(QDir(root).filePath(
                        prefix + "-" + suffix + ".png")))
                    return false;
                if (first && second && first->isVisible() && second->isVisible() &&
                    first->mapTo(&window, QPoint()).y() ==
                        second->mapTo(&window, QPoint()).y() &&
                    first->geometry().intersects(second->geometry()))
                    return false;
            }
            return true;
        };
        if (!auditAdvancedPage(landingAction, "e2e-advanced-landing-tr",
                               nullptr, nullptr) ||
            !auditAdvancedPage(classicAction, "e2e-classic-tr",
                               nullptr, nullptr) ||
            !auditAdvancedPage(testLabAction, "e2e-testlab-tr",
                               testLabGenerate, testLabAnalysis) ||
            !auditAdvancedPage(capacityAction, "e2e-capacity-tr",
                               capacitySearch, capacitySimulation)) {
            qCritical() << "Advanced responsive visual audit failed";
            return 69;
        }
        homeNavigation->click();
        QApplication::processEvents();
        const QPalette originalPalette = qApp->palette();
        QPalette darkPalette = originalPalette;
        darkPalette.setColor(QPalette::Window, QColor("#20201E"));
        darkPalette.setColor(QPalette::Base, QColor("#181816"));
        darkPalette.setColor(QPalette::WindowText, QColor("#F1EEE7"));
        darkPalette.setColor(QPalette::Text, QColor("#F1EEE7"));
        darkPalette.setColor(QPalette::ButtonText, QColor("#F1EEE7"));
        qApp->setPalette(darkPalette);
        window.resize(1366, 768);
        QApplication::processEvents();
        if (!window.grab().save(QDir(root).filePath(
                "e2e-home-tr-dark-1366x768.png")))
            return 63;
        QPalette auditLightPalette = originalPalette;
        auditLightPalette.setColor(QPalette::Window, QColor("#F4F1EB"));
        auditLightPalette.setColor(QPalette::Base, QColor("#FFFDF8"));
        auditLightPalette.setColor(QPalette::WindowText, QColor("#24211D"));
        auditLightPalette.setColor(QPalette::Text, QColor("#24211D"));
        auditLightPalette.setColor(QPalette::ButtonText, QColor("#24211D"));
        qApp->setPalette(auditLightPalette);
        window.setPalette(auditLightPalette);
        vidstorex_ui::applyTheme(window.QMainWindow::centralWidget());
        QApplication::processEvents();
        language->setCurrentIndex(language->findData("en"));
        QApplication::processEvents();
        if (!window.grab().save(QDir(root).filePath(
                "e2e-home-en-light-1366x768.png")))
            return 64;
        language->setCurrentIndex(language->findData("tr"));
        QApplication::processEvents();
        capacityAction->trigger();
        QApplication::processEvents();
        if (!capacityHeading->isVisible() ||
            capacityHeading->text() != QString::fromUtf8(
                "Gelişmiş / Kapasite Laboratuvarı")) {
            qCritical() << "Turkish Advanced Capacity wrapper failed";
            return 59;
        }
        window.grab().save(QDir(root).filePath("e2e-capacity-tr.png"));
        settingsNavigation->click();
        QApplication::processEvents();
        auto *settingsLanguageSection = window.findChild<QLabel *>(
            "settingsLanguageSection");
        if (!settingsLanguageSection ||
            settingsLanguageSection->text() != QString::fromUtf8("Dil") ||
            oauthConfig->isVisible() || syncCard->isVisible() ||
            !settingsAuthor->text().contains(
                QString::fromUtf8("Burhan Talha Yazıcı")) ||
            settingsAboutHeading->text() != QStringLiteral("Reliquary") ||
            settingsAboutVersion->text() != QStringLiteral("v1.4.0") ||
            settingsAboutDefinition->text() != QString::fromUtf8(
                "Değerli bir şeyi korumak için kullanılan muhafaza.") ||
            settingsAuthor->text() != QString::fromUtf8(
                "Burhan Talha Yazıcı • BTY tarafından geliştirildi") ||
            settingsLinkedInUrl->text() !=
                QStringLiteral("linkedin.com/in/burhanbty") ||
            settingsAboutDefinition->text().contains(
                QString::fromUtf8("dosyalarınızı"), Qt::CaseInsensitive) ||
            settingsLinkedIn->property("externalUrl").toString() !=
                QStringLiteral("https://www.linkedin.com/in/burhanbty")) {
            qCritical() << "Turkish Settings sections failed";
            return 60;
        }
        window.grab().save(QDir(root).filePath("e2e-settings-tr.png"));
        youtubeSyncAction->trigger();
        QApplication::processEvents();
        if (!youtubeSyncPage->isVisible() || !oauthConfig->isVisible() ||
            !syncCard->isVisible() ||
            !youtubeSyncHeading->text().contains(
                QString::fromUtf8("Deneysel")) ||
            !youtubeSyncWarning->text().contains(
                QString::fromUtf8("normal Reliquary"), Qt::CaseInsensitive)) {
            qCritical() << "Turkish Experimental YouTube Sync separation failed";
            return 68;
        }
        window.grab().save(QDir(root).filePath(
            "e2e-youtube-sync-experimental-tr.png"));
        language->setCurrentIndex(language->findData("en"));
        QApplication::processEvents();
        if (!youtubeSyncHeading->text().contains("Experimental") ||
            !youtubeSyncWarning->text().contains(
                "not required for normal Reliquary use",
                Qt::CaseInsensitive)) {
            qCritical() << "English Experimental YouTube Sync separation failed";
            return 69;
        }
        settingsNavigation->click();
        QApplication::processEvents();
        if (settingsAboutHeading->text() != QStringLiteral("Reliquary") ||
            settingsAboutVersion->text() != QStringLiteral("v1.4.0") ||
            settingsAboutDefinition->text() != QStringLiteral(
                "A container for preserving something precious.") ||
            settingsAuthor->text() != QStringLiteral(
                "Made by Burhan Talha Yazıcı • BTY") ||
            settingsAboutDefinition->text().contains(
                QStringLiteral("files"), Qt::CaseInsensitive)) {
            qCritical() << "English Reliquary About structure failed";
            return 70;
        }
        window.grab().save(QDir(root).filePath("e2e-settings-en.png"));
        if (qEnvironmentVariableIntValue(
                "VIDSTOREX_UI_AUDIT_ONLY") == 1)
            return 0;
        homeNavigation->click();
        QApplication::processEvents();

        struct SmokeState {
            int stage = 0;
            qint64 deadline = 0;
            QString setRoot;
            QStringList returnedFiles;
            QStringList sourceVideoFiles;
            bool sawScan = false;
            bool sawRecovery = false;
            bool testedActiveLanguageSwitch = false;
            bool createCardDone = false;
            bool recoveryCardDone = false;
            bool isolationChecked = false;
            bool activeEncodeNavigationChecked = false;
            bool activeRecoveryNavigationChecked = false;
            qint64 planFrozenAt = 0;
            QString planDuration;
            bool activeDownloadCaptured = false;
        };
        auto *state = new SmokeState{
            0, QDateTime::currentMSecsSinceEpoch() + 110000, {}, {}, {},
            false, false, false, false, false, false, false, false, 0, {}};
        auto *timer = new QTimer(&app);
        timer->setInterval(100);
        QObject::connect(timer, &QTimer::timeout, &app,
            [&, state, timer, root, source, sets, returned, recovered,
             homeSizes, actionIsPinnedAndVisible]() {
            const auto fail = [&](const int code, const QString &message) {
                qCritical() << message;
                QFile diagnostic(QDir(root).filePath("e2e-failure.txt"));
                if (diagnostic.open(QIODevice::WriteOnly | QIODevice::Text))
                    diagnostic.write(message.toUtf8());
                timer->stop();
                app.exit(code);
            };
            if (QDateTime::currentMSecsSinceEpoch() > state->deadline) {
                fail(32, QString(
                    "Assistant E2E smoke timed out at stage %1 "
                    "(stack=%2, sourceContinue=%3, createVideos=%4, "
                    "progressContinue=%5, scan=%6, recover=%7)")
                    .arg(state->stage)
                    .arg(stack->currentIndex())
                    .arg(sourceContinue->isEnabled())
                    .arg(createVideos->isEnabled())
                    .arg(progressContinue->isEnabled())
                    .arg(scan->isEnabled())
                    .arg(recover->isEnabled()) +
                    QString(" activity=%1 observedScan=%2 observedRecovery=%3 finalHash=%4")
                        .arg(activityTitle->text())
                        .arg(activityPanel->property("observedScan").toBool())
                        .arg(activityPanel->property("observedRecovery").toBool())
                        .arg(activityPanel->property("observedFinalHash").toBool()) +
                    QString(" description=%1 log=%2")
                        .arg(activityDescription->text(),
                             activityLog->toPlainText().right(1200)));
                return;
            }
            if (state->stage == 0) {
                create->click();
                input->setText(source);
                output->setText(sets);
                state->stage = 1;
                qInfo() << "Assistant E2E stage 1: source selected";
                return;
            }
            if (state->stage == 1 && sourceContinue->isEnabled()) {
                sourceContinue->click();
                if (!highCapacity->isChecked()) {
                    fail(75, "Fresh Create did not default to High Capacity");
                    return;
                }
                resilient->click();
                QApplication::processEvents();
                if (!modeBack->isVisible()) {
                    fail(76, QString(
                        "Mode Back control was not visible (stack=%1, action=%2, "
                        "actionPage=%3, parent=%4)")
                        .arg(stack->currentIndex())
                        .arg(actionBar->isVisible())
                        .arg(modeBack->parentWidget()
                            ? modeBack->parentWidget()->isVisible() : false)
                        .arg(modeBack->parentWidget()
                            ? modeBack->parentWidget()->objectName()
                            : QStringLiteral("none")));
                    return;
                }
                modeBack->click();
                sourceContinue->click();
                if (!resilient->isChecked()) {
                    fail(77, "Mode choice was not preserved across Back/Continue");
                    return;
                }
                highCapacity->click();
                QApplication::processEvents();
                window.grab().save(QDir(root).filePath("e2e-profile-en.png"));
                target->setValue(1);
                maximumSize->setValue(0);
                calculate->click();
                state->stage = 2;
                qInfo() << "Assistant E2E stage 2: planning";
                return;
            }
            if (state->stage == 2 && createVideos->isEnabled()) {
                if (state->planFrozenAt == 0) {
                    if (activityTitle->text() !=
                            QStringLiteral("Video Set plan is ready") ||
                        !activityElapsed->text().startsWith(
                            QStringLiteral("Duration:")) ||
                        !activityPanel->property("terminalOperation").toBool() ||
                        activityFlow->presentationMode() !=
                            VidStoreXProcessingFlow::PresentationMode::Compact ||
                        activityPanel->height() > 160 ||
                        activityDetailsButton->isChecked() ||
                        activityDetails->isVisible() ||
                        activityFlow->isVisible()) {
                        fail(78, "Completed plan does not use terminal wording");
                        return;
                    }
                    state->planDuration = activityElapsed->text();
                    state->planFrozenAt = QDateTime::currentMSecsSinceEpoch();
                    return;
                }
                if (QDateTime::currentMSecsSinceEpoch() -
                        state->planFrozenAt < 2200)
                    return;
                if (activityElapsed->text() != state->planDuration) {
                    fail(79, "Completed plan duration continued changing");
                    return;
                }
                language->setCurrentIndex(language->findData("tr"));
                QApplication::processEvents();
                if (activityTitle->text() !=
                        QString::fromUtf8("Video Set planı hazır") ||
                    !activityDescription->text().contains(
                        QString::fromUtf8("hesapladı")) ||
                    activityProgressLabel->text() !=
                        QString::fromUtf8("Plan tamamlandı") ||
                    !activityElapsed->text().startsWith(
                        QString::fromUtf8("Süre:"))) {
                    fail(80, "Completed plan Turkish copy is incomplete");
                    return;
                }
                window.grab().save(QDir(root).filePath(
                    "e2e-plan-complete-tr.png"));
                language->setCurrentIndex(language->findData("en"));
                QApplication::processEvents();
                if (!state->isolationChecked) {
                    const QString terminalDuration = activityElapsed->text();
                    language->setCurrentIndex(language->findData("tr"));
                    QApplication::processEvents();
                    for (const auto &[size, suffix] : homeSizes) {
                        window.resize(size);
                        QApplication::processEvents();
                        if (stack->currentIndex() != 3 ||
                            activityPanel->isHidden() ||
                            activityPanel->height() > 160 ||
                            activityDetails->isVisible() ||
                            activityFlow->isVisible() ||
                            applicationHeader->height() > 76 ||
                            workflowStepper->height() > 50 ||
                            !actionIsPinnedAndVisible(createVideos) ||
                            !window.grab().save(QDir(root).filePath(
                                "e2e-create-step3-" + suffix + ".png"))) {
                            fail(89, "Create Step 3 pinned action audit failed: " +
                                suffix);
                            return;
                        }
                        const int subtitleBottom = planSubtitle->mapTo(
                            &window, QPoint()).y() + planSubtitle->height();
                        const int summaryTop = planSummary->mapTo(
                            &window, QPoint()).y();
                        const int summaryBottom = summaryTop +
                            planSummary->height();
                        const int metricsTop = planMetrics->mapTo(
                            &window, QPoint()).y();
                        if (summaryTop - subtitleBottom > 20 ||
                            metricsTop - summaryBottom > 28) {
                            fail(105, "Create Plan content is not top-aligned: " +
                                suffix);
                            return;
                        }
                        if (size == QSize(1600, 900) &&
                            (assistantScroll->verticalScrollBar()->isVisible() ||
                             !substantiallyVisibleInWorkflow(planSummary) ||
                             !substantiallyVisibleInWorkflow(planMetrics))) {
                            fail(106, QString(
                                "Create Plan review is not visible in the "
                                "1600x900 viewport (scroll=%1 max=%2 page=%3 "
                                "stackMin=%4 currentMin=%5 viewport=%6)")
                                .arg(assistantScroll->verticalScrollBar()->isVisible())
                                .arg(assistantScroll->verticalScrollBar()->maximum())
                                .arg(assistantScroll->verticalScrollBar()->pageStep())
                                .arg(stack->minimumHeight())
                                .arg(stack->currentWidget()->minimumSizeHint().height())
                                .arg(assistantScroll->viewport()->height()));
                            return;
                        }
                    }
                    language->setCurrentIndex(language->findData("en"));
                    QApplication::processEvents();
                    planBack->click();
                    QApplication::processEvents();
                    if (stack->currentIndex() != 2 ||
                        !activityPanel->isHidden() ||
                        !calculate->text().contains("Review plan",
                            Qt::CaseInsensitive)) {
                        fail(90, "Completed plan leaked onto Create Mode");
                        return;
                    }
                    window.grab().save(QDir(root).filePath(
                        "e2e-bug-repro-create-mode.png"));
                    modeBack->click();
                    QApplication::processEvents();
                    if (stack->currentIndex() != 1 ||
                        !activityPanel->isHidden() ||
                        !actionIsPinnedAndVisible(sourceContinue)) {
                        fail(91, "Completed plan leaked onto Create File");
                        return;
                    }
                    window.grab().save(QDir(root).filePath(
                        "e2e-bug-repro-create-file.png"));
                    recoverChoice->click();
                    QApplication::processEvents();
                    if (stack->currentIndex() != 7 ||
                        !activityPanel->isHidden()) {
                        fail(92, "Completed Create plan leaked into Recover");
                        return;
                    }
                    window.grab().save(QDir(root).filePath(
                        "e2e-bug-repro-recover.png"));
                    homeNavigation->click();
                    QApplication::processEvents();
                    if (stack->currentIndex() != 0 ||
                        !activityPanel->isHidden()) {
                        fail(93, "Completed Create plan leaked into Home");
                        return;
                    }
                    recentNavigation->click();
                    QApplication::processEvents();
                    if (stack->currentIndex() != 10 ||
                        !activityPanel->isHidden()) {
                        fail(94, "Completed Create plan leaked into Recent");
                        return;
                    }
                    create->click();
                    QApplication::processEvents();
                    sourceContinue->click();
                    QApplication::processEvents();
                    if (stack->currentIndex() != 2 ||
                        !calculate->text().contains("Review plan",
                            Qt::CaseInsensitive)) {
                        fail(95, "Same-input navigation did not preserve the plan");
                        return;
                    }
                    calculate->click();
                    QApplication::processEvents();
                    if (stack->currentIndex() != 3 ||
                        activityPanel->isHidden() ||
                        activityElapsed->text() != terminalDuration) {
                        fail(96, "Returning to the valid plan changed its duration");
                        return;
                    }
                    state->isolationChecked = true;
                    if (qEnvironmentVariableIntValue(
                            "VIDSTOREX_WORKFLOW_LAYOUT_ONLY") == 1) {
                        timer->stop();
                        window.close();
                        app.exit(0);
                        return;
                    }
                }
                const QRegularExpression count(R"((\d+) video)");
                const auto match = count.match(planSummary->text());
                if (!match.hasMatch() || match.captured(1).toInt() < 3) {
                    fail(33, "Assistant plan did not create at least three parts");
                    return;
                }
                createVideos->click();
                state->stage = 3;
                qInfo() << "Assistant E2E stage 3: encoding";
                return;
            }
            if (state->stage == 3 && progressContinue->isEnabled()) {
                if (!state->activeEncodeNavigationChecked) {
                    fail(104, "Encode completed before navigation persistence was verified");
                    return;
                }
                if (!progressPart->text().contains("verified locally")) {
                    fail(34, "Assistant did not show local exact completion");
                    return;
                }
                if (!state->createCardDone) {
                    QString cardError;
                    if (!exerciseResultCardPreview(
                            createResultCard,
                            QDir(root).filePath("create-result-card.png"),
                            {QStringLiteral("source.bin"),
                             QStringLiteral("High Capacity"),
                             QStringLiteral("Video Set Ready"),
                             QStringLiteral("videos created"),
                             QStringLiteral("Local verification")},
                            {QStringLiteral("YouTube Round-Trip"),
                             QDir::fromNativeSeparators(root)},
                            QDir(root).filePath(
                                "create-result-card-preview.png"),
                            &cardError)) {
                        fail(74, QStringLiteral(
                            "Create result card E2E failed: ") + cardError);
                        return;
                    }
                    state->createCardDone = true;
                    language->setCurrentIndex(language->findData("tr"));
                    QApplication::processEvents();
                    if (!exerciseResultCardPreview(
                            createResultCard,
                            QDir(root).filePath("create-result-card-tr.png"),
                            {QString::fromUtf8("source.bin"),
                             QString::fromUtf8("High Capacity"),
                             QString::fromUtf8("Video Set Hazır"),
                             QString::fromUtf8("video oluşturuldu"),
                             QString::fromUtf8("Yerel doğrulama")},
                            {QStringLiteral("YouTube Round-Trip"),
                             QDir::fromNativeSeparators(root)},
                            QDir(root).filePath(
                                "create-result-card-preview-tr.png"),
                            &cardError)) {
                        fail(76, QStringLiteral(
                            "Turkish Create result card E2E failed: ") +
                            cardError);
                        return;
                    }
                    language->setCurrentIndex(language->findData("en"));
                    QApplication::processEvents();
                }
                const QDir setsDirectory(sets);
                const auto setNames = setsDirectory.entryList(
                    QDir::Dirs | QDir::NoDotAndDotDot);
                if (setNames.size() != 1) {
                    fail(35, "Assistant did not atomically publish one set");
                    return;
                }
                state->setRoot = setsDirectory.filePath(setNames.front());
                const QString recentAccessible = recent->item(0)->data(
                    Qt::AccessibleTextRole).toString();
                if (recent->count() != 1 ||
                    !recentAccessible.contains("Last opened:") ||
                    recentAccessible.contains(state->setRoot,
                        Qt::CaseInsensitive) ||
                    recentAccessible.contains("set_manifest",
                        Qt::CaseInsensitive)) {
                    fail(44, "Recent Video Sets did not record last-opened time");
                    return;
                }
                recentNavigation->click();
                QApplication::processEvents();
                if (stack->currentIndex() != 10 || recentFull->count() != 1 ||
                    recentFull->item(0)->data(Qt::UserRole) !=
                        recent->item(0)->data(Qt::UserRole) ||
                    !activityPanel->isHidden()) {
                    fail(97, "Recent full list diverged from the Home preview model");
                    return;
                }
                window.grab().save(QDir(root).filePath(
                    "e2e-recent-populated.png"));
                create->click();
                QApplication::processEvents();
                if (stack->currentIndex() != 4 || activityPanel->isHidden()) {
                    fail(98, "Completed Create state did not survive Recent navigation");
                    return;
                }
                const QDir videos(QDir(state->setRoot).filePath("videos"));
                auto videoNames = videos.entryList({"*.mkv"}, QDir::Files);
                if (videoNames.size() < 3) {
                    fail(36, "Assistant encode produced fewer than three videos");
                    return;
                }
                QDir().mkpath(returned);
                std::reverse(videoNames.begin(), videoNames.end());
                int index = 0;
                for (const auto &name : videoNames) {
                    const QString destination = QDir(returned).filePath(
                        QString("Returned shuffled clip %1.mkv").arg(++index));
                    if (!QFile::copy(videos.filePath(name), destination)) {
                        fail(37, "Could not prepare renamed returned videos");
                        return;
                    }
                    state->returnedFiles << destination;
                    state->sourceVideoFiles << videos.filePath(name);
                }
                progressContinue->click();
                QApplication::processEvents();
                if (stack->currentIndex() != 5 || !openVideos->isVisible() ||
                    !openYouTube->isVisible() || !uploadNotice->isVisible() ||
                    !uploadNotice->text().contains("playlist link",
                        Qt::CaseInsensitive) || syncCard->isVisible()) {
                    fail(70, "Normal YouTube page is not manual-upload only");
                    return;
                }
                window.grab().save(QDir(root).filePath("e2e-youtube-en.png"));
                qputenv("VIDSTOREX_FAKE_YTDLP_SOURCE",
                        videos.absolutePath().toUtf8());
                qputenv("VIDSTOREX_FAKE_YTDLP_DELAY_MS", "1200");
                // Ensure the deterministic adapter wins over any yt-dlp on
                // the developer machine, matching Instant Recovery E2E.
                qputenv("PATH", QByteArray{});
                uploaded->click();
                QApplication::processEvents();
                playlistEdit->setText(
                    "https://www.youtube.com/playlist?list=PL_FAKE_CREATE_E2E");
                QApplication::processEvents();
                if (stack->currentIndex() != 6 ||
                    !downloadReturned->isEnabled()) {
                    fail(107, "Create YouTube download controls were not ready");
                    return;
                }
                language->setCurrentIndex(language->findData("tr"));
                QApplication::processEvents();
                downloadReturned->click();
                state->stage = 31;
                return;
            }
            if (state->stage == 31) {
                if (activityPanel->property("terminalOperation").toBool() &&
                    activityTitle->text().contains("failed",
                        Qt::CaseInsensitive)) {
                    fail(131, QString(
                        "Create YouTube download failed: %1 | %2 | %3")
                        .arg(activityDescription->text(),
                             downloadStatus->text(),
                             activityLog->toPlainText().right(1200)));
                    return;
                }
                if (!state->activeDownloadCaptured &&
                    activityPanel->property("observedDownload").toBool() &&
                    activityFlow->mode() ==
                        VidStoreXProcessingFlow::Mode::Download &&
                    !activityPanel->property("terminalOperation").toBool()) {
                    const QList<QPair<QSize, QString>> downloadSizes{
                        {{1366, 768}, "1366x768"},
                        {{1600, 900}, "1600x900"}};
                    for (const auto &[size, suffix] : downloadSizes) {
                        window.resize(size);
                        QApplication::processEvents();
                        const QRect actionRect(
                            downloadReturned->mapTo(&window, QPoint()),
                            downloadReturned->size());
                        if (applicationHeader->height() > 76 ||
                            workflowStepper->height() > 50 ||
                            activityPanel->isHidden() ||
                            activityPanel->height() >= window.height() / 4 ||
                            activityDetailsButton->isChecked() ||
                            activityDetails->isVisible() ||
                            activityFlow->isVisible() ||
                            !substantiallyVisibleInWorkflow(playlistEdit) ||
                            !actionBar->isVisible() ||
                            !window.rect().contains(actionRect) ||
                            !window.grab().save(QDir(root).filePath(
                                "e2e-create-youtube-active-" + suffix +
                                ".png"))) {
                            fail(108, "Create YouTube active download layout "
                                "audit failed: " + suffix);
                            return;
                        }
                    }
                    state->activeDownloadCaptured = true;
                    return;
                }
                if (!state->activeDownloadCaptured ||
                    !downloadReturned->isEnabled() ||
                    stack->currentIndex() != 7)
                    return;
                language->setCurrentIndex(language->findData("en"));
                QApplication::processEvents();
                recoveryInput->setText(returned);
                recoveryOutput->setText(recovered);
                scan->click();
                if (recover->isEnabled()) {
                    fail(46, "Recovery was enabled while a new scan was starting");
                    return;
                }
                state->stage = 4;
                qInfo() << "Assistant E2E stage 4: scanning returned videos";
                return;
            }
            if (state->stage == 3 &&
                !state->testedActiveLanguageSwitch &&
                stack->currentIndex() == 4 &&
                !progressContinue->isEnabled()) {
                if (activityPanel->isHidden() || activityFlow->isVisible() ||
                    activityDetails->isVisible() ||
                    activityDetailsButton->isChecked() ||
                    activityPanel->height() > 160 ||
                    activityFlow->mode() != VidStoreXProcessingFlow::Mode::Create ||
                    activityFlow->presentationMode() !=
                        VidStoreXProcessingFlow::PresentationMode::Compact ||
                    activityPanel->property("terminalOperation").toBool() ||
                    activitySource->text().contains(source,
                        Qt::CaseInsensitive) ||
                    activitySource->text().isEmpty() ||
                    activityProgress->accessibleDescription().isEmpty()) {
                    fail(71, QString(
                        "Create Live Data Path or consumer source summary is "
                        "invalid (panelHidden=%1 flowVisible=%2 detailsVisible=%3 "
                        "detailsChecked=%4 panelHeight=%5 flowMode=%6 "
                        "presentation=%7 terminal=%8 sourceEmpty=%9 "
                        "sourceContainsPath=%10 accessibleEmpty=%11 source=%12)")
                        .arg(activityPanel->isHidden())
                        .arg(activityFlow->isVisible())
                        .arg(activityDetails->isVisible())
                        .arg(activityDetailsButton->isChecked())
                        .arg(activityPanel->height())
                        .arg(static_cast<int>(activityFlow->mode()))
                        .arg(static_cast<int>(activityFlow->presentationMode()))
                        .arg(activityPanel->property("terminalOperation").toBool())
                        .arg(activitySource->text().isEmpty())
                        .arg(activitySource->text().contains(
                            source, Qt::CaseInsensitive))
                        .arg(activityProgress->accessibleDescription().isEmpty())
                        .arg(activitySource->text()));
                    return;
                }
                if (!state->activeEncodeNavigationChecked) {
                    recoverChoice->click();
                    QApplication::processEvents();
                    if (stack->currentIndex() != 7 ||
                        !activityPanel->isHidden()) {
                        fail(99, "Active encode leaked into Recover presentation");
                        return;
                    }
                    create->click();
                    QApplication::processEvents();
                    if (stack->currentIndex() != 4 ||
                        activityPanel->isHidden()) {
                        fail(100, "Active encode did not survive navigation");
                        return;
                    }
                    state->activeEncodeNavigationChecked = true;
                }
                window.grab().save(QDir(root).filePath(
                    "e2e-create-processing-en.png"));
                const QString savedInput = input->text();
                const QString savedOutput = output->text();
                language->setCurrentIndex(language->findData("tr"));
                language->setCurrentIndex(language->findData("en"));
                if (stack->currentIndex() != 4 ||
                    input->text() != savedInput ||
                    output->text() != savedOutput ||
                    !highCapacity->isChecked() ||
                    progressContinue->isEnabled()) {
                    fail(55, "Language switch changed an active encode workflow");
                    return;
                }
                state->testedActiveLanguageSwitch = true;
            }
            if (state->stage == 4 &&
                activityPanel->property("observedScan").toBool()) {
                state->sawScan = true;
                if (recover->isEnabled()) {
                    if (activityPanel->isHidden() ||
                        activityTitle->text() !=
                            "Scanning downloaded videos" ||
                        !activityDescription->text().contains(
                            "The original file is not being rebuilt yet.")) {
                        fail(45, "Completed scan activity is not clearly identified");
                        return;
                    }
                }
                if (activityFlow->mode() !=
                        VidStoreXProcessingFlow::Mode::Scan ||
                    activityProgress->state() ==
                        VidStoreXBlockProgress::State::Success &&
                    !recover->isEnabled()) {
                    fail(46, "Scan processing visualization state is invalid");
                    return;
                }
            }
            if (state->stage == 5 && !activityPanel->isHidden() &&
                (activityTitle->text() == "Your file is being rebuilt" ||
                 activityTitle->text() == "Final verification is in progress")) {
                state->sawRecovery = true;
                if (!state->activeRecoveryNavigationChecked) {
                    homeNavigation->click();
                    QApplication::processEvents();
                    if (stack->currentIndex() != 0 ||
                        !activityPanel->isHidden()) {
                        fail(101, "Active recovery leaked into Home presentation");
                        return;
                    }
                    recoverChoice->click();
                    QApplication::processEvents();
                    if (stack->currentIndex() != 8 ||
                        activityPanel->isHidden()) {
                        fail(102, "Active recovery did not survive navigation");
                        return;
                    }
                    state->activeRecoveryNavigationChecked = true;
                }
            }
            if (state->stage == 4 && recover->isEnabled()) {
                if (!state->sawScan) {
                    fail(47, QString(
                        "Assistant did not expose a distinct scan phase "
                        "(observed=%1, visible=%2, title=%3, description=%4)")
                        .arg(activityPanel->property("observedScan").toBool())
                        .arg(!activityPanel->isHidden())
                        .arg(activityTitle->text(),
                             activityDescription->text()));
                    return;
                }
                if (!scanSummary->text().contains("Everything is ready")) {
                    fail(38, "Assistant scan did not report a complete set");
                    return;
                }
                window.grab().save(QDir(root).filePath("e2e-scan-en.png"));
                recover->click();
                state->stage = 5;
                qInfo() << "Assistant E2E stage 5: recovering";
                return;
            }
            if (state->stage == 5 && stack->currentIndex() == 9) {
                if (!state->sawRecovery ||
                    !state->activeRecoveryNavigationChecked ||
                    !activityPanel->property("observedFinalHash").toBool()) {
                    fail(48, "Recovery or final SHA phase was not observed");
                    return;
                }
                if (!success->text().contains("recovered exactly")) {
                    fail(39, "Assistant exact-success screen was not shown");
                    return;
                }
                if (!state->recoveryCardDone) {
                    QString cardError;
                    if (!exerciseResultCardPreview(
                            recoveryResultCard,
                            QDir(root).filePath("recovery-result-card.png"),
                            {QStringLiteral("source.bin"),
                             QStringLiteral("High Capacity"),
                             QStringLiteral("YouTube Round-Trip"),
                             QStringLiteral("SHA-256"),
                             QStringLiteral("Match")},
                            {QDir::fromNativeSeparators(root)},
                            QDir(root).filePath(
                                "recovery-result-card-preview.png"),
                            &cardError)) {
                        fail(75, QStringLiteral(
                            "Recovery result card E2E failed: ") + cardError);
                        return;
                    }
                    state->recoveryCardDone = true;
                }
                if (!successRail->isVisible() || successRail->height() < 1 ||
                    !window.grab().save(
                        QDir(root).filePath("e2e-success-en.png"))) {
                    fail(61, "Exact-success signature or visual audit is missing");
                    return;
                }
                const QString recoveredFile = QDir(recovered).filePath("source.bin");
                if (!QFileInfo::exists(recoveredFile)) {
                    fail(40, "Assistant recovered file is missing");
                    return;
                }
                const auto sourceSha = video_set::sha256_file(
                    std::filesystem::path(source.toStdWString()));
                const auto recoveredSha = video_set::sha256_file(
                    std::filesystem::path(recoveredFile.toStdWString()));
                if (sourceSha != recoveredSha) {
                    fail(41, "Assistant recovered SHA-256 does not match source");
                    return;
                }
                homeNavigation->click();
                language->setCurrentIndex(language->findData("tr"));
                QApplication::processEvents();
                auto *recentEntry = recent->itemWidget(recent->item(0));
                auto *recentTitle = recentEntry
                    ? recentEntry->findChild<QLabel *>(
                        "videoSetRecentEntryTitle0") : nullptr;
                auto *recentMetadata = recentEntry
                    ? recentEntry->findChild<QLabel *>(
                        "videoSetRecentEntryMetadata0") : nullptr;
                auto *recentStatus = recentEntry
                    ? recentEntry->findChild<QLabel *>(
                        "videoSetRecentEntryStatus0") : nullptr;
                const bool recentSaved = window.grab().save(
                    QDir(root).filePath("e2e-home-tr-one-recent.png"));
                const bool titleMetadataOverlap = recentTitle && recentMetadata &&
                    recentTitle->geometry().intersects(recentMetadata->geometry());
                const bool titleStatusOverlap = recentTitle && recentStatus &&
                    recentTitle->geometry().intersects(recentStatus->geometry());
                const bool metadataStatusOverlap = recentMetadata && recentStatus &&
                    recentMetadata->geometry().intersects(recentStatus->geometry());
                const bool homeScroll = assistantScroll &&
                    assistantScroll->verticalScrollBar()->isVisible();
                if (!recentTitle || !recentMetadata || !recentStatus ||
                    titleMetadataOverlap || titleStatusOverlap ||
                    metadataStatusOverlap || recent->height() > 110 ||
                    homeScroll || !recentSaved) {
                    fail(65, QString(
                        "Recent invariant failed: labels=%1/%2/%3 overlap=%4/%5/%6 "
                        "height=%7 scroll=%8 saved=%9")
                        .arg(recentTitle != nullptr)
                        .arg(recentMetadata != nullptr)
                        .arg(recentStatus != nullptr)
                        .arg(titleMetadataOverlap).arg(titleStatusOverlap)
                        .arg(metadataStatusOverlap).arg(recent->height())
                        .arg(homeScroll).arg(recentSaved));
                    return;
                }
                const QStringList originalRecent = QSettings().value(
                    "videoSet/recentManifests").toStringList();
                if (!originalRecent.isEmpty()) {
                    QSettings().setValue("videoSet/recentManifests",
                        QStringList(5, originalRecent.front()));
                    homeNavigation->click();
                    window.resize(1920, 1080);
                    QApplication::processEvents();
                    if (recent->count() != 3 ||
                        recent->verticalScrollBar()->isVisible() ||
                        recent->height() < 3 * 52 ||
                        !window.grab().save(QDir(root).filePath(
                            "e2e-home-tr-five-recent.png"))) {
                        fail(66, "Home Recent preview was not capped at three rows");
                        return;
                    }
                    recentNavigation->click();
                    QApplication::processEvents();
                    if (recentFull->count() != 5 ||
                        recentFull->item(0)->data(Qt::UserRole) !=
                            recent->item(0)->data(Qt::UserRole) ||
                        !window.grab().save(QDir(root).filePath(
                            "e2e-recent-tr-five-recent.png"))) {
                        fail(103, "Dedicated Recent did not show the full model");
                        return;
                    }
                    QSettings().setValue(
                        "videoSet/recentManifests", originalRecent);
                    window.resize(1366, 768);
                    homeNavigation->click();
                    QApplication::processEvents();
                }
                if (window.property("uiLanguage").toString() != "tr" ||
                    !window.property("uiTranslationLoaded").toBool() ||
                    stack->currentIndex() != 0 ||
                    brandSubtitle->text() != QString::fromUtf8(
                        "DOSYA → VİDEO → DOSYA · DİJİTAL ARŞİV") ||
                    create->text() != QString::fromUtf8("Dosya Seç") ||
                    activityTitle->text() != QCoreApplication::translate(
                        "DriveManagerUI", "Your file is being rebuilt") ||
                    success->text() != QString::fromUtf8(
                        "Dosyanız birebir kurtarıldı.")) {
                    fail(52, QString(
                        "Runtime English-to-Turkish translation failed: "
                        "language=%1 loaded=%2 stack=%3 brand=%4 create=%5 "
                        "activity=%6 success=%7")
                        .arg(window.property("uiLanguage").toString())
                        .arg(window.property("uiTranslationLoaded").toBool())
                        .arg(stack->currentIndex())
                        .arg(brandSubtitle->text(), create->text(),
                             activityTitle->text(), success->text()));
                    return;
                }
                stack->setCurrentIndex(2);
                QApplication::processEvents();
                if (resilientCard->title() != QString::fromUtf8("En Güvenli") ||
                    highCapacityCard->title() != QString::fromUtf8(
                        "Daha Az ve Daha Kısa Video") ||
                    !highCapacity->isChecked()) {
                    fail(53, "Turkish profile cards or selected profile changed");
                    return;
                }
                language->setCurrentIndex(language->findData("en"));
                QApplication::processEvents();
                if (window.property("uiLanguage").toString() != "en" ||
                    stack->currentIndex() != 2 ||
                    input->text() != source || output->text() != sets ||
                    recoveryInput->text() != returned ||
                    recoveryOutput->text() != recovered ||
                    !highCapacity->isChecked() || recent->count() != 1) {
                    fail(54, "Runtime Turkish-to-English switch changed workflow state");
                    return;
                }
                language->setCurrentIndex(language->findData("tr"));
                QApplication::processEvents();
                if (state->returnedFiles.isEmpty() ||
                    !QFile::remove(state->returnedFiles.front())) {
                    fail(42, "Could not prepare missing-part GUI scenario");
                    return;
                }
                stack->setCurrentIndex(7);
                scan->click();
                state->stage = 6;
                qInfo() << "Assistant E2E stage 6: checking missing part";
                return;
            }
            if (state->stage == 6 && scan->isEnabled() &&
                (scanSummary->text().contains("missing",
                     Qt::CaseInsensitive) ||
                 scanSummary->text().contains(QString::fromUtf8("eksik"),
                     Qt::CaseInsensitive))) {
                if (recover->isEnabled()) {
                    fail(43, "Recover remained enabled with a missing part");
                    return;
                }
                if (!activityParts->parts().contains(
                        VidStoreXPartState::Missing) ||
                    activityFlow->mode() !=
                        VidStoreXProcessingFlow::Mode::Scan ||
                    !window.grab().save(QDir(root).filePath(
                        "e2e-missing-part-tr.png"))) {
                    fail(72, QString(
                        "Missing part was not represented in the processing grid "
                        "(parts=%1 mode=%2 summary=%3)")
                        .arg(activityParts->parts().size())
                        .arg(static_cast<int>(activityFlow->mode()))
                        .arg(scanSummary->text()));
                    return;
                }
                if (state->sourceVideoFiles.isEmpty() ||
                    !QFile::copy(state->sourceVideoFiles.front(),
                                 state->returnedFiles.front())) {
                    fail(49, "Could not restore the missing part for corrupt test");
                    return;
                }
                QFile corruptCandidate(state->returnedFiles.at(1));
                if (!corruptCandidate.open(QIODevice::ReadWrite) ||
                    !corruptCandidate.seek(0) ||
                    corruptCandidate.write("X", 1) != 1) {
                    fail(50, "Could not prepare corrupt-part GUI scenario");
                    return;
                }
                corruptCandidate.close();
                scan->click();
                state->stage = 7;
                qInfo() << "Assistant E2E stage 7: checking corrupt part";
                return;
            }
            if (state->stage == 7 && scan->isEnabled() &&
                (scanSummary->text().contains("corrupt",
                     Qt::CaseInsensitive) ||
                 scanSummary->text().contains(QString::fromUtf8("bozuk"),
                     Qt::CaseInsensitive))) {
                if (recover->isEnabled()) {
                    fail(51, "Recover remained enabled with a corrupt part");
                    return;
                }
                if (!activityParts->parts().contains(
                        VidStoreXPartState::Corrupt) ||
                    !window.grab().save(QDir(root).filePath(
                        "e2e-corrupt-part-tr.png"))) {
                    fail(73, "Corrupt part was not represented in the processing grid");
                    return;
                }
                if (QSettings().value("ui/language").toString() != "tr") {
                    fail(56, "Runtime language preference was not persisted");
                    return;
                }
                qInfo() << "Assistant E2E complete";
                timer->stop();
                window.close();
                app.exit(0);
            }
        });
        timer->start();
    }

    // Deterministic, non-interactive launch check used by CTest. This only
    // exercises application/window construction and never touches files.
    if (smokeTest) {
        QTimer::singleShot(250, &app, [&app, &window]() {
            window.close();
            app.exit(0);
        });
    } else if (closeDuringEstimate) {
        QTimer::singleShot(10, &app, [&app, &window]() {
            window.close();
            app.exit(0);
        });
    }
    
    return QApplication::exec();
}
