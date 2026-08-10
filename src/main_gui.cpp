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
#include <QDebug>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QLineEdit>
#include <QDir>
#include <QFile>
#include <QFrame>
#include <QIcon>
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
#endif

} // namespace

int main(int argc, char *argv[]) {
    bool smokeTest = false;
    bool closeDuringEstimate = false;
    QString preflightSmokeInput;
    QString preflightSmokeOutput;
    QString videoSetAssistantSmokeRoot;
    QString instantRecoverySmokeRoot;
    QString instantRecoveryFakeYtDlp;
    QString instantRecoveryFixtureVideos;
    QString youtubeSyncSmokeRoot;
    QString youtubeSyncManifest;
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
        } else if (argument == "--instant-recovery-smoke-root" &&
                   i + 1 < argc) {
            instantRecoverySmokeRoot = QString::fromLocal8Bit(argv[++i]);
        } else if (argument == "--instant-recovery-fake-ytdlp" &&
                   i + 1 < argc) {
            instantRecoveryFakeYtDlp = QString::fromLocal8Bit(argv[++i]);
        } else if (argument == "--instant-recovery-fixture-videos" &&
                   i + 1 < argc) {
            instantRecoveryFixtureVideos = QString::fromLocal8Bit(argv[++i]);
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
        smokeTest || closeDuringEstimate;
    if (isolatedUiRun) {
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(
            QSettings::IniFormat, QSettings::UserScope,
            !videoSetAssistantSmokeRoot.isEmpty()
                ? QDir(videoSetAssistantSmokeRoot).filePath("settings")
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
    
    // Set application properties
    QApplication::setApplicationName("YouTube Media Storage");
    QApplication::setApplicationDisplayName("VidStoreX");
    QApplication::setApplicationVersion("1.0");
    QApplication::setOrganizationName("Media Storage");
    QApplication::setOrganizationDomain("brandonli.me");
    if (isolatedUiRun) {
        QSettings settings;
        settings.setValue("ui/language", "en");
        settings.setValue("ui/rememberRecentSets", true);
        settings.setValue("ui/showAdvancedTools", true);
        if (!instantRecoveryFakeYtDlp.isEmpty())
            settings.setValue("videoSet/ytdlpPath",
                              instantRecoveryFakeYtDlp);
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
    
    app.setWindowIcon(vidStoreXApplicationIcon());
    
    // Enable high DPI scaling (deprecated in Qt6, but kept for compatibility)
    // app.setAttribute(Qt::AA_EnableHighDpiScaling);
    // app.setAttribute(Qt::AA_UseHighDpiPixmaps);
    
    // Set style to a modern look if available
    if (QStyleFactory::keys().contains("Fusion")) {
        QApplication::setStyle("Fusion");
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
        qputenv("VIDSTOREX_FAKE_YTDLP_SOURCE",
                instantRecoveryFixtureVideos.toUtf8());
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
        if (!recoverNavigation || !playlist || !output || !start ||
            !status || !success) {
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
        QObject::connect(timer, &QTimer::timeout, &window,
            [&app, &window, status, success, recovered, timer, elapsed]() {
            *elapsed += 100;
            if (success->isVisible() && success->text().contains(
                    "recovered exactly", Qt::CaseInsensitive)) {
                if (!QFileInfo::exists(QDir(recovered).filePath("source.bin"))) {
                    qCritical() << "Instant Recovery exact output is missing";
                    app.exit(82);
                } else {
                    qInfo() << "Instant Recovery qwindows E2E complete:"
                            << status->text();
                    app.exit(0);
                }
                timer->stop();
                window.close();
                delete elapsed;
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
        auto *activityProgress = window.findChild<QProgressBar *>(
            "videoSetProgressBar");
        auto *technicalToggle = window.findChild<QToolButton *>(
            "videoSetTechnicalLogToggle");
        auto *technicalLog = window.findChild<QTextEdit *>(
            "videoSetTechnicalLog");
        auto *homeNavigation = window.findChild<QPushButton *>(
            "homeNavigationButton");
        auto *settingsNavigation = window.findChild<QPushButton *>(
            "settingsNavigationButton");
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
            technicalLog->document()->maximumBlockCount() != 5000 ||
            homeHeading->text() == QStringLiteral("VidStoreX") ||
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
            if (label->isVisible() && label->text() == QStringLiteral("VidStoreX"))
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
                                << button->objectName();
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
            !resilientChoice->isChecked() || highCapacityChoice->isChecked()) {
            qCritical() << "Create flow or Resilient default invariant failed";
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
        if (classicTools->isHidden() || !classicTools->isChecked()) {
            qCritical() << "Classic Video Set tools are inaccessible";
            return 11;
        }
        classicTools->setChecked(false);
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
        auto *recent = window.findChild<QListWidget *>(
            "videoSetRecentList");
        auto *activityPanel = window.findChild<QFrame *>(
            "videoSetActivityPanel");
        auto *activityTitle = window.findChild<QLabel *>(
            "videoSetActivityTitle");
        auto *activityDescription = window.findChild<QLabel *>(
            "videoSetActivityDescription");
        auto *activityProgress = window.findChild<QProgressBar *>(
            "videoSetProgressBar");
        auto *language = window.findChild<QComboBox *>(
            "uiLanguageCombo");
        auto *homeNavigation = window.findChild<QPushButton *>(
            "homeNavigationButton");
        auto *settingsNavigation = window.findChild<QPushButton *>(
            "settingsNavigationButton");
        auto *advancedNavigation = window.findChild<QToolButton *>(
            "advancedNavigationButton");
        auto *brandSubtitle = window.findChild<QLabel *>(
            "brandSubtitle");
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
            !highCapacity || !target || !maximumSize || !calculate ||
            !planSummary || !createVideos || !progressContinue ||
            !progressPart || !uploaded || !recoveryInput ||
            !recoveryOutput || !scan || !recover || !scanSummary ||
            !success || !recent || !activityPanel || !activityTitle ||
            !activityDescription || !activityProgress || !language ||
            !homeNavigation || !brandSubtitle || !resilientCard ||
            !highCapacityCard || !recoverChoice || !settingsNavigation ||
            !advancedNavigation || !homeHeading || !trustLabel ||
            !classicTools || !successRail || !capacityHeading ||
            !capacityAction || !youtubeSyncAction || !youtubeSyncPage ||
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
            if (label->isVisible() && label->text() == QStringLiteral("VidStoreX"))
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
                "Gelişmiş / Klasik Video Set Araçları")) {
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
        const QPalette originalPalette = window.palette();
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
        qApp->setPalette(originalPalette);
        window.setPalette(originalPalette);
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
            oauthConfig->isVisible() || syncCard->isVisible()) {
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
                QString::fromUtf8("normal VidStoreX"), Qt::CaseInsensitive)) {
            qCritical() << "Turkish Experimental YouTube Sync separation failed";
            return 68;
        }
        window.grab().save(QDir(root).filePath(
            "e2e-youtube-sync-experimental-tr.png"));
        language->setCurrentIndex(language->findData("en"));
        QApplication::processEvents();
        if (!youtubeSyncHeading->text().contains("Experimental") ||
            !youtubeSyncWarning->text().contains(
                "not required for normal VidStoreX use",
                Qt::CaseInsensitive)) {
            qCritical() << "English Experimental YouTube Sync separation failed";
            return 69;
        }
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
        };
        auto *state = new SmokeState{
            0, QDateTime::currentMSecsSinceEpoch() + 110000, {}, {}, {},
            false, false, false};
        auto *timer = new QTimer(&app);
        timer->setInterval(100);
        QObject::connect(timer, &QTimer::timeout, &app,
            [&, state, timer, root, source, sets, returned, recovered]() {
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
                        .arg(activityPanel->property("observedFinalHash").toBool()));
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
                if (!progressPart->text().contains("verified locally")) {
                    fail(34, "Assistant did not show local exact completion");
                    return;
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
                uploaded->click();
                recoveryInput->setText(returned);
                recoveryOutput->setText(recovered);
                stack->setCurrentIndex(7);
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
                if (activityProgress->maximum() == 0 &&
                    activityProgress->minimum() != 0) {
                    fail(46, "Scan progress range is invalid");
                    return;
                }
            }
            if (state->stage == 5 && !activityPanel->isHidden() &&
                activityTitle->text() == "Recovering the original file")
                state->sawRecovery = true;
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
                    !activityPanel->property("observedFinalHash").toBool()) {
                    fail(48, "Recovery or final SHA phase was not observed");
                    return;
                }
                if (!success->text().contains("recovered exactly")) {
                    fail(39, "Assistant exact-success screen was not shown");
                    return;
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
                    if (recent->count() != 5 ||
                        !recent->verticalScrollBar()->isVisible() ||
                        recent->height() < 3 * 58 ||
                        !window.grab().save(QDir(root).filePath(
                            "e2e-home-tr-five-recent.png"))) {
                        fail(66, "Multiple Recent rows did not use bounded scrolling");
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
                    !activityTitle->text().contains(
                        QString::fromUtf8("Orijinal dosya")) ||
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
