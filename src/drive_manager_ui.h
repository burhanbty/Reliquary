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

#include <memory>
#include <optional>

#include "configuration.h"
#include "encoding_reliability.h"
#include "gui_preflight_model.h"
#include "media_storage.h"

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

    void logReliabilityEstimate(
        const EncodingReliabilityEstimate &estimate,
        const EncodingReliabilityOptions &options) const;

    // UI Components
    QWidget *centralWidget;
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
