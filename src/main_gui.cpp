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
#include <QComboBox>
#include <QDebug>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QLineEdit>
#include <QDir>
#include <QFile>
#include <QFrame>
#include <QListWidget>
#include <QRegularExpression>
#include <QSettings>
#include <QSet>
#include <QTimer>
#include <QStyleFactory>
#include <QTextDocument>

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
    const bool isolatedUiRun = !videoSetAssistantSmokeRoot.isEmpty() ||
        smokeTest || closeDuringEstimate;
    if (isolatedUiRun) {
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(
            QSettings::IniFormat, QSettings::UserScope,
            !videoSetAssistantSmokeRoot.isEmpty()
                ? QDir(videoSetAssistantSmokeRoot).filePath("settings")
                : QDir(QDir::tempPath()).filePath(
                    "vidstorex-gui-smoke-settings"));
    }
    QApplication app(argc, argv);
    
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
    }
    
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
        auto *classicAction = window.findChild<QAction *>(
            "advancedClassicVideoSetAction");
        if (!assistantStack || !assistantScroll || !createChoice ||
            !recoverChoice || !resilientChoice || !highCapacityChoice ||
            !advancedToggle || !advancedPanel || !classicTools ||
            !activityPanel || !activityTitle || !activityProgress ||
            !technicalToggle || !technicalLog || !homeNavigation ||
            !settingsNavigation || !language || !settingsLanguage ||
            !settingsPage || !advancedNavigation || !trustLabel ||
            !homeHeading || !createRail || !recoverRail || !stepper ||
            !recentEmpty || !classicAction) {
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
            !recentEmpty->isVisible() ||
            (advancedNavigation->text() != QStringLiteral("Advanced") &&
             advancedNavigation->text() != QString::fromUtf8("Gelişmiş"))) {
            qCritical() << "Video Set Assistant welcome/scroll/focus invariant failed";
            return 6;
        }
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
            settingsLanguage->currentData() != language->currentData()) {
            qCritical() << "Settings page language selector is not synchronized";
            return 17;
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
            !capacityAction) {
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
        if (!window.grab().save(QDir(root).filePath("e2e-home-tr.png"))) {
            qCritical() << "Could not save Turkish Home visual audit";
            return 58;
        }
        capacityAction->trigger();
        QApplication::processEvents();
        if (!capacityHeading->isVisible() ||
            capacityHeading->text() != QString::fromUtf8(
                "Gelişmiş / Kapasite Laboratuvarı")) {
            qCritical() << "Turkish Advanced Capacity wrapper failed";
            return 59;
        }
        settingsNavigation->click();
        QApplication::processEvents();
        auto *settingsLanguageSection = window.findChild<QLabel *>(
            "settingsLanguageSection");
        if (!settingsLanguageSection ||
            settingsLanguageSection->text() != QString::fromUtf8("Dil")) {
            qCritical() << "Turkish Settings sections failed";
            return 60;
        }
        language->setCurrentIndex(language->findData("en"));
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
                    !recent->item(0)->text().contains("Last opened:") ||
                    recent->item(0)->text().contains(state->setRoot,
                        Qt::CaseInsensitive) ||
                    recent->item(0)->text().contains("set_manifest",
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
