/*
 * This file is part of yt-media-storage, a tool for encoding media.
 * Copyright (C) 2026 Brandon Li <https://brandonli.me/>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include <QMainWindow>
#include <QHBoxLayout>
#include <QPushButton>
#include <QLabel>
#include <QProgressBar>
#include <QTextEdit>
#include <QFileDialog>
#include <QMessageBox>
#include <QListWidget>
#include <QSplitter>
#include <QScrollArea>
#include <QGroupBox>
#include <QLineEdit>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QTimer>
#include <QThread>
#include <QCheckBox>
#include <QToolButton>
#include <QDateTime>
#include <QProcess>
#include <QTableWidget>
#include <QTabWidget>
#include <QStackedWidget>
#include <QRadioButton>
#include <QButtonGroup>
#include <QTranslator>
#include <QVector>

#include <memory>
#include <optional>

#include "configuration.h"
#include "encoding_reliability.h"
#include "gui_preflight_model.h"
#include "media_storage.h"
#include "result_card.h"
#include "video_set_workflow.h"
#include "youtube_network_service.h"

class VidStoreXSignalRail;
class VidStoreXStepper;
class VidStoreXFlowIllustration;
class VidStoreXOnboardingProgress;
class VidStoreXBlockProgress;
class VidStoreXPartGrid;
class VidStoreXProcessingFlow;
class BrandIntroOverlay;
class QAction;

class WorkerThread : public QThread {
    Q_OBJECT

public:
    enum Operation {
        Encode,
        Decode,
        StreamEncode,
        StreamDecode
    };

    WorkerThread(Operation op, const QString &input, const QString &output,
                 bool encrypt = false, const QString &password = QString(),
                 const QString &streamUrl = QString(), int bitrate = FRAME_BITRATE,
                 int streamWidth = FRAME_WIDTH_STREAM, int streamHeight = FRAME_HEIGHT_STREAM,
                 int streamFps = FRAME_FPS,
                 double repairRatio = DEFAULT_REPAIR_RATIO,
                 std::optional<ms_encoding_estimate_t> preflightEstimate =
                     std::nullopt,
                 bool allowLowDisk = false,
                 QObject *parent = nullptr);

signals:
    void progressUpdated(int percentage);

    void statusUpdated(const QString &status);

    void operationCompleted(bool success, const QString &message, int status);

    void logMessage(const QString &message);

protected:
    void run() override;

private:
    Operation operation;
    QString inputPath;
    QString outputPath;
    bool encrypt;
    QString password;
    QString streamUrl;
    int bitrate;
    int streamWidth;
    int streamHeight;
    int streamFps;
    double repairRatio;
    std::optional<ms_encoding_estimate_t> preflightEstimate;
    bool allowLowDisk;
};

struct GuiPreflightJob {
    uint64_t generation = 0;
    GuiPreflightFingerprint fingerprint;
    QString inputPath;
    QString outputPath;
    bool encrypted = false;
    QString password;
    double repairRatio = DEFAULT_REPAIR_RATIO;
    ms_encoding_mode_t encodingMode = MS_ENCODING_MODE_RESILIENT;
};

class PreflightEstimateThread : public QThread {
    Q_OBJECT

public:
    explicit PreflightEstimateThread(
        GuiPreflightJob job, QObject *parent = nullptr);

    [[nodiscard]] const GuiPreflightJob &job() const noexcept {
        return job_;
    }

    [[nodiscard]] ms_status_t resultStatus() const noexcept {
        return result_status_;
    }

    [[nodiscard]] const ms_encoding_estimate_t &estimate() const noexcept {
        return estimate_;
    }

signals:
    void phaseChanged(uint64_t generation, const QString &phase);

protected:
    void run() override;

private:
    GuiPreflightJob job_;
    ms_status_t result_status_ = MS_ERR_IO;
    ms_encoding_estimate_t estimate_{};
};

class DriveManagerUI : public QMainWindow {
    Q_OBJECT

public:
    explicit DriveManagerUI(QWidget *parent = nullptr);

    ~DriveManagerUI() override;

protected:
    bool eventFilter(QObject *object, QEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
    void changeEvent(QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private
slots:
    void selectInputFile();

    void selectOutputFile();

    void selectInputDirectory();

    void selectOutputDirectory();

    void startEncode();

    void startDecode();

    void startBatchEncode();

    void startStreamEncode();

    void startStreamDecode();

    void onPlatformChanged(int index) const;

    void onResolutionChanged(int index) const;

    void onReliabilityProfileChanged(int index);

    void onEncodingModeChanged(int index);

    void onPreflightInputChanged();

    void onCustomRepairChanged();

    void runDebouncedPreflight();

    void onLowDiskOverrideToggled(bool checked);

    void clearLogs() const;

    void onOperationCompleted(
        bool success, const QString &message, int status);

    void onProgressUpdated(int percentage) const;

    void onStatusUpdated(const QString &status) const;

    void onLogMessage(const QString &message) const;

    void updateFileList() const;

    void removeSelectedFiles() const;

    void clearFileList() const;

    void togglePasswordVisibility() const;

private:
    void setupUI();

    void setupMenuBar();

    void setupStatusBar();

    void connectSignals();

    void resetProgress();

    void logMessage(const QString &message) const;

    void loadSettings();

    void saveSettings() const;

    bool validatePaths();

    void requestPreflight(bool force = false);

    void startPreflightJob(const GuiPreflightJob &job);

    void onPreflightFinished(PreflightEstimateThread *thread);

    void updatePreflightPanel();

    void updateEncodeEligibility();

    [[nodiscard]] std::optional<GuiPreflightFingerprint>
    currentPreflightFingerprint(
        GuiPreflightStatus *waitingStatus = nullptr) const;

    [[nodiscard]] GuiPreflightSnapshot currentSnapshot() const;

    void clearPreflightValues();

    void logPreflightEstimate() const;

    [[nodiscard]] bool confirmOverwrite() const;

    void launchEncode();

    [[nodiscard]] EncodingReliabilityOptions selectedReliabilityOptions() const;

    [[nodiscard]] ReliabilityProfile selectedReliabilityProfile() const;

    [[nodiscard]] ms_encoding_mode_t selectedEncodingMode() const;

    [[nodiscard]] bool isCustomReliabilityProfile() const;

    void logReliabilityEstimate(
        const EncodingReliabilityEstimate &estimate,
        const EncodingReliabilityOptions &options) const;

    void startTestLabProcess(const QStringList &arguments);

    void refreshTestLabDashboard();

    void refreshCapacityLabDashboard();

    void setupVideoSetAssistant(QGroupBox *classicEncodeGroup,
                                QGroupBox *classicRecoveryGroup);

    void setupApplicationNavigation();

    void setupSettingsPage();

    void setupOnboardingPage();

    void updateResponsiveLayout(const QSize &viewport);

    void applySemanticVisualRoles();

    void updateNavigationVisuals();

    void setUiLanguage(const QString &language, bool persist = true);

    void retranslateUserInterface();

    void showOnboarding();

    void showBrandIntro();

    void completeBrandIntro();

    void setOnboardingPage(int page);

    void completeOnboarding();

    void updateOnboardingAvailability();

    [[nodiscard]] bool canOpenOnboarding() const;

    [[nodiscard]] QString translatedWorkflowText(
        const std::string &english) const;

    void showVideoSetHome();

    void showVideoSetRecent();

    void showVideoSetCreate();

    void showVideoSetRecover();

    void updateProfileCardVisuals();
    void updateVideoSetPlanSummaryText();

    void updateVideoSetAssistant();

    void startVideoSetProcess(const QStringList &arguments,
                              bool assistantOperation = true);

    void handleVideoSetOutput(const QString &text);

    void handleVideoSetProgressOutput(const QString &text);

    void renderVideoSetActivity();

    [[nodiscard]] video_set_workflow::PresentationPage
    currentOperationPresentationPage() const noexcept;

    [[nodiscard]] QString currentVideoSetPlanInputKey() const;

    [[nodiscard]] bool currentVideoSetPlanMatches() const;

    void updateWizardActionBarVisibility();

    void handleVideoSetFinished(int exitCode,
                                QProcess::ExitStatus exitStatus);

    void calculateVideoSetPlan();

    void startVideoSetEncode(bool resume);

    void startVideoSetScan();

    void startVideoSetRecovery(bool resume);

    void startVideoSetDownload();

    void startYouTubeSync();
    void startYouTubeReadinessProbe();

    void handleYouTubeSyncResponse(
        int status, const QByteArray &body,
        const QList<QPair<QByteArray, QByteArray>> &headers);

    void continueYouTubeUpload();

    [[nodiscard]] QString findYtDlpExecutable() const;

    void refreshRecentVideoSets();

    void rememberRecentVideoSet(const QString &manifestPath);

    void openRecentVideoSet(const QString &manifestPath);

    [[nodiscard]] std::optional<result_card::Model>
    currentCreateResultCard() const;

    [[nodiscard]] std::optional<result_card::Model>
    currentRecoveryResultCard() const;

    void showCreateResultCard();

    void showRecoveryResultCard();

    [[nodiscard]] QStringList videoSetEncodeArguments(
        const QString &command) const;

    // UI Components
    QWidget *centralWidget = nullptr;
    QSplitter *mainSplitter;

    // Left panel - File operations
    QGroupBox *fileOperationsGroup;
    QLineEdit *inputFileEdit;
    QLineEdit *outputFileEdit;
    QPushButton *selectInputButton;
    QPushButton *selectOutputButton;
    QCheckBox *encryptCheckBox;
    QLineEdit *passwordEdit;
    QPushButton *passwordVisibilityButton;
    QComboBox *reliabilityProfileCombo;
    QDoubleSpinBox *repairPercentSpinBox;
    QLabel *reliabilityHelpLabel;
    QComboBox *encodingModeCombo;
    QLabel *encodingModeHelpLabel;
    QCheckBox *videoSetCheckBox = nullptr;
    QPushButton *encodeButton;
    QPushButton *decodeButton;

    // Preflight estimate
    QGroupBox *preflightGroup;
    QLabel *preflightStatusIcon;
    QLabel *preflightStatusValue;
    QProgressBar *preflightProgress;
    QLabel *preflightInputSizeValue;
    QLabel *preflightReliabilityValue;
    QLabel *preflightRepairValue;
    QLabel *preflightLikelyOutputValue;
    QLabel *preflightRangeValue;
    QLabel *preflightAvailableDiskValue;
    QLabel *preflightRequiredDiskValue;
    QLabel *preflightMissingDiskValue;
    QToolButton *preflightDetailsButton;
    QWidget *preflightDetailsWidget;
    QLabel *preflightSourcePacketsValue;
    QLabel *preflightRepairPacketsValue;
    QLabel *preflightTotalPacketsValue;
    QLabel *preflightFramesValue;
    QLabel *preflightVideoDurationValue;
    QLabel *preflightSafetyMarginValue;
    QLabel *preflightProbeFramesValue;
    QLabel *preflightProbeDurationValue;
    QLabel *preflightMethodValue;
    QLabel *preflightHeaderValue;
    QLabel *preflightFrameCapacityValue;
    QCheckBox *lowDiskOverrideCheckBox;

    // Batch operations
    QGroupBox *batchGroup;
    QListWidget *fileListWidget;
    QPushButton *addFilesButton;
    QPushButton *removeFilesButton;
    QPushButton *clearFilesButton;
    QPushButton *batchEncodeButton;
    QLineEdit *batchOutputDirEdit;
    QPushButton *batchOutputButton;

    // Streaming
    QGroupBox *streamGroup;
    QComboBox *platformCombo;
    QLineEdit *streamUrlEdit;
    QLineEdit *streamKeyEdit;
    QSpinBox *bitrateSpinBox;
    QSpinBox *fpsSpinBox;
    QComboBox *resolutionCombo;
    QPushButton *streamEncodeButton;
    QPushButton *streamDecodeButton;

    // Right panel - Status and logs
    QGroupBox *statusGroup;
    QProgressBar *progressBar;
    QLabel *statusLabel;
    QLabel *progressLabel;

    QGroupBox *logsGroup;
    QTextEdit *logTextEdit;
    QPushButton *clearLogsButton;

    // Menu and status bar
    QLabel *permanentStatus;

    // Settings
    QComboBox *qualityCombo;
    QComboBox *codecCombo;

    // YouTube Test Lab
    QTabWidget *mainTabs = nullptr;
    QFrame *applicationHeader = nullptr;
    QLabel *brandLabel = nullptr;
    QLabel *brandSubtitleLabel = nullptr;
    QPushButton *homeNavigationButton = nullptr;
    QPushButton *createNavigationButton = nullptr;
    QPushButton *recoverNavigationButton = nullptr;
    QPushButton *recentNavigationButton = nullptr;
    QToolButton *advancedNavigationButton = nullptr;
    QPushButton *settingsNavigationButton = nullptr;
    QComboBox *languageCombo = nullptr;
    QComboBox *settingsLanguageCombo = nullptr;
    QWidget *settingsPage = nullptr;
    QWidget *advancedLandingPage = nullptr;
    QWidget *classicToolsPage = nullptr;
    QWidget *testLabPage = nullptr;
    QWidget *capacityLabPage = nullptr;
    QWidget *youtubeSyncPage = nullptr;
    QWidget *onboardingPage = nullptr;
    BrandIntroOverlay *brandIntroOverlay = nullptr;
    QStackedWidget *onboardingStack = nullptr;
    VidStoreXOnboardingProgress *onboardingProgress = nullptr;
    QVector<QLabel *> onboardingTitles;
    QVector<QLabel *> onboardingDescriptions;
    QVector<QLabel *> onboardingSupportingLabels;
    QVector<VidStoreXFlowIllustration *> onboardingIllustrations;
    QPushButton *onboardingSkipButton = nullptr;
    QPushButton *onboardingBackButton = nullptr;
    QPushButton *onboardingNextButton = nullptr;
    QPushButton *settingsShowOnboardingButton = nullptr;
    QLabel *settingsOnboardingDescription = nullptr;
    QLabel *settingsAboutHeading = nullptr;
    QLabel *settingsAboutVersion = nullptr;
    QLabel *settingsAboutDefinition = nullptr;
    QLabel *settingsAboutAuthor = nullptr;
    QPushButton *settingsLinkedInButton = nullptr;
    QLabel *settingsLinkedInUrl = nullptr;
    QLabel *onboardingAuthorLabel = nullptr;
    QPushButton *onboardingLinkedInButton = nullptr;
    QAction *gettingStartedAction = nullptr;
    QLabel *settingsHeadingLabel = nullptr;
    QLabel *settingsDescriptionLabel = nullptr;
    QLabel *settingsLanguageLabel = nullptr;
    QLabel *settingsOutputLabel = nullptr;
    QLineEdit *settingsOutputEdit = nullptr;
    QPushButton *settingsOutputBrowseButton = nullptr;
    QCheckBox *rememberRecentCheckBox = nullptr;
    QCheckBox *showAdvancedToolsCheckBox = nullptr;
    QLabel *youtubeSyncConnectionLabel = nullptr;
    QLabel *youtubeSyncApiStatusLabel = nullptr;
    QLabel *youtubeSyncChannelLabel = nullptr;
    QLineEdit *youtubeOAuthConfigEdit = nullptr;
    QPushButton *youtubeOAuthConfigBrowseButton = nullptr;
    QPushButton *youtubeConnectButton = nullptr;
    QPushButton *youtubeDisconnectButton = nullptr;
    QPushButton *youtubeSetupInstructionsButton = nullptr;
    QComboBox *youtubeDefaultPrivacyCombo = nullptr;
    QCheckBox *youtubePrivacyTitlesCheckBox = nullptr;
    QCheckBox *youtubeAutoDownloadCheckBox = nullptr;
    QFrame *youtubeSyncOperationCard = nullptr;
    youtube_sync::YouTubeNetworkService *youtubeNetworkService = nullptr;
    bool youtubeAwaitingChannel = false;
    bool youtubePendingSyncAfterRefresh = false;
    QString youtubeSyncOperation;
    QString youtubeSyncAccessToken;
    QString youtubeSyncStatePath;
    youtube_sync::SyncState youtubeRuntimeSyncState;
    uint32_t youtubeSyncPartIndex = 0;
    uint32_t youtubeSyncRetryAttempt = 0;
    uint32_t youtubeProcessingPollAttempt = 0;
    qint64 youtubeProcessingStartedMs = 0;
    QProcess *youtubeReadinessProcess = nullptr;
    QTranslator *uiTranslator = nullptr;
    QString uiLanguage = QStringLiteral("en");
    bool uiTranslationLoaded = false;
    QComboBox *testLabPresetCombo = nullptr;
    QLineEdit *testLabOutputEdit = nullptr;
    QLineEdit *testLabManifestEdit = nullptr;
    QComboBox *testLabSimulationCombo = nullptr;
    QLineEdit *testLabVideoEdit = nullptr;
    QLineEdit *testLabCaseEdit = nullptr;
    QLineEdit *testLabFolderEdit = nullptr;
    QLineEdit *testLabMappingsEdit = nullptr;
    QLineEdit *testLabSessionLabelEdit = nullptr;
    QLabel *testLabActiveSessionLabel = nullptr;
    QLabel *testLabDuplicateWarning = nullptr;
    QCheckBox *testLabRecordNewCheck = nullptr;
    QLabel *testLabEstimateLabel = nullptr;
    QProgressBar *testLabProgress = nullptr;
    QTableWidget *testLabResults = nullptr;
    QTableWidget *testLabBatchPreview = nullptr;
    QPushButton *testLabGenerateButton = nullptr;
    QPushButton *testLabResumeButton = nullptr;
    QPushButton *testLabSimulateButton = nullptr;
    QPushButton *testLabAnalyzeButton = nullptr;
    QPushButton *testLabPreviewFolderButton = nullptr;
    QPushButton *testLabAnalyzeFolderButton = nullptr;
    QPushButton *testLabNewSessionButton = nullptr;
    QPushButton *testLabDeduplicateButton = nullptr;
    QPushButton *testLabReportButton = nullptr;
    QPushButton *testLabCancelButton = nullptr;
    QProcess *testLabProcess = nullptr;
    QString testLabCancelFile;
    QString testLabOutputBuffer;

    // Video Set Assistant (guided UI over the existing file-only CLI workflow)
    QWidget *videoSetPage = nullptr;
    QWidget *videoSetWelcomePage = nullptr;
    QWidget *videoSetRecentPage = nullptr;
    QLabel *videoSetRecentPageTitle = nullptr;
    QLabel *videoSetRecentPageDescription = nullptr;
    QLabel *videoSetIntroLabel = nullptr;
    QLabel *videoSetValidationLabel = nullptr;
    QStackedWidget *videoSetAssistantStack = nullptr;
    QScrollArea *videoSetAssistantScrollArea = nullptr;
    QVector<QLabel *> videoSetAssistantPageHeadings;
    QVector<QLabel *> videoSetAssistantPageSubtitles;
    VidStoreXStepper *videoSetStepIndicator = nullptr;
    QFrame *videoSetWizardActionBar = nullptr;
    QStackedWidget *videoSetWizardActionStack = nullptr;
    QLabel *videoSetPrimaryMessage = nullptr;
    QLabel *videoSetSuggestedAction = nullptr;
    QPushButton *videoSetWelcomeCreateButton = nullptr;
    QPushButton *videoSetWelcomeRecoverButton = nullptr;
    QFrame *videoSetCreateCard = nullptr;
    QFrame *videoSetRecoverCard = nullptr;
    QLabel *videoSetCreateCardTitle = nullptr;
    QLabel *videoSetRecoverCardTitle = nullptr;
    QLabel *videoSetCreateCardDescription = nullptr;
    QLabel *videoSetRecoverCardDescription = nullptr;
    QLabel *videoSetCreateFlowLabel = nullptr;
    QLabel *videoSetRecoverFlowLabel = nullptr;
    QLabel *videoSetTrustLabel = nullptr;
    QToolButton *videoSetTrustDetailsButton = nullptr;
    QFrame *videoSetRecentGroup = nullptr;
    QLabel *videoSetRecentTitle = nullptr;
    QListWidget *videoSetRecentList = nullptr;
    QPushButton *videoSetRecentContinueButton = nullptr;
    QPushButton *videoSetRecentOpenFolderButton = nullptr;
    QPushButton *videoSetRecentRemoveButton = nullptr;
    QFrame *videoSetRecentEmptyState = nullptr;
    QLabel *videoSetRecentEmptyLabel = nullptr;
    QPushButton *videoSetRecentEmptyCreateButton = nullptr;
    QPushButton *videoSetRecentEmptyRecoverButton = nullptr;
    QListWidget *videoSetRecentFullList = nullptr;
    QPushButton *videoSetRecentFullContinueButton = nullptr;
    QPushButton *videoSetRecentFullOpenFolderButton = nullptr;
    QPushButton *videoSetRecentFullRemoveButton = nullptr;
    QFrame *videoSetRecentFullEmptyState = nullptr;
    QLabel *videoSetRecentFullEmptyLabel = nullptr;
    QPushButton *videoSetRecentFullEmptyCreateButton = nullptr;
    QLineEdit *videoSetAssistantInputEdit = nullptr;
    QLineEdit *videoSetAssistantOutputEdit = nullptr;
    QPushButton *videoSetAssistantInputBrowseButton = nullptr;
    QPushButton *videoSetAssistantOutputBrowseButton = nullptr;
    QLineEdit *videoSetInputEdit = nullptr;
    QLineEdit *videoSetOutputEdit = nullptr;
    QLabel *videoSetSourceInfoLabel = nullptr;
    QLabel *videoSetSourceDropLabel = nullptr;
    QPushButton *videoSetSourceContinueButton = nullptr;
    QRadioButton *videoSetResilientRadio = nullptr;
    QRadioButton *videoSetHighCapacityRadio = nullptr;
    QButtonGroup *videoSetProfileCards = nullptr;
    QToolButton *videoSetAdvancedSettingsButton = nullptr;
    QWidget *videoSetAdvancedSettingsWidget = nullptr;
    QPushButton *videoSetModeContinueButton = nullptr;
    QSpinBox *videoSetAssistantTargetSpin = nullptr;
    QSpinBox *videoSetAssistantMaximumSizeSpin = nullptr;
    QDoubleSpinBox *videoSetAssistantReserveSpin = nullptr;
    QComboBox *videoSetProfileCombo = nullptr;
    QSpinBox *videoSetTargetSpin = nullptr;
    QSpinBox *videoSetMaximumSizeSpin = nullptr;
    QDoubleSpinBox *videoSetReserveSpin = nullptr;
    QLabel *videoSetAdvancedProfileLabel = nullptr;
    QGroupBox *videoSetResilientCard = nullptr;
    QGroupBox *videoSetHighCapacityCard = nullptr;
    QLabel *videoSetResilientDescription = nullptr;
    QLabel *videoSetHighCapacityDescription = nullptr;
    QLabel *videoSetPlanSummaryLabel = nullptr;
    QLabel *videoSetPlanMetricsLabel = nullptr;
    QLabel *videoSetPlanSafetyLabel = nullptr;
    QToolButton *videoSetPartDetailsButton = nullptr;
    QTableWidget *videoSetAssistantPlanTable = nullptr;
    QTableWidget *videoSetPlanTable = nullptr;
    QPushButton *videoSetCreateVideosButton = nullptr;
    QProgressBar *videoSetProgress = nullptr;
    QProgressBar *videoSetAssistantProgress = nullptr;
    QLabel *videoSetProgressPhaseLabel = nullptr;
    QLabel *videoSetProgressPartLabel = nullptr;
    QPushButton *videoSetProgressContinueButton = nullptr;
    QPushButton *videoSetProgressOpenFolderButton = nullptr;
    QPushButton *videoSetCreateResultCardButton = nullptr;
    QPushButton *videoSetProgressResumeButton = nullptr;
    QPushButton *videoSetAssistantCancelButton = nullptr;
    QFrame *videoSetActivityPanel = nullptr;
    QLabel *videoSetActivityIcon = nullptr;
    QLabel *videoSetActivityTitle = nullptr;
    QLabel *videoSetActivityDescription = nullptr;
    QLabel *videoSetActivitySourceSummary = nullptr;
    VidStoreXProcessingFlow *videoSetActivityFlow = nullptr;
    VidStoreXPartGrid *videoSetActivityPartGrid = nullptr;
    QLabel *videoSetActivityProgressLabel = nullptr;
    VidStoreXBlockProgress *videoSetActivityProgress = nullptr;
    QLabel *videoSetActivityCounter = nullptr;
    QLabel *videoSetActivityCurrentItem = nullptr;
    QLabel *videoSetActivityElapsed = nullptr;
    QLabel *videoSetActivityRemaining = nullptr;
    QLabel *videoSetActivityWatchdog = nullptr;
    QPushButton *videoSetActivityRetryButton = nullptr;
    QLabel *videoSetRecoveryAvailabilityLabel = nullptr;
    QToolButton *videoSetTechnicalLogButton = nullptr;
    QTextEdit *videoSetLog = nullptr;
    QLabel *videoSetUploadInstructionsLabel = nullptr;
    QVector<QLabel *> videoSetUploadInstructionLabels;
    QPushButton *videoSetOpenVideosButton = nullptr;
    QPushButton *videoSetOpenYouTubeButton = nullptr;
    QPushButton *videoSetOpenChecklistButton = nullptr;
    QPushButton *videoSetUploadedButton = nullptr;
    QPushButton *videoSetYouTubeSyncButton = nullptr;
    QPushButton *videoSetYouTubeSyncPauseButton = nullptr;
    QLabel *videoSetYouTubeSyncStatus = nullptr;
    QProgressBar *videoSetYouTubeSyncProgress = nullptr;
    QLineEdit *videoSetPlaylistUrlEdit = nullptr;
    QPushButton *videoSetDownloadButton = nullptr;
    QPushButton *videoSetSelectYtDlpButton = nullptr;
    QPushButton *videoSetManualReturnedButton = nullptr;
    QLabel *videoSetDownloadStatusLabel = nullptr;
    QProgressBar *videoSetDownloadProgress = nullptr;
    QLineEdit *videoSetRecoveryInputEdit = nullptr;
    QLineEdit *videoSetRecoveryOutputEdit = nullptr;
    QLineEdit *videoSetAssistantRecoveryInputEdit = nullptr;
    QLineEdit *videoSetAssistantRecoveryOutputEdit = nullptr;
    QLabel *videoSetScanSummaryLabel = nullptr;
    QLabel *videoSetScanCountsLabel = nullptr;
    QVector<QLabel *> videoSetScanMetricTitles;
    QVector<QLabel *> videoSetScanMetricValues;
    QListWidget *videoSetDetectedSetsList = nullptr;
    QPushButton *videoSetAssistantScanButton = nullptr;
    QPushButton *videoSetAssistantRecoverButton = nullptr;
    QLineEdit *videoSetInstantPlaylistEdit = nullptr;
    QPushButton *videoSetInstantRecoverButton = nullptr;
    QLabel *videoSetInstantRecoveryStatus = nullptr;
    QPushButton *videoSetOpenReturnedButton = nullptr;
    QLabel *videoSetRecoveryProgressLabel = nullptr;
    QProgressBar *videoSetRecoveryProgressBar = nullptr;
    QLabel *videoSetSuccessLabel = nullptr;
    QLabel *videoSetSuccessIcon = nullptr;
    QLabel *videoSetSuccessDetailsLabel = nullptr;
    VidStoreXSignalRail *videoSetSuccessRail = nullptr;
    QPushButton *videoSetOpenRecoveredButton = nullptr;
    QPushButton *videoSetOpenSetFolderButton = nullptr;
    QPushButton *videoSetCopyShaButton = nullptr;
    QPushButton *videoSetRecoveryResultCardButton = nullptr;
    QPushButton *videoSetReturnHomeButton = nullptr;
    QGroupBox *videoSetClassicToolsGroup = nullptr;
    QPushButton *videoSetPlanButton = nullptr;
    QPushButton *videoSetEncodeButton = nullptr;
    QPushButton *videoSetResumeButton = nullptr;
    QPushButton *videoSetScanButton = nullptr;
    QPushButton *videoSetRecoverButton = nullptr;
    QPushButton *videoSetRecoverResumeButton = nullptr;
    QPushButton *videoSetCancelButton = nullptr;
    QProcess *videoSetProcess = nullptr;
    QProcess *videoSetDownloadProcess = nullptr;
    QTimer *videoSetPlanDebounceTimer = nullptr;
    QTimer *videoSetOperationTimer = nullptr;
    video_set_workflow::Controller videoSetWorkflow;
    video_set_workflow::OperationProgressModel videoSetOperationProgress;
    QString videoSetProcessBuffer;
    QString videoSetProgressLineBuffer;
    QString videoSetActiveCommand;
    QStringList videoSetLastAssistantArguments;
    QString videoSetPlanningInputKey;
    QString videoSetPlannedInputKey;
    QString videoSetCurrentSetRoot;
    QString videoSetCurrentManifest;
    QString videoSetFinalSha;
    QString videoSetRecoveredProfileName;
    QDateTime videoSetCreateCompletedAt;
    QDateTime videoSetRecoveryCompletedAt;
    bool videoSetRecoveryFromYouTube = false;
    bool videoSetAssistantOperation = false;
    bool videoSetCancelRequested = false;
    bool videoSetInstantRecoveryActive = false;
    QString videoSetInstantRecoveryJobRoot;
    int videoSetLastCreatePage = 1;
    int videoSetLastRecoverPage = 7;

    // YouTube Capacity Lab (experimental; never changes production defaults)
    QComboBox *capacityPresetCombo = nullptr;
    QLineEdit *capacityOutputEdit = nullptr;
    QLineEdit *capacityManifestEdit = nullptr;
    QLineEdit *capacitySourceManifestEdit = nullptr;
    QLineEdit *capacityReturnedFolderEdit = nullptr;
    QLineEdit *capacityBlocksEdit = nullptr;
    QLineEdit *capacityBitsEdit = nullptr;
    QLineEdit *capacitySignalsEdit = nullptr;
    QLineEdit *capacityRepairsEdit = nullptr;
    QComboBox *capacityResolutionCombo = nullptr;
    QComboBox *capacitySimulationCombo = nullptr;
    QSpinBox *capacityMaximumCasesSpin = nullptr;
    QDoubleSpinBox *capacityMaximumDiskSpin = nullptr;
    QSpinBox *capacityShortlistSpin = nullptr;
    QCheckBox *capacityEstimateOnlyCheck = nullptr;
    QCheckBox *capacityIncludeSimulationFailuresCheck = nullptr;
    QLabel *capacityEstimateLabel = nullptr;
    QProgressBar *capacityProgress = nullptr;
    QTableWidget *capacityResults = nullptr;
    QPushButton *capacityEstimateButton = nullptr;
    QPushButton *capacityStartButton = nullptr;
    QPushButton *capacityResumeButton = nullptr;
    QPushButton *capacityShortlistButton = nullptr;
    QPushButton *capacityAnalyzeFolderButton = nullptr;
    QPushButton *capacityReportButton = nullptr;
    QPushButton *capacityOpenFolderButton = nullptr;
    QPushButton *capacityCancelButton = nullptr;

    // Worker thread
    std::unique_ptr<WorkerThread> workerThread;
    PreflightEstimateThread *preflightThread = nullptr;
    std::optional<GuiPreflightJob> pendingPreflightJob;
    QTimer *preflightDebounceTimer;
    GuiPreflightModel preflightModel;
    std::optional<ms_encoding_estimate_t> acceptedPreflightEstimate;
    QDateTime acceptedPreflightCompletedAt;

    // State
    bool isOperationRunning;
    bool pendingEncodeAfterPreflight = false;
    bool shuttingDown = false;
    QString currentOperation;
};
