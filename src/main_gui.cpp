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
#include <QComboBox>
#include <QDebug>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QLineEdit>
#include <QDir>
#include <QFile>
#include <QListWidget>
#include <QRegularExpression>
#include <QSettings>
#include <QTimer>
#include <QStyleFactory>

#include <algorithm>
#include <filesystem>

#include "drive_manager_ui.h"

int main(int argc, char *argv[]) {
    bool smokeTest = false;
    bool closeDuringEstimate = false;
    QString preflightSmokeInput;
    QString preflightSmokeOutput;
    QString videoSetAssistantSmokeRoot;
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
        }
    }
    if (!videoSetAssistantSmokeRoot.isEmpty()) {
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(
            QSettings::IniFormat, QSettings::UserScope,
            QDir(videoSetAssistantSmokeRoot).filePath("settings"));
    }
    QApplication app(argc, argv);
    
    // Set application properties
    QApplication::setApplicationName("YouTube Media Storage");
    QApplication::setApplicationDisplayName("Drive Manager");
    QApplication::setApplicationVersion("1.0");
    QApplication::setOrganizationName("Media Storage");
    QApplication::setOrganizationDomain("brandonli.me");
    
    // Set application icon (if available)
    // app.setWindowIcon(QIcon(":/icons/app_icon.png"));
    
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
        if (!videoSetValidation->text().contains("Real YouTube") ||
            !videoSetValidation->text().contains("4/4 parts") ||
            !videoSetValidation->text().contains("full-file SHA exact") ||
            !videoSetValidation->text().contains(
                "successful only after the final full-file SHA-256 matches")) {
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
        if (!assistantStack || !assistantScroll || !createChoice ||
            !recoverChoice || !resilientChoice || !highCapacityChoice ||
            !advancedToggle || !advancedPanel || !classicTools) {
            qCritical() << "Video Set Assistant controls were not found";
            return 6;
        }
        window.resize(1280, 720);
        QApplication::processEvents();
        if (assistantStack->currentIndex() != 0 ||
            !assistantScroll->widgetResizable() ||
            window.minimumHeight() > 720 ||
            createChoice->focusPolicy() == Qt::NoFocus ||
            recoverChoice->focusPolicy() == Qt::NoFocus) {
            qCritical() << "Video Set Assistant welcome/scroll/focus invariant failed";
            return 6;
        }
        createChoice->click();
        QApplication::processEvents();
        if (assistantStack->currentIndex() != 1 ||
            !resilientChoice->isChecked() || highCapacityChoice->isChecked()) {
            qCritical() << "Create flow or Resilient default invariant failed";
            return 7;
        }
        advancedToggle->setChecked(false);
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
        classicTools->setChecked(true);
        QApplication::processEvents();
        if (classicTools->isHidden()) {
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
        if (!stack || !create || !input || !output || !sourceContinue ||
            !highCapacity || !target || !maximumSize || !calculate ||
            !planSummary || !createVideos || !progressContinue ||
            !progressPart || !uploaded || !recoveryInput ||
            !recoveryOutput || !scan || !recover || !scanSummary ||
            !success || !recent) {
            qCritical() << "Assistant E2E smoke controls were not found";
            return 31;
        }

        struct SmokeState {
            int stage = 0;
            qint64 deadline = 0;
            QString setRoot;
            QStringList returnedFiles;
        };
        auto *state = new SmokeState{
            0, QDateTime::currentMSecsSinceEpoch() + 110000, {}, {}};
        auto *timer = new QTimer(&app);
        timer->setInterval(100);
        QObject::connect(timer, &QTimer::timeout, &app,
            [&, state, timer, root, source, sets, returned, recovered]() {
            const auto fail = [&](const int code, const QString &message) {
                qCritical() << message;
                timer->stop();
                window.close();
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
                    .arg(recover->isEnabled()));
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
                if (recent->count() != 1 ||
                    !recent->item(0)->text().contains("Last opened:")) {
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
                }
                progressContinue->click();
                uploaded->click();
                recoveryInput->setText(returned);
                recoveryOutput->setText(recovered);
                stack->setCurrentIndex(7);
                scan->click();
                state->stage = 4;
                qInfo() << "Assistant E2E stage 4: scanning returned videos";
                return;
            }
            if (state->stage == 4 && recover->isEnabled()) {
                if (!scanSummary->text().contains("found and verified")) {
                    fail(38, "Assistant scan did not report a complete set");
                    return;
                }
                recover->click();
                state->stage = 5;
                qInfo() << "Assistant E2E stage 5: recovering";
                return;
            }
            if (state->stage == 5 && stack->currentIndex() == 9) {
                if (!success->text().contains("recovered exactly")) {
                    fail(39, "Assistant exact-success screen was not shown");
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
                scanSummary->text().contains("missing",
                    Qt::CaseInsensitive)) {
                if (recover->isEnabled()) {
                    fail(43, "Recover remained enabled with a missing part");
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
