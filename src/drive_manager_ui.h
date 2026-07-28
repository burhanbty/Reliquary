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

#include <memory>

#include "configuration.h"
#include "encoding_reliability.h"

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
                 QObject *parent = nullptr);

signals:
    void progressUpdated(int percentage);

    void statusUpdated(const QString &status);

    void operationCompleted(bool success, const QString &message);

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

    void onReliabilityProfileChanged(int index) const;

    void clearLogs() const;

    void onOperationCompleted(bool success, const QString &message);

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
    QPushButton *encodeButton;
    QPushButton *decodeButton;

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

    // State
    bool isOperationRunning;
    QString currentOperation;
};
