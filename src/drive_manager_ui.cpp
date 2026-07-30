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

#include "drive_manager_ui.h"
#include "encoding_reliability.h"
#include "media_storage.h"
#include "video_encoder.h"
#include "youtube_test_lab.h"

#include <QCoreApplication>
#include <QDir>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFile>
#include <QApplication>
#include <QMainWindow>
#include <QMenuBar>
#include <QStatusBar>
#include <QFileDialog>
#include <QMessageBox>
#include <QDir>
#include <QStandardPaths>
#include <QSettings>
#include <QHeaderView>
#include <QFileInfo>
#include <QDateTime>
#include <QDesktopServices>
#include <QFormLayout>
#include <QStyle>
#include <QLocale>
#include <QSignalBlocker>
#include <QSet>
#include <QUrl>

#include <chrono>
#include <cmath>
#include <vector>

WorkerThread::WorkerThread(const Operation op, const QString &input, const QString &output,
                           const bool encrypt, const QString &password,
                           const QString &streamUrl, const int bitrate,
                           const int streamWidth, const int streamHeight,
                           const int streamFps, const double repairRatio,
                           std::optional<ms_encoding_estimate_t> estimate,
                           const bool lowDiskOverride,
                           QObject *parent)
    : QThread(parent), operation(op), inputPath(input), outputPath(output),
      encrypt(encrypt), password(password), streamUrl(streamUrl), bitrate(bitrate),
      streamWidth(streamWidth), streamHeight(streamHeight), streamFps(streamFps),
      repairRatio(repairRatio), preflightEstimate(std::move(estimate)),
      allowLowDisk(lowDiskOverride) {
}

static int gui_encode_progress(const uint64_t current, const uint64_t total, void *user) {
    auto *thread = static_cast<WorkerThread *>(user);
    if (total > 0) {
        const int pct = 5 + static_cast<int>(90 * (current + 1) / total);
        emit thread->progressUpdated(pct);
    }
    return thread->isInterruptionRequested() ? 1 : 0;
}

static int gui_decode_progress(const uint64_t current, const uint64_t total, void *user) {
    auto *thread = static_cast<WorkerThread *>(user);
    if (total > 0) {
        const int pct = 10 + static_cast<int>(70 * current / total);
        emit thread->progressUpdated(pct);
    }
    return thread->isInterruptionRequested() ? 1 : 0;
}

static int gui_stream_encode_progress(const uint64_t current, const uint64_t total, void *user) {
    auto *thread = static_cast<WorkerThread *>(user);
    if (total > 0) {
        const int pct = 5 + static_cast<int>(90 * (current + 1) / total);
        emit thread->progressUpdated(pct);
    }
    return thread->isInterruptionRequested() ? 1 : 0;
}

static int gui_stream_decode_progress(const uint64_t current, const uint64_t total, void *user) {
    auto *thread = static_cast<WorkerThread *>(user);
    if (total > 0) {
        const int pct = 10 + static_cast<int>(70 * current / total);
        emit thread->progressUpdated(pct);
    } else if (current > 0) {
        emit thread->progressUpdated(std::min(static_cast<int>(current % 90) + 10, 95));
    }
    return thread->isInterruptionRequested() ? 1 : 0;
}

static void emit_performance_report(WorkerThread *thread,
                                    const ms_result_t &result) {
    const size_t required = ms_format_performance_report(
        &result, nullptr, 0);
    std::vector<char> report(required);
    ms_format_performance_report(
        &result, report.data(), report.size());
    emit thread->logMessage(
        QString::fromUtf8(report.data()).trimmed());
}

static QString format_bytes(const uint64_t bytes) {
    static constexpr const char *units[] = {
        "B", "KiB", "MiB", "GiB", "TiB"};
    double value = static_cast<double>(bytes);
    std::size_t unit = 0;
    while (value >= 1024.0 && unit < 4) {
        value /= 1024.0;
        ++unit;
    }
    const int decimals = unit == 0 ? 0 : 2;
    return QString("%1 %2")
        .arg(QLocale().toString(value, 'f', decimals),
             QString::fromLatin1(units[unit]));
}

static QString format_count(const uint64_t value) {
    return QLocale().toString(static_cast<qulonglong>(value));
}

void WorkerThread::run() {
    const std::string input = inputPath.toStdString();
    const std::string output = outputPath.toStdString();
    const std::string pw = password.toStdString();

    if (operation == Encode) {
        emit statusUpdated("Starting encoding process...");
        emit logMessage("Encoding: " + inputPath + " -> " + outputPath);
        if (encrypt) {
            emit logMessage("Encrypting chunks with password");
        }
        emit progressUpdated(5);

        ms_encode_options_t opts{};
        opts.input_path = input.c_str();
        opts.output_path = output.c_str();
        opts.encrypt = encrypt ? 1 : 0;
        opts.password = pw.c_str();
        opts.password_len = pw.size();
        opts.hash_algorithm = MS_HASH_CRC32;
        opts.progress = gui_encode_progress;
        opts.progress_user = this;
        opts.repair_ratio = repairRatio;
        opts.repair_ratio_is_set = 1;
        opts.preflight_estimate = preflightEstimate
                                      ? &*preflightEstimate
                                      : nullptr;
        opts.preflight_duration_seconds =
            preflightEstimate
                ? preflightEstimate->preflight_duration_seconds
                : 0.0;
        opts.allow_low_disk = allowLowDisk ? 1 : 0;
        opts.encoding_mode = preflightEstimate
            ? preflightEstimate->encoding_mode
            : MS_ENCODING_MODE_RESILIENT;

        ms_result_t result{};

        if (const ms_status_t status = ms_encode(&opts, &result); status == MS_OK) {
            emit logMessage(QString("Input size: %1 bytes").arg(result.input_size));
            emit logMessage(QString("Chunks: %1").arg(result.total_chunks));
            emit logMessage(QString("Generated %1 packets in %2 frames")
                .arg(result.total_packets).arg(result.total_frames));
            emit_performance_report(this, result);
            emit progressUpdated(100);
            emit operationCompleted(
                true, "Encoding completed successfully", MS_OK);
        } else {
            emit operationCompleted(
                false, QString("Error: %1").arg(ms_status_string(status)),
                status);
        }
    } else if (operation == Decode) {
        emit statusUpdated("Starting decoding process...");
        emit logMessage("Decoding: " + inputPath + " -> " + outputPath);
        emit progressUpdated(10);

        ms_decode_options_t opts{};
        opts.input_path = input.c_str();
        opts.output_path = output.c_str();
        opts.password = pw.c_str();
        opts.password_len = pw.size();
        opts.progress = gui_decode_progress;
        opts.progress_user = this;

        ms_result_t result{};

        if (const ms_status_t status = ms_decode(&opts, &result); status == MS_OK) {
            emit logMessage(QString("Video size: %1 bytes").arg(result.input_size));
            emit logMessage(QString("Packets extracted: %1").arg(result.total_packets));
            emit logMessage(QString("Chunks decoded: %1").arg(result.total_chunks));
            emit logMessage(QString("Frames: %1").arg(result.total_frames));
            emit_performance_report(this, result);
            emit progressUpdated(100);
            emit operationCompleted(
                true, "Decoding completed successfully", MS_OK);
        } else {
            emit operationCompleted(
                false, QString("Error: %1").arg(ms_status_string(status)),
                status);
        }
    } else if (operation == StreamEncode) {
        const std::string url = streamUrl.toStdString();
        emit statusUpdated("Starting stream encode...");
        emit logMessage("Stream encode: " + inputPath + " -> " + streamUrl);
        emit logMessage(QString("Resolution: %1x%2 (%3 fps)").arg(streamWidth).arg(streamHeight).arg(streamFps));
        emit logMessage(QString("Bitrate: %1 kbps").arg(bitrate));
        if (encrypt) {
            emit logMessage("Encrypting chunks with password");
        }
        emit progressUpdated(5);

        ms_stream_encode_options_t opts{};
        opts.input_path = input.c_str();
        opts.stream_url = url.c_str();
        opts.encrypt = encrypt ? 1 : 0;
        opts.password = pw.c_str();
        opts.password_len = pw.size();
        opts.hash_algorithm = MS_HASH_CRC32;
        opts.bitrate_kbps = bitrate;
        opts.width = streamWidth;
        opts.height = streamHeight;
        opts.fps = streamFps;
        opts.progress = gui_stream_encode_progress;
        opts.progress_user = this;
        opts.repair_ratio = repairRatio;
        opts.repair_ratio_is_set = 1;

        ms_result_t result{};

        if (const ms_status_t status = ms_stream_encode(&opts, &result); status == MS_OK) {
            emit logMessage(QString("Input size: %1 bytes").arg(result.input_size));
            emit logMessage(QString("Chunks: %1").arg(result.total_chunks));
            emit logMessage(QString("Streamed %1 packets in %2 frames")
                .arg(result.total_packets).arg(result.total_frames));
            emit_performance_report(this, result);
            emit progressUpdated(100);
            emit operationCompleted(
                true, "Stream encode completed successfully", MS_OK);
        } else {
            emit operationCompleted(
                false,
                QString("Stream encode error: %1")
                    .arg(ms_status_string(status)),
                status);
        }
    } else if (operation == StreamDecode) {
        const std::string url = streamUrl.toStdString();
        emit statusUpdated("Waiting for stream...");
        emit logMessage("Stream decode: " + streamUrl + " -> " + outputPath);
        emit progressUpdated(5);

        ms_stream_decode_options_t opts{};
        opts.stream_url = url.c_str();
        opts.output_path = output.c_str();
        opts.password = pw.c_str();
        opts.password_len = pw.size();
        opts.timeout_sec = 30;
        opts.progress = gui_stream_decode_progress;
        opts.progress_user = this;

        ms_result_t result{};

        if (const ms_status_t status = ms_stream_decode(&opts, &result); status == MS_OK) {
            emit logMessage(QString("Packets extracted: %1").arg(result.total_packets));
            emit logMessage(QString("Chunks decoded: %1").arg(result.total_chunks));
            emit logMessage(QString("Frames: %1").arg(result.total_frames));
            emit logMessage(QString("Output size: %1 bytes").arg(result.output_size));
            emit_performance_report(this, result);
            emit progressUpdated(100);
            emit operationCompleted(
                true, "Stream decode completed successfully", MS_OK);
        } else {
            emit operationCompleted(
                false,
                QString("Stream decode error: %1")
                    .arg(ms_status_string(status)),
                status);
        }
    }
}

PreflightEstimateThread::PreflightEstimateThread(
    GuiPreflightJob job, QObject *parent)
    : QThread(parent), job_(std::move(job)) {
}

void PreflightEstimateThread::run() {
    emit phaseChanged(job_.generation, "Preparing estimate");
    if (isInterruptionRequested()) return;

    const std::string input = job_.inputPath.toStdString();
    const std::string output = job_.outputPath.toStdString();
    const std::string password = job_.password.toStdString();
    ms_encode_options_t options{};
    options.input_path = input.c_str();
    options.output_path = output.c_str();
    options.encrypt = job_.encrypted ? 1 : 0;
    options.password = password.c_str();
    options.password_len = password.size();
    options.hash_algorithm = MS_HASH_CRC32;
    options.repair_ratio = job_.repairRatio;
    options.repair_ratio_is_set = 1;
    options.encoding_mode = job_.encodingMode;

    emit phaseChanged(
        job_.generation, "Calculating packets and frames");
    emit phaseChanged(job_.generation, "Encoding probe");
    result_status_ = ms_estimate_encode(&options, 1, &estimate_);
    if (isInterruptionRequested()) return;
    emit phaseChanged(job_.generation, "Checking disk space");
    emit phaseChanged(job_.generation, "Finalizing estimate");
}

DriveManagerUI::DriveManagerUI(QWidget *parent)
    : QMainWindow(parent), isOperationRunning(false) {
    setWindowTitle("YouTube Media Storage - Drive Manager");
    setMinimumSize(1200, 800);

    loadSettings();
    setupUI();
    setupMenuBar();
    setupStatusBar();
    connectSignals();

    preflightDebounceTimer = new QTimer(this);
    preflightDebounceTimer->setSingleShot(true);
    preflightDebounceTimer->setInterval(400);
    connect(preflightDebounceTimer, &QTimer::timeout,
            this, &DriveManagerUI::runDebouncedPreflight);

    resetProgress();
    clearPreflightValues();
    updatePreflightPanel();
    logMessage("Drive Manager initialized");
}

DriveManagerUI::~DriveManagerUI() {
    shuttingDown = true;
    pendingPreflightJob.reset();
    preflightModel.beginShutdown();
    if (preflightThread && preflightThread->isRunning()) {
        preflightThread->requestInterruption();
        preflightThread->wait();
    }
    if (workerThread && workerThread->isRunning()) {
        workerThread->requestInterruption();
        workerThread->wait();
    }
    if (testLabProcess &&
        testLabProcess->state() != QProcess::NotRunning) {
        QFile cancel(testLabCancelFile);
        if (cancel.open(QIODevice::WriteOnly)) cancel.close();
        if (!testLabProcess->waitForFinished(10000))
            testLabProcess->kill();
    }
    saveSettings();
}

void DriveManagerUI::setupUI() {
    centralWidget = new QWidget(this);
    setCentralWidget(centralWidget);

    mainSplitter = new QSplitter(Qt::Horizontal, centralWidget);

    // Left panel
    auto *leftPanel = new QWidget();
    auto *leftLayout = new QVBoxLayout(leftPanel);

    // File operations group
    fileOperationsGroup = new QGroupBox("File Operations");
    auto *fileOpsLayout = new QGridLayout(fileOperationsGroup);

    fileOpsLayout->addWidget(new QLabel("Input File:"), 0, 0);
    inputFileEdit = new QLineEdit();
    inputFileEdit->setObjectName("inputFileEdit");
    inputFileEdit->setReadOnly(true);
    fileOpsLayout->addWidget(inputFileEdit, 0, 1);

    selectInputButton = new QPushButton("Browse...");
    fileOpsLayout->addWidget(selectInputButton, 0, 2);

    fileOpsLayout->addWidget(new QLabel("Output File:"), 1, 0);
    outputFileEdit = new QLineEdit();
    outputFileEdit->setObjectName("outputFileEdit");
    outputFileEdit->setReadOnly(true);
    fileOpsLayout->addWidget(outputFileEdit, 1, 1);

    selectOutputButton = new QPushButton("Browse...");
    fileOpsLayout->addWidget(selectOutputButton, 1, 2);

    encryptCheckBox = new QCheckBox("Encrypt with password");
    fileOpsLayout->addWidget(encryptCheckBox, 2, 0, 1, 3);

    fileOpsLayout->addWidget(new QLabel("Password:"), 3, 0);
    passwordEdit = new QLineEdit();
    passwordEdit->setPlaceholderText("For encrypt or decrypt");
    passwordEdit->setEchoMode(QLineEdit::Password);
    fileOpsLayout->addWidget(passwordEdit, 3, 1);
    passwordVisibilityButton = new QPushButton("Show");
    passwordVisibilityButton->setFixedWidth(selectInputButton->sizeHint().width());
    fileOpsLayout->addWidget(passwordVisibilityButton, 3, 2);

    fileOpsLayout->addWidget(new QLabel("Encoding mode:"), 4, 0);
    encodingModeCombo = new QComboBox();
    encodingModeCombo->setObjectName("encodingModeCombo");
    encodingModeCombo->addItem(
        "Resilient / Platform", MS_ENCODING_MODE_RESILIENT);
    encodingModeCombo->addItem(
        "Fast Local", MS_ENCODING_MODE_FAST_LOCAL);
    fileOpsLayout->addWidget(encodingModeCombo, 4, 1, 1, 2);

    encodingModeHelpLabel = new QLabel(
        "Resilient / Platform tolerates re-encoding and supports FEC. "
        "Fast Local is much smaller and faster, but is only for lossless "
        "local storage; lossy uploads or re-encoding may destroy the data.");
    encodingModeHelpLabel->setWordWrap(true);
    encodingModeHelpLabel->setStyleSheet(
        "color: palette(mid); font-size: 9pt;");
    fileOpsLayout->addWidget(encodingModeHelpLabel, 5, 0, 1, 3);

    fileOpsLayout->addWidget(new QLabel("Reliability:"), 6, 0);
    reliabilityProfileCombo = new QComboBox();
    reliabilityProfileCombo->addItem("Local / Fast (5%)", 5.0);
    reliabilityProfileCombo->addItem("Balanced (20%)", 20.0);
    reliabilityProfileCombo->addItem("Durable (50%)", 50.0);
    reliabilityProfileCombo->addItem("Custom", -1.0);
    fileOpsLayout->addWidget(reliabilityProfileCombo, 6, 1);

    repairPercentSpinBox = new QDoubleSpinBox();
    repairPercentSpinBox->setObjectName("repairPercentSpinBox");
    repairPercentSpinBox->setRange(0.0, MAX_REPAIR_PERCENTAGE);
    repairPercentSpinBox->setDecimals(2);
    repairPercentSpinBox->setSingleStep(0.5);
    repairPercentSpinBox->setValue(DEFAULT_REPAIR_PERCENTAGE);
    repairPercentSpinBox->setSuffix("%");
    repairPercentSpinBox->setEnabled(false);
    fileOpsLayout->addWidget(repairPercentSpinBox, 6, 2);

    reliabilityHelpLabel = new QLabel(
        "Higher repair improves damage tolerance, but increases frames, time, and output size.");
    reliabilityHelpLabel->setWordWrap(true);
    reliabilityHelpLabel->setStyleSheet("color: palette(mid); font-size: 9pt;");
    fileOpsLayout->addWidget(reliabilityHelpLabel, 7, 0, 1, 3);

    encodeButton = new QPushButton("Encode to Video");
    encodeButton->setObjectName("encodeButton");
    encodeButton->setIcon(QIcon::fromTheme("media-record"));
    fileOpsLayout->addWidget(encodeButton, 8, 0, 1, 3);

    decodeButton = new QPushButton("Decode from Video");
    decodeButton->setIcon(QIcon::fromTheme("media-playback-start"));
    fileOpsLayout->addWidget(decodeButton, 9, 0, 1, 3);

    leftLayout->addWidget(fileOperationsGroup);

    // Batch operations group
    batchGroup = new QGroupBox("Batch Operations");
    auto *batchLayout = new QVBoxLayout(batchGroup);

    fileListWidget = new QListWidget();
    fileListWidget->setSelectionMode(QAbstractItemView::ExtendedSelection);
    batchLayout->addWidget(fileListWidget);

    auto *batchButtonsLayout = new QHBoxLayout();
    addFilesButton = new QPushButton("Add Files");
    removeFilesButton = new QPushButton("Remove Selected");
    clearFilesButton = new QPushButton("Clear All");
    batchButtonsLayout->addWidget(addFilesButton);
    batchButtonsLayout->addWidget(removeFilesButton);
    batchButtonsLayout->addWidget(clearFilesButton);
    batchLayout->addLayout(batchButtonsLayout);

    auto *batchOutputLayout = new QHBoxLayout();
    batchOutputLayout->addWidget(new QLabel("Output Directory:"));
    batchOutputDirEdit = new QLineEdit();
    batchOutputDirEdit->setReadOnly(true);
    batchOutputButton = new QPushButton("Browse...");
    batchOutputLayout->addWidget(batchOutputDirEdit);
    batchOutputLayout->addWidget(batchOutputButton);
    batchLayout->addLayout(batchOutputLayout);

    batchEncodeButton = new QPushButton("Batch Encode All");
    batchEncodeButton->setIcon(QIcon::fromTheme("document-save-all"));
    batchLayout->addWidget(batchEncodeButton);

    leftLayout->addWidget(batchGroup);

    // Streaming group
    streamGroup = new QGroupBox("Streaming (Twitch / YouTube)");
    auto *streamLayout = new QGridLayout(streamGroup);

    streamLayout->addWidget(new QLabel("Platform:"), 0, 0);
    platformCombo = new QComboBox();
    platformCombo->addItem("Twitch", "rtmp://live.twitch.tv/app/");
    platformCombo->addItem("YouTube", "rtmp://a.rtmp.youtube.com/live2/");
    platformCombo->addItem("Custom", "");
    streamLayout->addWidget(platformCombo, 0, 1, 1, 2);

    streamLayout->addWidget(new QLabel("RTMP URL:"), 1, 0);
    streamUrlEdit = new QLineEdit();
    streamUrlEdit->setPlaceholderText("rtmp://live.twitch.tv/app/");
    streamUrlEdit->setText("rtmp://live.twitch.tv/app/");
    streamLayout->addWidget(streamUrlEdit, 1, 1, 1, 2);

    streamLayout->addWidget(new QLabel("Stream Key:"), 2, 0);
    streamKeyEdit = new QLineEdit();
    streamKeyEdit->setPlaceholderText("Your stream key");
    streamKeyEdit->setEchoMode(QLineEdit::Password);
    streamLayout->addWidget(streamKeyEdit, 2, 1, 1, 2);

    streamLayout->addWidget(new QLabel("Resolution:"), 3, 0);
    resolutionCombo = new QComboBox();
    resolutionCombo->addItem("1080p (1920x1080)", QSize(1920, 1080));
    resolutionCombo->addItem("1440p (2560x1440)", QSize(2560, 1440));
    resolutionCombo->addItem("4K (3840x2160)", QSize(3840, 2160));
    resolutionCombo->setCurrentIndex(0);
    streamLayout->addWidget(resolutionCombo, 3, 1, 1, 2);

    streamLayout->addWidget(new QLabel("Bitrate (kbps):"), 4, 0);
    bitrateSpinBox = new QSpinBox();
    bitrateSpinBox->setRange(1000, 50000);
    bitrateSpinBox->setValue(8000);
    bitrateSpinBox->setSingleStep(1000);
    bitrateSpinBox->setSuffix(" kbps");
    streamLayout->addWidget(bitrateSpinBox, 4, 1, 1, 2);

    streamLayout->addWidget(new QLabel("FPS:"), 5, 0);
    fpsSpinBox = new QSpinBox();
    fpsSpinBox->setRange(1, 240);
    fpsSpinBox->setValue(FRAME_FPS);
    fpsSpinBox->setSingleStep(1);
    fpsSpinBox->setSuffix(" fps");
    streamLayout->addWidget(fpsSpinBox, 5, 1, 1, 2);

    streamEncodeButton = new QPushButton("Stream Encode");
    streamEncodeButton->setIcon(QIcon::fromTheme("network-transmit"));
    streamLayout->addWidget(streamEncodeButton, 6, 0, 1, 3);

    streamDecodeButton = new QPushButton("Stream Decode");
    streamDecodeButton->setIcon(QIcon::fromTheme("network-receive"));
    streamLayout->addWidget(streamDecodeButton, 7, 0, 1, 3);

    leftLayout->addWidget(streamGroup);

    // Right panel
    auto *rightPanel = new QWidget();
    auto *rightLayout = new QVBoxLayout(rightPanel);

    // Status group
    statusGroup = new QGroupBox("Status");
    auto *statusLayout = new QVBoxLayout(statusGroup);

    progressBar = new QProgressBar();
    progressBar->setRange(0, 100);
    statusLayout->addWidget(progressBar);

    progressLabel = new QLabel("Ready");
    statusLayout->addWidget(progressLabel);

    statusLabel = new QLabel("Status: Idle");
    statusLayout->addWidget(statusLabel);

    rightLayout->addWidget(statusGroup);

    // Preflight estimate group
    preflightGroup = new QGroupBox("Preflight Estimate");
    preflightGroup->setObjectName("preflightEstimateGroup");
    auto *preflightLayout = new QVBoxLayout(preflightGroup);
    auto *preflightStatusLayout = new QHBoxLayout();
    preflightStatusIcon = new QLabel();
    preflightStatusIcon->setFixedSize(20, 20);
    preflightStatusValue = new QLabel("Waiting for input");
    preflightStatusValue->setObjectName("preflightStatusValue");
    QFont statusFont = preflightStatusValue->font();
    statusFont.setBold(true);
    preflightStatusValue->setFont(statusFont);
    preflightStatusLayout->addWidget(preflightStatusIcon);
    preflightStatusLayout->addWidget(preflightStatusValue, 1);
    preflightLayout->addLayout(preflightStatusLayout);

    preflightProgress = new QProgressBar();
    preflightProgress->setTextVisible(false);
    preflightProgress->setMaximumHeight(
        preflightProgress->sizeHint().height());
    preflightProgress->hide();
    preflightLayout->addWidget(preflightProgress);

    auto *summaryForm = new QFormLayout();
    summaryForm->setFieldGrowthPolicy(
        QFormLayout::AllNonFixedFieldsGrow);
    preflightInputSizeValue = new QLabel("-");
    preflightReliabilityValue = new QLabel("-");
    preflightRepairValue = new QLabel("-");
    preflightLikelyOutputValue = new QLabel("-");
    preflightRangeValue = new QLabel("-");
    preflightAvailableDiskValue = new QLabel("-");
    preflightRequiredDiskValue = new QLabel("-");
    preflightMissingDiskValue = new QLabel("-");
    summaryForm->addRow("Input size:", preflightInputSizeValue);
    summaryForm->addRow("Reliability profile:",
                        preflightReliabilityValue);
    summaryForm->addRow("Repair percentage:",
                        preflightRepairValue);
    summaryForm->addRow("Estimated likely output:",
                        preflightLikelyOutputValue);
    summaryForm->addRow("Expected output range:",
                        preflightRangeValue);
    summaryForm->addRow("Available disk space:",
                        preflightAvailableDiskValue);
    summaryForm->addRow("Required disk space:",
                        preflightRequiredDiskValue);
    summaryForm->addRow("Missing space:",
                        preflightMissingDiskValue);
    preflightLayout->addLayout(summaryForm);

    preflightDetailsButton = new QToolButton();
    preflightDetailsButton->setText("Show details");
    preflightDetailsButton->setCheckable(true);
    preflightDetailsButton->setArrowType(Qt::RightArrow);
    preflightDetailsButton->setToolButtonStyle(
        Qt::ToolButtonTextBesideIcon);
    preflightLayout->addWidget(preflightDetailsButton);

    preflightDetailsWidget = new QWidget();
    auto *detailsForm = new QFormLayout(preflightDetailsWidget);
    detailsForm->setContentsMargins(0, 0, 0, 0);
    preflightSourcePacketsValue = new QLabel("-");
    preflightRepairPacketsValue = new QLabel("-");
    preflightTotalPacketsValue = new QLabel("-");
    preflightFramesValue = new QLabel("-");
    preflightVideoDurationValue = new QLabel("-");
    preflightSafetyMarginValue = new QLabel("-");
    preflightProbeFramesValue = new QLabel("-");
    preflightProbeDurationValue = new QLabel("-");
    preflightMethodValue = new QLabel("-");
    preflightHeaderValue = new QLabel("-");
    preflightFrameCapacityValue = new QLabel("-");
    preflightMethodValue->setWordWrap(true);
    detailsForm->addRow("Source packets:",
                        preflightSourcePacketsValue);
    detailsForm->addRow("Repair packets:",
                        preflightRepairPacketsValue);
    detailsForm->addRow("Total packets:",
                        preflightTotalPacketsValue);
    detailsForm->addRow("Estimated frames:",
                        preflightFramesValue);
    detailsForm->addRow("Estimated video duration:",
                        preflightVideoDurationValue);
    detailsForm->addRow("Safety margin:",
                        preflightSafetyMarginValue);
    detailsForm->addRow("Probe frame count:",
                        preflightProbeFramesValue);
    detailsForm->addRow("Probe duration:",
                        preflightProbeDurationValue);
    detailsForm->addRow("Estimation method:",
                        preflightMethodValue);
    detailsForm->addRow("Header bytes:",
                        preflightHeaderValue);
    detailsForm->addRow("Frame payload capacity:",
                        preflightFrameCapacityValue);
    preflightDetailsWidget->hide();
    preflightLayout->addWidget(preflightDetailsWidget);

    lowDiskOverrideCheckBox =
        new QCheckBox("Proceed despite insufficient disk space");
    lowDiskOverrideCheckBox->setObjectName(
        "lowDiskOverrideCheckBox");
    lowDiskOverrideCheckBox->hide();
    preflightLayout->addWidget(lowDiskOverrideCheckBox);
    rightLayout->addWidget(preflightGroup);

    // Logs group
    logsGroup = new QGroupBox("Logs");
    auto *logsLayout = new QVBoxLayout(logsGroup);

    logTextEdit = new QTextEdit();
    logTextEdit->setReadOnly(true);
    // logTextEdit->setMaximumBlockCount(1000); // Commented out - not available in Qt6
    logsLayout->addWidget(logTextEdit);

    clearLogsButton = new QPushButton("Clear Logs");
    logsLayout->addWidget(clearLogsButton);

    rightLayout->addWidget(logsGroup);

    // Add panels to splitter
    auto *leftScrollArea = new QScrollArea();
    leftScrollArea->setWidgetResizable(true);
    leftScrollArea->setFrameShape(QFrame::NoFrame);
    leftScrollArea->setWidget(leftPanel);
    mainSplitter->addWidget(leftScrollArea);
    mainSplitter->addWidget(rightPanel);
    mainSplitter->setSizes({600, 600});

    // Main application tabs
    mainTabs = new QTabWidget(centralWidget);
    mainTabs->addTab(mainSplitter, "Storage");

    auto *testLabPage = new QWidget();
    auto *testLabLayout = new QVBoxLayout(testLabPage);
    auto *testLabNotice = new QLabel(
        "<b>YouTube Test Lab</b><br>"
        "Creates Resilient test videos for a manual Private/Unlisted "
        "YouTube roundtrip. It never signs in, uploads, or downloads. "
        "Upload candidates contain at least 60 real data frames and "
        "2.0 seconds at 30 FPS; no blank or repeated filler frames are "
        "used. Small requested payloads are extended deterministically. "
        "Local simulation is fast feedback only and is <b>not</b> a "
        "guaranteed copy of YouTube processing.");
    testLabNotice->setWordWrap(true);
    testLabLayout->addWidget(testLabNotice);

    auto *testLabGenerateGroup =
        new QGroupBox("A. Generate Test Suite");
    auto *testLabGenerateLayout =
        new QGridLayout(testLabGenerateGroup);
    testLabPresetCombo = new QComboBox();
    testLabPresetCombo->addItems({"Quick Test", "Full Matrix"});
    testLabOutputEdit = new QLineEdit();
    testLabOutputEdit->setPlaceholderText(
        "Suite parent output directory");
    auto *testLabOutputBrowse = new QPushButton("Browse...");
    testLabEstimateLabel = new QLabel(
        "Quick: 6 cases (requested 64 KiB random, 5/20/50%, "
        "1080p + 4K). Effective payload and disk estimates include "
        "the 2-second / 60-frame minimum.");
    testLabEstimateLabel->setWordWrap(true);
    testLabGenerateButton = new QPushButton("Generate");
    testLabResumeButton = new QPushButton("Resume Incomplete");
    testLabGenerateLayout->addWidget(new QLabel("Preset:"), 0, 0);
    testLabGenerateLayout->addWidget(testLabPresetCombo, 0, 1, 1, 2);
    testLabGenerateLayout->addWidget(new QLabel("Output:"), 1, 0);
    testLabGenerateLayout->addWidget(testLabOutputEdit, 1, 1);
    testLabGenerateLayout->addWidget(testLabOutputBrowse, 1, 2);
    testLabGenerateLayout->addWidget(testLabEstimateLabel, 2, 0, 1, 3);
    testLabGenerateLayout->addWidget(testLabGenerateButton, 3, 1);
    testLabGenerateLayout->addWidget(testLabResumeButton, 3, 2);
    testLabLayout->addWidget(testLabGenerateGroup);

    auto *testLabRoundtripGroup =
        new QGroupBox("B. Local Simulation / C. Real YouTube Analysis");
    auto *testLabRoundtripLayout =
        new QGridLayout(testLabRoundtripGroup);
    testLabManifestEdit = new QLineEdit();
    testLabManifestEdit->setPlaceholderText("manifest.json");
    auto *testLabManifestBrowse = new QPushButton("Manifest...");
    testLabSimulationCombo = new QComboBox();
    for (const auto &profile :
         youtube_test_lab::simulation_profiles())
        testLabSimulationCombo->addItem(
            QString::fromStdString(profile.name));
    testLabVideoEdit = new QLineEdit();
    testLabVideoEdit->setPlaceholderText(
        "Downloaded YouTube video");
    auto *testLabVideoBrowse = new QPushButton("Video...");
    testLabCaseEdit = new QLineEdit();
    testLabCaseEdit->setPlaceholderText(
        "Optional case ID; filename auto-detection is attempted");
    testLabFolderEdit = new QLineEdit();
    testLabFolderEdit->setPlaceholderText(
        "Folder containing downloaded YouTube videos");
    auto *testLabFolderBrowse = new QPushButton("Folder...");
    testLabMappingsEdit = new QLineEdit();
    testLabMappingsEdit->setPlaceholderText(
        "Optional corrections: filename.webm=yt001;other.mp4=yt002");
    testLabSessionLabelEdit = new QLineEdit();
    testLabSessionLabelEdit->setPlaceholderText(
        "Initial upload, 24-hour retest, 7-day retest...");
    testLabActiveSessionLabel = new QLabel("Active analysis session: -");
    testLabNewSessionButton = new QPushButton("New Session");
    testLabRecordNewCheck =
        new QCheckBox("Record as new timed observation");
    testLabDuplicateWarning = new QLabel();
    testLabDuplicateWarning->setWordWrap(true);
    testLabDuplicateWarning->setStyleSheet(
        "color: #b06000; font-weight: bold;");
    testLabSimulateButton = new QPushButton("Run Local Simulation");
    testLabAnalyzeButton = new QPushButton("Analyze Single Video");
    testLabPreviewFolderButton = new QPushButton("Preview Folder Mapping");
    testLabAnalyzeFolderButton = new QPushButton("Analyze Folder");
    testLabDeduplicateButton = new QPushButton("Deduplicate Results");
    testLabReportButton = new QPushButton("Refresh Reports");
    testLabCancelButton = new QPushButton("Cancel");
    testLabCancelButton->setEnabled(false);
    testLabProgress = new QProgressBar();
    testLabProgress->setRange(0, 100);
    testLabRoundtripLayout->addWidget(new QLabel("Manifest:"), 0, 0);
    testLabRoundtripLayout->addWidget(testLabManifestEdit, 0, 1);
    testLabRoundtripLayout->addWidget(testLabManifestBrowse, 0, 2);
    auto *simulationOnlyLabel = new QLabel(
        "Simulation profile (used only by Run Local Simulation):");
    simulationOnlyLabel->setWordWrap(true);
    testLabRoundtripLayout->addWidget(simulationOnlyLabel, 1, 0);
    testLabRoundtripLayout->addWidget(testLabSimulationCombo, 1, 1, 1, 2);
    testLabRoundtripLayout->addWidget(testLabSimulateButton, 2, 1, 1, 2);
    testLabRoundtripLayout->addWidget(
        new QLabel("<b>Real YouTube analysis</b>"), 3, 0, 1, 3);
    testLabRoundtripLayout->addWidget(new QLabel("Returned video:"), 4, 0);
    testLabRoundtripLayout->addWidget(testLabVideoEdit, 4, 1);
    testLabRoundtripLayout->addWidget(testLabVideoBrowse, 4, 2);
    testLabRoundtripLayout->addWidget(new QLabel("Case:"), 5, 0);
    testLabRoundtripLayout->addWidget(testLabCaseEdit, 5, 1, 1, 2);
    testLabRoundtripLayout->addWidget(new QLabel("Folder:"), 6, 0);
    testLabRoundtripLayout->addWidget(testLabFolderEdit, 6, 1);
    testLabRoundtripLayout->addWidget(testLabFolderBrowse, 6, 2);
    testLabRoundtripLayout->addWidget(new QLabel("Mapping:"), 7, 0);
    testLabRoundtripLayout->addWidget(testLabMappingsEdit, 7, 1, 1, 2);
    testLabRoundtripLayout->addWidget(new QLabel("Session label:"), 8, 0);
    testLabRoundtripLayout->addWidget(testLabSessionLabelEdit, 8, 1);
    testLabRoundtripLayout->addWidget(testLabNewSessionButton, 8, 2);
    testLabRoundtripLayout->addWidget(testLabActiveSessionLabel, 9, 0, 1, 2);
    testLabRoundtripLayout->addWidget(testLabRecordNewCheck, 9, 2);
    testLabRoundtripLayout->addWidget(testLabAnalyzeButton, 10, 0);
    testLabRoundtripLayout->addWidget(testLabPreviewFolderButton, 10, 1);
    testLabRoundtripLayout->addWidget(testLabAnalyzeFolderButton, 10, 2);
    testLabRoundtripLayout->addWidget(testLabDuplicateWarning, 11, 0, 1, 3);
    testLabRoundtripLayout->addWidget(testLabDeduplicateButton, 12, 0);
    testLabRoundtripLayout->addWidget(testLabReportButton, 12, 1);
    testLabRoundtripLayout->addWidget(testLabCancelButton, 12, 2);
    testLabRoundtripLayout->addWidget(testLabProgress, 13, 0, 1, 3);
    testLabLayout->addWidget(testLabRoundtripGroup);

    testLabBatchPreview = new QTableWidget(0, 8);
    testLabBatchPreview->setHorizontalHeaderLabels({
        "Filename", "Detected case", "Resolution", "Codec",
        "Size", "Status", "Duplicate", "User mapping"});
    testLabBatchPreview->horizontalHeader()->setSectionResizeMode(
        QHeaderView::ResizeToContents);
    testLabBatchPreview->horizontalHeader()->setStretchLastSection(true);
    testLabLayout->addWidget(new QLabel("<b>Batch mapping preview</b>"));
    testLabLayout->addWidget(testLabBatchPreview);

    auto *testLabInstructions = new QLabel(
        "<b>Manual YouTube steps:</b> upload only candidates marked "
        "<i>Ready for YouTube - 60+ frames, 2.0+ seconds, local "
        "SHA-256 passed</i>, choose Private or Unlisted, wait for processing, download "
        "your own video, then import it here. Fast Local is not designed "
        "for lossy YouTube processing.");
    testLabInstructions->setWordWrap(true);
    testLabLayout->addWidget(testLabInstructions);

    testLabResults = new QTableWidget(0, 15);
    testLabResults->setHorizontalHeaderLabels({
        "Case", "Resolution", "Reliability", "Requested",
        "Effective", "Minimum", "Expected", "Candidate frames",
        "Candidate duration", "YouTube ready", "Validation",
        "Upload", "Returned", "Packets", "SHA-256 / Status"});
    testLabResults->horizontalHeader()->setSectionResizeMode(
        QHeaderView::ResizeToContents);
    testLabResults->horizontalHeader()->setStretchLastSection(true);
    testLabLayout->addWidget(testLabResults, 1);
    mainTabs->addTab(testLabPage, "YouTube Test Lab");

    auto *capacityPage = new QWidget();
    auto *capacityLayout = new QVBoxLayout(capacityPage);
    auto *capacityNotice = new QLabel(
        "<b>YouTube Capacity Lab (experimental)</b><br>"
        "Searches 4x4/6x6/8x8 geometry, 1/2-bit modulation, signal "
        "strength and repair levels behind local lossless and H.264 "
        "gates. These controls never change the production Resilient "
        "profile. Shortlisted videos still require a manual real "
        "YouTube roundtrip before they can be considered proven.");
    capacityNotice->setWordWrap(true);
    capacityLayout->addWidget(capacityNotice);
    auto *capacityControls =
        new QGroupBox("Experiment controls");
    auto *capacityGrid = new QGridLayout(capacityControls);
    capacityPresetCombo = new QComboBox();
    capacityPresetCombo->addItems(
        {"Smoke", "Staged Sweep", "Custom"});
    capacityOutputEdit = new QLineEdit();
    capacityOutputEdit->setPlaceholderText(
        "Experiment parent output directory");
    auto *capacityOutputBrowse = new QPushButton("Browse...");
    capacityManifestEdit = new QLineEdit();
    capacityManifestEdit->setPlaceholderText(
        "Capacity manifest.json");
    auto *capacityManifestBrowse = new QPushButton("Manifest...");
    capacityBlocksEdit = new QLineEdit("8,6,4");
    capacityBitsEdit = new QLineEdit("1,2");
    capacitySignalsEdit = new QLineEdit("0.75,1.0,1.25,1.5");
    capacityRepairsEdit = new QLineEdit("0,1,2,5");
    capacityResolutionCombo = new QComboBox();
    capacityResolutionCombo->addItems(
        {"1080p", "2160p", "1080p,2160p"});
    capacitySimulationCombo = new QComboBox();
    capacitySimulationCombo->addItems(
        {"h264-medium", "h264-light", "h264-heavy"});
    capacityMaximumCasesSpin = new QSpinBox();
    capacityMaximumCasesSpin->setRange(1, 192);
    capacityMaximumCasesSpin->setValue(64);
    capacityMaximumDiskSpin = new QDoubleSpinBox();
    capacityMaximumDiskSpin->setRange(0.5, 1024.0);
    capacityMaximumDiskSpin->setDecimals(1);
    capacityMaximumDiskSpin->setValue(20.0);
    capacityMaximumDiskSpin->setSuffix(" GiB");
    capacityShortlistSpin = new QSpinBox();
    capacityShortlistSpin->setRange(1, 12);
    capacityShortlistSpin->setValue(8);
    capacityEstimateOnlyCheck =
        new QCheckBox("Estimate only (write no videos)");
    capacityEstimateLabel = new QLabel(
        "Smoke: 12 local cases. Staged: 24 geometry/modulation cases, "
        "then at most 4 repair families and 3 resolution/profile "
        "finalists. The raw 192-case matrix is never the default.");
    capacityEstimateLabel->setWordWrap(true);
    capacityEstimateButton = new QPushButton("Estimate");
    capacityStartButton = new QPushButton("Start");
    capacityResumeButton = new QPushButton("Resume");
    capacityCancelButton = new QPushButton("Cancel");
    capacityCancelButton->setEnabled(false);
    capacityGrid->addWidget(new QLabel("Preset:"), 0, 0);
    capacityGrid->addWidget(capacityPresetCombo, 0, 1);
    capacityGrid->addWidget(new QLabel("Output:"), 0, 2);
    capacityGrid->addWidget(capacityOutputEdit, 0, 3);
    capacityGrid->addWidget(capacityOutputBrowse, 0, 4);
    capacityGrid->addWidget(new QLabel("Manifest:"), 1, 0);
    capacityGrid->addWidget(capacityManifestEdit, 1, 1, 1, 3);
    capacityGrid->addWidget(capacityManifestBrowse, 1, 4);
    capacityGrid->addWidget(new QLabel("Block sizes:"), 2, 0);
    capacityGrid->addWidget(capacityBlocksEdit, 2, 1);
    capacityGrid->addWidget(new QLabel("Bits/block:"), 2, 2);
    capacityGrid->addWidget(capacityBitsEdit, 2, 3);
    capacityGrid->addWidget(new QLabel("Signals:"), 3, 0);
    capacityGrid->addWidget(capacitySignalsEdit, 3, 1);
    capacityGrid->addWidget(new QLabel("Repair %:"), 3, 2);
    capacityGrid->addWidget(capacityRepairsEdit, 3, 3);
    capacityGrid->addWidget(new QLabel("Resolution:"), 4, 0);
    capacityGrid->addWidget(capacityResolutionCombo, 4, 1);
    capacityGrid->addWidget(new QLabel("Simulation:"), 4, 2);
    capacityGrid->addWidget(capacitySimulationCombo, 4, 3);
    capacityGrid->addWidget(new QLabel("Max cases:"), 5, 0);
    capacityGrid->addWidget(capacityMaximumCasesSpin, 5, 1);
    capacityGrid->addWidget(new QLabel("Max disk:"), 5, 2);
    capacityGrid->addWidget(capacityMaximumDiskSpin, 5, 3);
    capacityGrid->addWidget(new QLabel("Shortlist limit:"), 6, 0);
    capacityGrid->addWidget(capacityShortlistSpin, 6, 1);
    capacityGrid->addWidget(capacityEstimateOnlyCheck, 6, 2, 1, 2);
    capacityGrid->addWidget(capacityEstimateLabel, 7, 0, 1, 5);
    capacityGrid->addWidget(capacityEstimateButton, 8, 0);
    capacityGrid->addWidget(capacityStartButton, 8, 1);
    capacityGrid->addWidget(capacityResumeButton, 8, 2);
    capacityGrid->addWidget(capacityCancelButton, 8, 3);
    capacityLayout->addWidget(capacityControls);

    auto *capacityActions = new QGroupBox(
        "Shortlist and returned-video analysis");
    auto *capacityActionsGrid = new QGridLayout(capacityActions);
    capacityReturnedFolderEdit = new QLineEdit();
    capacityReturnedFolderEdit->setPlaceholderText(
        "Folder containing downloaded MP4/WebM files");
    auto *capacityReturnedBrowse = new QPushButton("Folder...");
    capacityShortlistButton =
        new QPushButton("Generate YouTube Shortlist");
    capacityAnalyzeFolderButton =
        new QPushButton("Analyze Returned Folder");
    capacityReportButton = new QPushButton("Refresh Reports");
    capacityOpenFolderButton =
        new QPushButton("Open Experiment Folder");
    capacityActionsGrid->addWidget(
        new QLabel("Returned folder:"), 0, 0);
    capacityActionsGrid->addWidget(
        capacityReturnedFolderEdit, 0, 1, 1, 3);
    capacityActionsGrid->addWidget(capacityReturnedBrowse, 0, 4);
    capacityActionsGrid->addWidget(capacityShortlistButton, 1, 0);
    capacityActionsGrid->addWidget(
        capacityAnalyzeFolderButton, 1, 1);
    capacityActionsGrid->addWidget(capacityReportButton, 1, 2);
    capacityActionsGrid->addWidget(capacityOpenFolderButton, 1, 3);
    capacityLayout->addWidget(capacityActions);
    capacityProgress = new QProgressBar();
    capacityProgress->setRange(0, 100);
    capacityLayout->addWidget(capacityProgress);
    capacityResults = new QTableWidget(0, 16);
    capacityResults->setHorizontalHeaderLabels({
        "Config", "Stage", "Block", "Bits", "Signal", "Repair",
        "Resolution", "Useful KiB/s", "Gain", "Candidate",
        "Recovery", "Margin", "BER/SER", "SHA", "Pareto",
        "Status / Shortlist reason"});
    capacityResults->horizontalHeader()->setSectionResizeMode(
        QHeaderView::ResizeToContents);
    capacityResults->horizontalHeader()->setStretchLastSection(true);
    capacityLayout->addWidget(capacityResults, 1);
    mainTabs->addTab(capacityPage, "Capacity Lab");

    connect(capacityPresetCombo,
            QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](const int index) {
        const bool custom = index == 2;
        for (auto *edit : {capacityBlocksEdit, capacityBitsEdit,
                           capacitySignalsEdit, capacityRepairsEdit})
            edit->setEnabled(custom);
        capacityResolutionCombo->setEnabled(custom);
    });
    capacityBlocksEdit->setEnabled(false);
    capacityBitsEdit->setEnabled(false);
    capacitySignalsEdit->setEnabled(false);
    capacityRepairsEdit->setEnabled(false);
    capacityResolutionCombo->setEnabled(false);
    connect(capacityOutputBrowse, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getExistingDirectory(
            this, "Capacity Lab output directory");
        if (!path.isEmpty()) capacityOutputEdit->setText(path);
    });
    connect(capacityManifestBrowse, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getOpenFileName(
            this, "Select Capacity Lab manifest", {},
            "JSON manifest (manifest.json)");
        if (!path.isEmpty()) capacityManifestEdit->setText(path);
    });
    connect(capacityReturnedBrowse, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getExistingDirectory(
            this, "Returned YouTube video folder");
        if (!path.isEmpty())
            capacityReturnedFolderEdit->setText(path);
    });
    const auto capacityRunArguments =
        [this](const QString &command) {
            const QString preset =
                capacityPresetCombo->currentIndex() == 0 ? "smoke" :
                capacityPresetCombo->currentIndex() == 1 ? "staged" :
                "custom";
            QStringList args{
                "capacitylab", command, "--preset", preset,
                "--output", capacityOutputEdit->text(),
                "--max-cases",
                QString::number(capacityMaximumCasesSpin->value()),
                "--max-disk-gib",
                QString::number(capacityMaximumDiskSpin->value()),
                "--max-shortlist-videos",
                QString::number(capacityShortlistSpin->value()),
                "--simulation",
                capacitySimulationCombo->currentText()};
            if (preset == "custom")
                args << "--block-size" << capacityBlocksEdit->text()
                     << "--bits-per-block" << capacityBitsEdit->text()
                     << "--signal" << capacitySignalsEdit->text()
                     << "--repair-percent" << capacityRepairsEdit->text()
                     << "--resolution"
                     << capacityResolutionCombo->currentText();
            if (capacityEstimateOnlyCheck->isChecked())
                args << "--estimate-only";
            return args;
        };
    connect(capacityEstimateButton, &QPushButton::clicked,
            this, [this, capacityRunArguments] {
        if (capacityOutputEdit->text().isEmpty()) return;
        startTestLabProcess(capacityRunArguments("estimate"));
    });
    connect(capacityStartButton, &QPushButton::clicked,
            this, [this, capacityRunArguments] {
        if (capacityOutputEdit->text().isEmpty()) return;
        startTestLabProcess(capacityRunArguments("run"));
    });
    connect(capacityResumeButton, &QPushButton::clicked, this, [this] {
        if (capacityManifestEdit->text().isEmpty()) return;
        startTestLabProcess({
            "capacitylab", "resume", "--manifest",
            capacityManifestEdit->text()});
    });
    connect(capacityShortlistButton, &QPushButton::clicked, this, [this] {
        if (capacityManifestEdit->text().isEmpty()) return;
        startTestLabProcess({
            "capacitylab", "shortlist", "--manifest",
            capacityManifestEdit->text(), "--max-videos",
            QString::number(capacityShortlistSpin->value())});
    });
    connect(capacityAnalyzeFolderButton, &QPushButton::clicked,
            this, [this] {
        if (capacityManifestEdit->text().isEmpty() ||
            capacityReturnedFolderEdit->text().isEmpty())
            return;
        startTestLabProcess({
            "capacitylab", "analyze-folder", "--manifest",
            capacityManifestEdit->text(), "--folder",
            capacityReturnedFolderEdit->text(),
            "--session-label", "Initial YouTube test"});
    });
    connect(capacityReportButton, &QPushButton::clicked, this, [this] {
        if (capacityManifestEdit->text().isEmpty()) return;
        startTestLabProcess({
            "capacitylab", "report", "--manifest",
            capacityManifestEdit->text(), "--format", "markdown"});
    });
    connect(capacityOpenFolderButton, &QPushButton::clicked, this, [this] {
        if (capacityManifestEdit->text().isEmpty()) return;
        QDesktopServices::openUrl(QUrl::fromLocalFile(
            QFileInfo(capacityManifestEdit->text())
                .absolutePath()));
    });
    connect(capacityCancelButton, &QPushButton::clicked, this, [this] {
        if (testLabProcess &&
            testLabProcess->state() != QProcess::NotRunning) {
            QFile cancel(testLabCancelFile);
            if (cancel.open(QIODevice::WriteOnly)) cancel.close();
            capacityCancelButton->setEnabled(false);
        }
    });

    connect(testLabPresetCombo,
            QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](const int index) {
                testLabEstimateLabel->setText(
                    index == 0
                        ? "Quick: 6 cases (requested 64 KiB random, "
                          "5/20/50%, 1080p + 4K). Effective payload "
                          "is sized for at least 60 real frames."
                        : "Full: 36 cases after de-duplicating the "
                          "current production resolution. Review disk "
                          "preflight before generation.");
            });
    connect(testLabOutputBrowse, &QPushButton::clicked, this, [this] {
        const QString directory = QFileDialog::getExistingDirectory(
            this, "Test Lab output directory");
        if (!directory.isEmpty()) testLabOutputEdit->setText(directory);
    });
    connect(testLabManifestBrowse, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getOpenFileName(
            this, "Select Test Lab manifest", {},
            "JSON manifest (manifest.json)");
        if (!path.isEmpty()) {
            testLabManifestEdit->setText(path);
            refreshTestLabDashboard();
        }
    });
    connect(testLabVideoBrowse, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getOpenFileName(
            this, "Select downloaded YouTube video", {},
            "Video files (*.mp4 *.mkv *.webm);;All files (*)");
        if (!path.isEmpty()) testLabVideoEdit->setText(path);
    });
    connect(testLabFolderBrowse, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getExistingDirectory(
            this, "Select downloaded YouTube video folder");
        if (!path.isEmpty()) testLabFolderEdit->setText(path);
    });
    connect(testLabGenerateButton, &QPushButton::clicked, this, [this] {
        if (testLabOutputEdit->text().isEmpty()) {
            QMessageBox::warning(
                this, "YouTube Test Lab", "Select an output directory.");
            return;
        }
        startTestLabProcess({
            "testlab", "generate", "--preset",
            testLabPresetCombo->currentIndex() == 0 ? "quick" : "full",
            "--output", testLabOutputEdit->text()});
    });
    connect(testLabResumeButton, &QPushButton::clicked, this, [this] {
        if (testLabManifestEdit->text().isEmpty()) return;
        startTestLabProcess({
            "testlab", "resume", "--suite",
            testLabManifestEdit->text()});
    });
    connect(testLabSimulateButton, &QPushButton::clicked, this, [this] {
        if (testLabManifestEdit->text().isEmpty()) return;
        startTestLabProcess({
            "testlab", "simulate", "--suite",
            testLabManifestEdit->text(), "--profile",
            testLabSimulationCombo->currentText()});
    });
    connect(testLabAnalyzeButton, &QPushButton::clicked, this, [this] {
        if (testLabManifestEdit->text().isEmpty() ||
            testLabVideoEdit->text().isEmpty()) return;
        QStringList args{
            "testlab", "analyze", "--suite",
            testLabManifestEdit->text(), "--video",
            testLabVideoEdit->text()};
        if (!testLabCaseEdit->text().isEmpty())
            args << "--case" << testLabCaseEdit->text();
        if (!testLabSessionLabelEdit->text().isEmpty())
            args << "--session-label"
                 << testLabSessionLabelEdit->text();
        if (testLabRecordNewCheck->isChecked())
            args << "--record-new-observation";
        testLabDuplicateWarning->clear();
        startTestLabProcess(args);
    });
    const auto appendFolderMappings =
        [this](QStringList &args) {
            const auto mappings = testLabMappingsEdit->text().split(
                ';', Qt::SkipEmptyParts);
            for (const auto &mapping : mappings)
                args << "--map" << mapping.trimmed();
        };
    connect(testLabPreviewFolderButton, &QPushButton::clicked,
            this, [this, appendFolderMappings] {
        if (testLabManifestEdit->text().isEmpty() ||
            testLabFolderEdit->text().isEmpty())
            return;
        QStringList args{
            "testlab", "analyze-folder", "--suite",
            testLabManifestEdit->text(), "--folder",
            testLabFolderEdit->text(), "--dry-run"};
        appendFolderMappings(args);
        testLabBatchPreview->setRowCount(0);
        logMessage(
            "[Test Lab] Mapping preview includes filename, detected case, "
            "resolution, codec, size, status and duplicate state.");
        startTestLabProcess(args);
    });
    connect(testLabAnalyzeFolderButton, &QPushButton::clicked,
            this, [this, appendFolderMappings] {
        if (testLabManifestEdit->text().isEmpty() ||
            testLabFolderEdit->text().isEmpty())
            return;
        QStringList args{
            "testlab", "analyze-folder", "--suite",
            testLabManifestEdit->text(), "--folder",
            testLabFolderEdit->text()};
        appendFolderMappings(args);
        if (!testLabSessionLabelEdit->text().isEmpty())
            args << "--session-label"
                 << testLabSessionLabelEdit->text();
        if (testLabRecordNewCheck->isChecked())
            args << "--record-new-observation";
        startTestLabProcess(args);
    });
    connect(testLabNewSessionButton, &QPushButton::clicked,
            this, [this] {
        testLabSessionLabelEdit->clear();
        testLabSessionLabelEdit->setFocus();
        testLabRecordNewCheck->setChecked(true);
        testLabActiveSessionLabel->setText(
            "New session: enter a label; it will be created "
            "when analysis starts.");
    });
    connect(testLabDeduplicateButton, &QPushButton::clicked,
            this, [this] {
        if (testLabManifestEdit->text().isEmpty()) return;
        const auto choice = QMessageBox::question(
            this, "Deduplicate Results",
            "Choose Yes to apply deduplication with a timestamped "
            "manifest backup. Choose No for a safe dry-run only.",
            QMessageBox::Yes | QMessageBox::No |
                QMessageBox::Cancel,
            QMessageBox::No);
        if (choice == QMessageBox::Cancel) return;
        startTestLabProcess({
            "testlab", "deduplicate", "--suite",
            testLabManifestEdit->text(),
            choice == QMessageBox::Yes ? "--apply" : "--dry-run"});
    });
    connect(testLabReportButton, &QPushButton::clicked, this, [this] {
        if (testLabManifestEdit->text().isEmpty()) return;
        startTestLabProcess({
            "testlab", "report", "--suite",
            testLabManifestEdit->text(), "--format", "markdown"});
    });
    connect(testLabCancelButton, &QPushButton::clicked, this, [this] {
        if (testLabProcess &&
            testLabProcess->state() != QProcess::NotRunning) {
            QFile cancel(testLabCancelFile);
            if (cancel.open(QIODevice::WriteOnly)) cancel.close();
            testLabCancelButton->setEnabled(false);
            logMessage(
                "[Test Lab] Cancellation requested; finishing the "
                "current safe boundary.");
        }
    });

    // Main layout
    auto *mainLayout = new QHBoxLayout(centralWidget);
    mainLayout->addWidget(mainTabs);
}

void DriveManagerUI::setupMenuBar() {
    // Menu setup - using QMainWindow's built-in menuBar
    QMenu *fileMenu = menuBar()->addMenu("&File");
    fileMenu->addAction("E&xit", this, &QWidget::close);

    QMenu *toolsMenu = menuBar()->addMenu("&Tools");
    toolsMenu->addAction("&Clear Logs", this, &DriveManagerUI::clearLogs);

    QMenu *helpMenu = menuBar()->addMenu("&Help");
    helpMenu->addAction("&About", [this]() {
        QMessageBox::about(this, "About",
                           "YouTube Media Storage Drive Manager\n\n"
                           "Encode and decode files using video storage technology\n"
                           "Version 1.0");
    });
}

void DriveManagerUI::setupStatusBar() {
    // Status bar setup - using QMainWindow's built-in statusBar
    permanentStatus = new QLabel("Ready");
    statusBar()->addPermanentWidget(permanentStatus);
}

void DriveManagerUI::connectSignals() {
    connect(selectInputButton, &QPushButton::clicked, this, &DriveManagerUI::selectInputFile);
    connect(selectOutputButton, &QPushButton::clicked, this, &DriveManagerUI::selectOutputFile);
    connect(encodeButton, &QPushButton::clicked, this, &DriveManagerUI::startEncode);
    connect(decodeButton, &QPushButton::clicked, this, &DriveManagerUI::startDecode);

    connect(addFilesButton, &QPushButton::clicked, this, &DriveManagerUI::selectInputDirectory);
    connect(removeFilesButton, &QPushButton::clicked, this, &DriveManagerUI::removeSelectedFiles);
    connect(clearFilesButton, &QPushButton::clicked, this, &DriveManagerUI::clearFileList);
    connect(batchOutputButton, &QPushButton::clicked, this, &DriveManagerUI::selectOutputDirectory);
    connect(batchEncodeButton, &QPushButton::clicked, this, &DriveManagerUI::startBatchEncode);

    connect(clearLogsButton, &QPushButton::clicked, this, &DriveManagerUI::clearLogs);
    connect(passwordVisibilityButton, &QPushButton::clicked, this, &DriveManagerUI::togglePasswordVisibility);

    connect(streamEncodeButton, &QPushButton::clicked, this, &DriveManagerUI::startStreamEncode);
    connect(streamDecodeButton, &QPushButton::clicked, this, &DriveManagerUI::startStreamDecode);
    connect(platformCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &DriveManagerUI::onPlatformChanged);
    connect(resolutionCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &DriveManagerUI::onResolutionChanged);
    connect(reliabilityProfileCombo,
            QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &DriveManagerUI::onReliabilityProfileChanged);
    connect(encodingModeCombo,
            QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &DriveManagerUI::onEncodingModeChanged);
    connect(inputFileEdit, &QLineEdit::textChanged,
            this, &DriveManagerUI::onPreflightInputChanged);
    connect(outputFileEdit, &QLineEdit::textChanged,
            this, &DriveManagerUI::onPreflightInputChanged);
    connect(encryptCheckBox, &QCheckBox::toggled,
            this, &DriveManagerUI::onPreflightInputChanged);
    connect(passwordEdit, &QLineEdit::editingFinished,
            this, &DriveManagerUI::onPreflightInputChanged);
    connect(repairPercentSpinBox,
            QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, &DriveManagerUI::onCustomRepairChanged);
    connect(preflightDetailsButton, &QToolButton::toggled,
            this, [this](const bool checked) {
                preflightDetailsButton->setArrowType(
                    checked ? Qt::DownArrow : Qt::RightArrow);
                preflightDetailsButton->setText(
                    checked ? "Hide details" : "Show details");
                preflightDetailsWidget->setVisible(checked);
            });
    connect(lowDiskOverrideCheckBox, &QCheckBox::toggled,
            this, &DriveManagerUI::onLowDiskOverrideToggled);
}

void DriveManagerUI::togglePasswordVisibility() const {
    if (passwordEdit->echoMode() == QLineEdit::Password) {
        passwordEdit->setEchoMode(QLineEdit::Normal);
        passwordVisibilityButton->setText("Hide");
    } else {
        passwordEdit->setEchoMode(QLineEdit::Password);
        passwordVisibilityButton->setText("Show");
    }
}

void DriveManagerUI::selectInputFile() {
    const QString fileName = QFileDialog::getOpenFileName(this, "Select Input File",
                                                          QStandardPaths::writableLocation(
                                                              QStandardPaths::DocumentsLocation));
    if (!fileName.isEmpty()) {
        inputFileEdit->setText(fileName);
        logMessage("Selected input file: " + fileName);
    }
}

void DriveManagerUI::selectOutputFile() {
    const QString fileName = QFileDialog::getSaveFileName(this, "Select Output File",
                                                          QStandardPaths::writableLocation(
                                                              QStandardPaths::DocumentsLocation),
                                                          "Video Files (*.mkv *.mp4);;All Files (*)");
    if (!fileName.isEmpty()) {
        outputFileEdit->setText(fileName);
        logMessage("Selected output file: " + fileName);
    }
}

void DriveManagerUI::selectInputDirectory() {
    QStringList fileNames = QFileDialog::getOpenFileNames(this, "Select Files to Encode",
                                                          QStandardPaths::writableLocation(
                                                              QStandardPaths::DocumentsLocation));

    for (const QString &fileName: fileNames) {
        if (!fileName.isEmpty() && !fileListWidget->findItems(fileName, Qt::MatchExactly).count()) {
            fileListWidget->addItem(fileName);
        }
    }

    if (!fileNames.isEmpty()) {
        logMessage(QString("Added %1 files to batch list").arg(fileNames.size()));
        updateFileList();
    }
}

void DriveManagerUI::selectOutputDirectory() {
    const QString dirName = QFileDialog::getExistingDirectory(this, "Select Output Directory",
                                                              QStandardPaths::writableLocation(
                                                                  QStandardPaths::DocumentsLocation));
    if (!dirName.isEmpty()) {
        batchOutputDirEdit->setText(dirName);
        logMessage("Selected output directory: " + dirName);
    }
}

void DriveManagerUI::startEncode() {
    if (isOperationRunning) {
        QMessageBox::warning(this, "Warning", "An operation is already in progress");
        return;
    }

    if (!validatePaths()) {
        return;
    }

    const bool encrypt = encryptCheckBox->isChecked();
    if (encrypt && passwordEdit->text().isEmpty()) {
        QMessageBox::warning(this, "Warning", "Password required when encrypting");
        return;
    }

    const auto fingerprint = currentPreflightFingerprint();
    if (!fingerprint) {
        requestPreflight();
        return;
    }

    const bool estimateTooOld =
        acceptedPreflightCompletedAt.isValid() &&
        acceptedPreflightCompletedAt.secsTo(
            QDateTime::currentDateTimeUtc()) > 300;
    GuiEncodeEligibility eligibility =
        preflightModel.eligibility(*fingerprint);
    if (estimateTooOld) {
        eligibility = GuiEncodeEligibility::RefreshRequired;
    }

    if (eligibility == GuiEncodeEligibility::RefreshRequired) {
        pendingEncodeAfterPreflight = true;
        statusLabel->setText(
            "Status: Preflight is being refreshed");
        permanentStatus->setText("Preflight is being refreshed");
        requestPreflight(estimateTooOld);
        return;
    }
    if (eligibility ==
        GuiEncodeEligibility::BlockedInsufficientDisk) {
        QMessageBox::warning(
            this, "Insufficient disk space",
            "Encoding is blocked because the required disk space is "
            "not available. Review the Preflight Estimate panel.");
        return;
    }
    if (eligibility == GuiEncodeEligibility::Blocked) {
        QMessageBox::warning(
            this, "Preflight not ready",
            "Encoding cannot start until preflight succeeds.");
        return;
    }
    if (eligibility == GuiEncodeEligibility::ConfirmDiskUnknown &&
        QMessageBox::warning(
            this, "Disk space could not be verified",
            "Available disk space could not be verified. Encoding may "
            "fail or fill the target disk. Continue anyway?",
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No) != QMessageBox::Yes) {
        return;
    }
    if (eligibility ==
            GuiEncodeEligibility::ConfirmOutputSizeUnavailable &&
        QMessageBox::warning(
            this, "Output-size estimate unavailable",
            "The FFV1 output-size probe was unavailable. Packet and "
            "frame counts are valid, but disk requirements cannot be "
            "predicted. Continue anyway?",
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No) != QMessageBox::Yes) {
        return;
    }
    if (!confirmOverwrite()) return;
    launchEncode();
}

void DriveManagerUI::launchEncode() {
    EncodingReliabilityOptions reliability;
    try {
        reliability = selectedReliabilityOptions();
    } catch (const std::exception &error) {
        QMessageBox::warning(
            this, "Invalid repair percentage",
            QString::fromUtf8(error.what()));
        return;
    }

    isOperationRunning = true;
    pendingEncodeAfterPreflight = false;
    currentOperation = "Encoding";
    encodeButton->setEnabled(false);
    decodeButton->setEnabled(false);
    const bool encrypt = encryptCheckBox->isChecked();

    workerThread = std::make_unique<WorkerThread>(WorkerThread::Encode,
                                                  inputFileEdit->text(), outputFileEdit->text(), encrypt,
                                                   passwordEdit->text(), QString(), 35000,
                                                   0, 0, 0,
                                                   reliability.repair_ratio,
                                                   acceptedPreflightEstimate,
                                                   preflightModel.lowDiskOverride(),
                                                   this);

    connect(workerThread.get(), &WorkerThread::progressUpdated,
            this, &DriveManagerUI::onProgressUpdated);
    connect(workerThread.get(), &WorkerThread::statusUpdated,
            this, &DriveManagerUI::onStatusUpdated);
    connect(workerThread.get(), &WorkerThread::operationCompleted,
            this, &DriveManagerUI::onOperationCompleted);
    connect(workerThread.get(), &WorkerThread::logMessage,
            this, &DriveManagerUI::onLogMessage);

    workerThread->start();
}

void DriveManagerUI::startDecode() {
    if (isOperationRunning) {
        QMessageBox::warning(this, "Warning", "An operation is already in progress");
        return;
    }

    if (!validatePaths()) {
        return;
    }

    isOperationRunning = true;
    currentOperation = "Decoding";
    encodeButton->setEnabled(false);
    decodeButton->setEnabled(false);

    workerThread = std::make_unique<WorkerThread>(WorkerThread::Decode,
                                                  inputFileEdit->text(), outputFileEdit->text(), false,
                                                   passwordEdit->text(), QString(), 35000,
                                                   0, 0, 0,
                                                   DEFAULT_REPAIR_RATIO,
                                                   std::nullopt, false, this);

    connect(workerThread.get(), &WorkerThread::progressUpdated,
            this, &DriveManagerUI::onProgressUpdated);
    connect(workerThread.get(), &WorkerThread::statusUpdated,
            this, &DriveManagerUI::onStatusUpdated);
    connect(workerThread.get(), &WorkerThread::operationCompleted,
            this, &DriveManagerUI::onOperationCompleted);
    connect(workerThread.get(), &WorkerThread::logMessage,
            this, &DriveManagerUI::onLogMessage);

    workerThread->start();
}

void DriveManagerUI::startBatchEncode() {
    if (isOperationRunning) {
        QMessageBox::warning(this, "Warning", "An operation is already in progress");
        return;
    }

    if (fileListWidget->count() == 0) {
        QMessageBox::warning(this, "Warning", "No files in batch list");
        return;
    }

    if (batchOutputDirEdit->text().isEmpty()) {
        QMessageBox::warning(this, "Warning", "Please select an output directory");
        return;
    }

    logMessage("Batch encoding not yet implemented - processing first file only");

    if (const QListWidgetItem *firstItem = fileListWidget->item(0)) {
        const QString inputPath = firstItem->text();
        const QFileInfo fileInfo(inputPath);
        const QString outputPath = batchOutputDirEdit->text() + "/" + fileInfo.baseName() + ".mkv";

        inputFileEdit->setText(inputPath);
        outputFileEdit->setText(outputPath);

        startEncode();
    }
}

void DriveManagerUI::onPlatformChanged(const int index) const {
    const QString baseUrl = platformCombo->itemData(index).toString();
    streamUrlEdit->setText(baseUrl);
    streamUrlEdit->setReadOnly(index != 2);
}

void DriveManagerUI::onResolutionChanged(const int index) const {
    if (const QSize res = resolutionCombo->itemData(index).toSize(); res.height() <= 1080)
        bitrateSpinBox->setValue(8000); // adaptive bitrate
    else if (res.height() <= 1440)
        bitrateSpinBox->setValue(16000);
    else
        bitrateSpinBox->setValue(35000);
}

void DriveManagerUI::onReliabilityProfileChanged(const int index) {
    const double percentage =
        reliabilityProfileCombo->itemData(index).toDouble();
    const bool fastLocal =
        encodingModeCombo->currentData().toInt() ==
        MS_ENCODING_MODE_FAST_LOCAL;
    const bool custom = percentage < 0.0 && !fastLocal;
    repairPercentSpinBox->setEnabled(custom);
    if (!custom) {
        repairPercentSpinBox->setValue(percentage);
    }
    onPreflightInputChanged();
}

void DriveManagerUI::onEncodingModeChanged(const int) {
    const bool fastLocal =
        encodingModeCombo->currentData().toInt() ==
        MS_ENCODING_MODE_FAST_LOCAL;
    reliabilityProfileCombo->setEnabled(!fastLocal);
    repairPercentSpinBox->setEnabled(
        !fastLocal &&
        reliabilityProfileCombo->currentData().toDouble() < 0.0);
    reliabilityHelpLabel->setText(
        fastLocal
            ? "Not applicable in Fast Local Mode. No repair/FEC packets "
              "are generated."
            : "Higher repair improves damage tolerance, but increases "
              "frames, time, and output size.");
    encodingModeHelpLabel->setText(
        fastLocal
            ? "Fast Local is optimized for lossless local storage and "
              "requires an .mkv output. Re-encoding or uploading the "
              "video to lossy platforms may destroy the data."
            : "Resilient / Platform produces larger output, but is more "
              "tolerant of re-encoding and supports reliability/FEC "
              "profiles.");
    onPreflightInputChanged();
}

void DriveManagerUI::onPreflightInputChanged() {
    if (shuttingDown) return;
    if (preflightDebounceTimer) preflightDebounceTimer->stop();
    pendingEncodeAfterPreflight = false;
    requestPreflight();
}

void DriveManagerUI::onCustomRepairChanged() {
    if (shuttingDown ||
        encodingModeCombo->currentData().toInt() ==
            MS_ENCODING_MODE_FAST_LOCAL ||
        reliabilityProfileCombo->currentData().toDouble() >= 0.0) {
        return;
    }

    pendingEncodeAfterPreflight = false;
    pendingPreflightJob.reset();
    acceptedPreflightEstimate.reset();
    acceptedPreflightCompletedAt = {};
    preflightModel.waitFor(GuiPreflightStatus::InputChanged);
    clearPreflightValues();
    updatePreflightPanel();
    preflightDebounceTimer->start();
}

void DriveManagerUI::runDebouncedPreflight() {
    requestPreflight();
}

std::optional<GuiPreflightFingerprint>
DriveManagerUI::currentPreflightFingerprint(
    GuiPreflightStatus *waitingStatus) const {
    if (inputFileEdit->text().trimmed().isEmpty()) {
        if (waitingStatus) {
            *waitingStatus = GuiPreflightStatus::WaitingForInput;
        }
        return std::nullopt;
    }
    if (outputFileEdit->text().trimmed().isEmpty()) {
        if (waitingStatus) {
            *waitingStatus =
                GuiPreflightStatus::WaitingForOutputPath;
        }
        return std::nullopt;
    }
    if (encryptCheckBox->isChecked() &&
        passwordEdit->text().isEmpty()) {
        if (waitingStatus) {
            *waitingStatus =
                GuiPreflightStatus::WaitingForValidSettings;
        }
        return std::nullopt;
    }

    const QFileInfo inputInfo(inputFileEdit->text());
    const QFileInfo outputInfo(outputFileEdit->text());
    const QString canonicalInput = inputInfo.canonicalFilePath();
    const QString normalizedInput =
        QDir::cleanPath(canonicalInput.isEmpty()
                            ? inputInfo.absoluteFilePath()
                            : canonicalInput);
    const QString normalizedOutput =
        QDir::cleanPath(outputInfo.absoluteFilePath());

    GuiPreflightFingerprint fingerprint;
    fingerprint.normalized_input_path =
        normalizedInput.toStdString();
    fingerprint.input_size =
        inputInfo.exists() && inputInfo.isFile()
            ? static_cast<uint64_t>(inputInfo.size())
            : 0;
    fingerprint.input_last_write_time =
        inputInfo.exists()
            ? inputInfo.lastModified().toMSecsSinceEpoch()
            : 0;
    fingerprint.normalized_output_path =
        normalizedOutput.toStdString();
    fingerprint.reliability_profile =
        encodingModeCombo->currentData().toInt() ==
                MS_ENCODING_MODE_FAST_LOCAL
            ? -1
            : reliabilityProfileCombo->currentIndex();
    fingerprint.repair_ratio =
        encodingModeCombo->currentData().toInt() ==
                MS_ENCODING_MODE_FAST_LOCAL
            ? 0.0
            : selectedReliabilityOptions().repair_ratio;
    fingerprint.encrypted = encryptCheckBox->isChecked();
    fingerprint.encoding_mode =
        encodingModeCombo->currentData().toInt();
    return fingerprint;
}

void DriveManagerUI::requestPreflight(const bool force) {
    if (shuttingDown) return;

    GuiPreflightStatus waiting =
        GuiPreflightStatus::WaitingForValidSettings;
    std::optional<GuiPreflightFingerprint> fingerprint;
    try {
        fingerprint = currentPreflightFingerprint(&waiting);
    } catch (const std::exception &) {
        waiting = GuiPreflightStatus::WaitingForValidSettings;
    }
    if (!fingerprint) {
        pendingPreflightJob.reset();
        acceptedPreflightEstimate.reset();
        acceptedPreflightCompletedAt = {};
        preflightModel.waitFor(waiting);
        clearPreflightValues();
        updatePreflightPanel();
        return;
    }

    const auto generation = preflightModel.request(
        *fingerprint, force);
    if (!generation) {
        updateEncodeEligibility();
        return;
    }

    acceptedPreflightEstimate.reset();
    acceptedPreflightCompletedAt = {};
    GuiPreflightJob job;
    job.generation = *generation;
    job.fingerprint = *fingerprint;
    job.inputPath = inputFileEdit->text();
    job.outputPath = outputFileEdit->text();
    job.encrypted = encryptCheckBox->isChecked();
    job.password = passwordEdit->text();
    job.repairRatio = fingerprint->repair_ratio;
    job.encodingMode = static_cast<ms_encoding_mode_t>(
        fingerprint->encoding_mode);
    clearPreflightValues();
    updatePreflightPanel();

    if (preflightThread && preflightThread->isRunning()) {
        pendingPreflightJob = std::move(job);
        return;
    }
    startPreflightJob(job);
}

void DriveManagerUI::startPreflightJob(
    const GuiPreflightJob &job) {
    if (shuttingDown) return;
    preflightThread = new PreflightEstimateThread(job, this);
    connect(preflightThread,
            &PreflightEstimateThread::phaseChanged,
            this,
            [this](const uint64_t generation,
                   const QString &phase) {
                if (generation != preflightModel.generation() ||
                    shuttingDown) {
                    return;
                }
                statusLabel->setText("Status: " + phase);
                permanentStatus->setText(phase);
                preflightStatusValue->setToolTip(phase);
            });
    connect(preflightThread, &QThread::finished, this,
            [this, thread = preflightThread]() {
                onPreflightFinished(thread);
            });
    preflightThread->start();
}

GuiPreflightSnapshot DriveManagerUI::currentSnapshot() const {
    GuiPreflightSnapshot snapshot;
    if (!acceptedPreflightEstimate) return snapshot;
    const auto &estimate = *acceptedPreflightEstimate;
    snapshot.output_size_estimate_available =
        estimate.output_size_estimate_available != 0;
    snapshot.disk_space_known = estimate.disk_space_known != 0;
    snapshot.disk_space_sufficient =
        estimate.disk_space_sufficient;
    snapshot.can_start_encoding =
        estimate.can_start_encoding != 0;
    snapshot.low_disk_override_permitted =
        estimate.low_disk_override_permitted != 0;
    snapshot.warning =
        QString::fromUtf8(estimate.warning).toStdString();
    snapshot.error =
        QString::fromUtf8(estimate.error).toStdString();
    return snapshot;
}

void DriveManagerUI::onPreflightFinished(
    PreflightEstimateThread *thread) {
    if (!thread) return;
    const GuiPreflightJob job = thread->job();
    const ms_status_t status = thread->resultStatus();
    const ms_encoding_estimate_t estimate = thread->estimate();

    bool accepted = false;
    if (!shuttingDown) {
        if (status == MS_OK) {
            acceptedPreflightEstimate = estimate;
            accepted = preflightModel.accept(
                job.generation, job.fingerprint,
                currentSnapshot());
            if (!accepted) acceptedPreflightEstimate.reset();
        } else {
            accepted = preflightModel.fail(
                job.generation, job.fingerprint);
        }
    }

    if (accepted) {
        acceptedPreflightCompletedAt =
            QDateTime::currentDateTimeUtc();
        preflightStatusValue->setToolTip(
            status == MS_OK
                ? QString::fromUtf8(estimate.warning)
                : QString::fromUtf8(ms_status_string(status)));
        if (status == MS_OK) {
            logPreflightEstimate();
        } else {
            logMessage(
                QString("Preflight estimate failed: %1")
                    .arg(ms_status_string(status)));
        }
        const QString completedStatus =
            QString::fromLatin1(gui_preflight_status_text(
                preflightModel.status()));
        statusLabel->setText(
            "Status: Preflight " + completedStatus);
        permanentStatus->setText(
            "Preflight " + completedStatus);
    }

    const bool wasCurrentThread = preflightThread == thread;
    thread->deleteLater();
    if (wasCurrentThread) preflightThread = nullptr;
    updatePreflightPanel();

    if (wasCurrentThread && pendingPreflightJob) {
        const GuiPreflightJob pending = *pendingPreflightJob;
        pendingPreflightJob.reset();
        startPreflightJob(pending);
        return;
    }

    if (accepted && pendingEncodeAfterPreflight &&
        preflightModel.status() !=
            GuiPreflightStatus::EstimateFailed) {
        pendingEncodeAfterPreflight = false;
        QTimer::singleShot(0, this, &DriveManagerUI::startEncode);
    } else if (accepted &&
               preflightModel.status() ==
                   GuiPreflightStatus::EstimateFailed) {
        pendingEncodeAfterPreflight = false;
    }
}

void DriveManagerUI::clearPreflightValues() {
    const QList<QLabel *> values{
        preflightInputSizeValue,
        preflightReliabilityValue,
        preflightRepairValue,
        preflightLikelyOutputValue,
        preflightRangeValue,
        preflightAvailableDiskValue,
        preflightRequiredDiskValue,
        preflightMissingDiskValue,
        preflightSourcePacketsValue,
        preflightRepairPacketsValue,
        preflightTotalPacketsValue,
        preflightFramesValue,
        preflightVideoDurationValue,
        preflightSafetyMarginValue,
        preflightProbeFramesValue,
        preflightProbeDurationValue,
        preflightMethodValue,
        preflightHeaderValue,
        preflightFrameCapacityValue,
    };
    for (QLabel *label : values) label->setText("-");
}

void DriveManagerUI::updatePreflightPanel() {
    const GuiPreflightStatus status = preflightModel.status();
    preflightStatusValue->setText(
        QString::fromLatin1(gui_preflight_status_text(status)));
    const bool estimating =
        status == GuiPreflightStatus::Estimating ||
        status == GuiPreflightStatus::InputChanged;
    preflightProgress->setVisible(estimating);
    preflightProgress->setRange(0, estimating ? 0 : 100);

    QStyle::StandardPixmap icon = QStyle::SP_MessageBoxInformation;
    if (status == GuiPreflightStatus::InsufficientDiskSpace ||
        status == GuiPreflightStatus::EstimateFailed) {
        icon = QStyle::SP_MessageBoxCritical;
    } else if (
        status == GuiPreflightStatus::DiskSpaceUnknown ||
        status ==
            GuiPreflightStatus::OutputSizeEstimateUnavailable ||
        status == GuiPreflightStatus::WaitingForValidSettings) {
        icon = QStyle::SP_MessageBoxWarning;
    } else if (status == GuiPreflightStatus::Ready) {
        icon = QStyle::SP_DialogApplyButton;
    }
    preflightStatusIcon->setPixmap(
        style()->standardIcon(icon).pixmap(18, 18));

    if (acceptedPreflightEstimate) {
        const auto &e = *acceptedPreflightEstimate;
        preflightInputSizeValue->setText(
            format_bytes(e.input_size_bytes));
        const bool fastLocal =
            e.encoding_mode == MS_ENCODING_MODE_FAST_LOCAL;
        preflightReliabilityValue->setText(
            fastLocal ? "Not applicable"
                      : reliabilityProfileCombo->currentText());
        preflightRepairValue->setText(
            fastLocal ? "Not applicable"
                      : QString("%1%").arg(
                            e.repair_percentage, 0, 'f', 2));
        preflightSourcePacketsValue->setText(
            fastLocal ? "Not applicable"
                      : format_count(e.source_packet_count));
        preflightRepairPacketsValue->setText(
            fastLocal ? "Not applicable"
                      : format_count(e.repair_packet_count));
        preflightTotalPacketsValue->setText(
            fastLocal ? "Not applicable"
                      : format_count(e.total_packet_count));
        preflightFramesValue->setText(
            format_count(e.estimated_frame_count));
        preflightVideoDurationValue->setText(
            QString("%1 s").arg(
                e.estimated_video_duration_seconds, 0, 'f', 2));
        preflightLikelyOutputValue->setText(
            e.output_size_estimate_available
                ? format_bytes(e.estimated_output_bytes)
                : "Unavailable");
        preflightRangeValue->setText(
            e.output_size_estimate_available
                ? QString("%1 - %2")
                      .arg(format_bytes(e.estimated_output_min_bytes),
                           format_bytes(e.estimated_output_max_bytes))
                : "Unavailable");
        preflightAvailableDiskValue->setText(
            e.disk_space_known
                ? format_bytes(e.available_disk_bytes)
                : "Unknown");
        preflightRequiredDiskValue->setText(
            e.required_disk_space_known
                ? format_bytes(e.required_disk_bytes)
                : "Unknown");
        preflightSafetyMarginValue->setText(
            e.required_disk_space_known
                ? format_bytes(e.safety_margin_bytes)
                : "Unknown");
        if (e.disk_space_known &&
            e.required_disk_space_known &&
            e.available_disk_bytes < e.required_disk_bytes) {
            preflightMissingDiskValue->setText(
                format_bytes(
                    e.required_disk_bytes -
                    e.available_disk_bytes));
        } else {
            preflightMissingDiskValue->setText("None");
        }
        preflightProbeFramesValue->setText(
            format_count(e.probe_frame_count));
        preflightProbeDurationValue->setText(
            QString("%1 s").arg(
                e.probe_duration_seconds, 0, 'f', 3));
        preflightMethodValue->setText(
            QString::fromUtf8(e.estimation_method));
        preflightHeaderValue->setText(
            fastLocal ? format_bytes(e.header_bytes) : "Not applicable");
        preflightFrameCapacityValue->setText(
            fastLocal
                ? format_bytes(e.frame_payload_capacity)
                : "Not applicable");
    }

    const bool lowDisk =
        status == GuiPreflightStatus::InsufficientDiskSpace;
    {
        const QSignalBlocker blocker(lowDiskOverrideCheckBox);
        lowDiskOverrideCheckBox->setVisible(lowDisk);
        lowDiskOverrideCheckBox->setEnabled(lowDisk);
        lowDiskOverrideCheckBox->setChecked(
            lowDisk && preflightModel.lowDiskOverride());
    }
    updateEncodeEligibility();
}

void DriveManagerUI::updateEncodeEligibility() {
    if (isOperationRunning) {
        encodeButton->setEnabled(false);
        return;
    }
    const auto fingerprint = currentPreflightFingerprint();
    if (!fingerprint) {
        encodeButton->setEnabled(false);
        return;
    }
    const auto eligibility =
        preflightModel.eligibility(*fingerprint);
    encodeButton->setEnabled(
        eligibility == GuiEncodeEligibility::Ready ||
        eligibility == GuiEncodeEligibility::ConfirmDiskUnknown ||
        eligibility ==
            GuiEncodeEligibility::ConfirmOutputSizeUnavailable);
}

void DriveManagerUI::onLowDiskOverrideToggled(
    const bool checked) {
    if (!checked) {
        (void)preflightModel.setLowDiskOverride(false);
        updateEncodeEligibility();
        return;
    }
    const auto answer = QMessageBox::warning(
        this, "Proceed despite insufficient disk space",
        "The target disk does not have the required free space. "
        "Continuing may fail and may fill the disk. This override "
        "applies only to the disk-space blocker. Continue?",
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes ||
        !preflightModel.setLowDiskOverride(true)) {
        const QSignalBlocker blocker(lowDiskOverrideCheckBox);
        lowDiskOverrideCheckBox->setChecked(false);
    }
    updateEncodeEligibility();
}

bool DriveManagerUI::confirmOverwrite() const {
    if (!QFileInfo::exists(outputFileEdit->text())) return true;
    return QMessageBox::warning(
               const_cast<DriveManagerUI *>(this),
               "Overwrite existing output?",
               "The selected output already exists. It will only be "
               "replaced after encoding completes successfully. "
               "Continue?",
               QMessageBox::Yes | QMessageBox::No,
               QMessageBox::No) == QMessageBox::Yes;
}

void DriveManagerUI::logPreflightEstimate() const {
    if (!acceptedPreflightEstimate) return;
    const auto &e = *acceptedPreflightEstimate;
    QStringList lines;
    lines << "Preflight estimate:"
          << QString("  Input: %1")
                 .arg(format_bytes(e.input_size_bytes))
          << QString("  Mode: %1")
                 .arg(e.encoding_mode == MS_ENCODING_MODE_FAST_LOCAL
                          ? "Fast Local"
                          : "Resilient / Platform")
          << QString("  Frames: %1")
                 .arg(format_count(e.estimated_frame_count))
          << QString("  Video duration: %1 s")
                 .arg(e.estimated_video_duration_seconds, 0, 'f', 2);
    if (e.encoding_mode == MS_ENCODING_MODE_FAST_LOCAL) {
        lines << QString("  Header: %1")
                     .arg(format_bytes(e.header_bytes))
              << QString("  Frame payload capacity: %1")
                     .arg(format_bytes(e.frame_payload_capacity));
    } else {
        lines << QString("  Reliability: %1 (%2%)")
                     .arg(reliabilityProfileCombo->currentText())
                     .arg(e.repair_percentage, 0, 'f', 2)
              << QString("  Packets: %1 source + %2 repair")
                     .arg(format_count(e.source_packet_count),
                          format_count(e.repair_packet_count));
    }
    if (e.output_size_estimate_available) {
        lines << QString("  Estimated output: %1")
                     .arg(format_bytes(e.estimated_output_bytes))
              << QString("  Expected range: %1 - %2")
                     .arg(format_bytes(e.estimated_output_min_bytes),
                          format_bytes(e.estimated_output_max_bytes));
    } else {
        lines << "  Estimated output: unavailable";
    }
    lines << QString("  Available disk: %1")
                 .arg(e.disk_space_known
                          ? format_bytes(e.available_disk_bytes)
                          : "unknown")
          << QString("  Required disk: %1")
                 .arg(e.required_disk_space_known
                          ? format_bytes(e.required_disk_bytes)
                          : "unknown")
          << QString("  Probe: %1 frames in %2 s")
                 .arg(format_count(e.probe_frame_count))
                 .arg(e.probe_duration_seconds, 0, 'f', 3)
          << QString("  Status: %1")
                 .arg(gui_preflight_status_text(
                     preflightModel.status()));
    logMessage(lines.join('\n'));
}

void DriveManagerUI::startStreamEncode() {
    if (isOperationRunning) {
        QMessageBox::warning(this, "Warning", "An operation is already in progress");
        return;
    }

    if (inputFileEdit->text().isEmpty()) {
        QMessageBox::warning(this, "Warning", "Please select an input file");
        return;
    }

    if (!QFile::exists(inputFileEdit->text())) {
        QMessageBox::warning(this, "Warning", "Input file does not exist");
        return;
    }

    if (streamKeyEdit->text().isEmpty()) {
        QMessageBox::warning(this, "Warning", "Please enter your stream key");
        return;
    }

    const QString fullUrl = streamUrlEdit->text() + streamKeyEdit->text();

    const bool encrypt = encryptCheckBox->isChecked();
    if (encrypt && passwordEdit->text().isEmpty()) {
        QMessageBox::warning(this, "Warning", "Password required when encrypting");
        return;
    }

    EncodingReliabilityOptions reliability;
    try {
        reliability = selectedReliabilityOptions();
        const QSize estimate_res =
            resolutionCombo->currentData().toSize();
        const auto layout = compute_frame_layout(
            estimate_res.width(), estimate_res.height());
        const uint64_t packets_per_frame =
            static_cast<uint64_t>(layout.bytes_per_frame) / PACKET_SIZE;
        const auto estimate = estimate_encoding_reliability(
            static_cast<uint64_t>(QFileInfo(inputFileEdit->text()).size()),
            encrypt, reliability, packets_per_frame,
            static_cast<uint32_t>(fpsSpinBox->value()));
        logReliabilityEstimate(estimate, reliability);
    } catch (const std::exception &error) {
        QMessageBox::warning(
            this, "Invalid repair percentage",
            QString::fromUtf8(error.what()));
        return;
    }

    isOperationRunning = true;
    currentOperation = "Stream Encoding";
    encodeButton->setEnabled(false);
    decodeButton->setEnabled(false);
    streamEncodeButton->setEnabled(false);
    streamDecodeButton->setEnabled(false);

    const QSize res = resolutionCombo->currentData().toSize();
    workerThread = std::make_unique<WorkerThread>(WorkerThread::StreamEncode,
                                                  inputFileEdit->text(), QString(), encrypt,
                                                  passwordEdit->text(), fullUrl,
                                                  bitrateSpinBox->value(),
                                                   res.width(), res.height(),
                                                   fpsSpinBox->value(),
                                                   reliability.repair_ratio,
                                                   std::nullopt, false, this);

    connect(workerThread.get(), &WorkerThread::progressUpdated,
            this, &DriveManagerUI::onProgressUpdated);
    connect(workerThread.get(), &WorkerThread::statusUpdated,
            this, &DriveManagerUI::onStatusUpdated);
    connect(workerThread.get(), &WorkerThread::operationCompleted,
            this, &DriveManagerUI::onOperationCompleted);
    connect(workerThread.get(), &WorkerThread::logMessage,
            this, &DriveManagerUI::onLogMessage);

    workerThread->start();
}

void DriveManagerUI::startStreamDecode() {
    if (isOperationRunning) {
        QMessageBox::warning(this, "Warning", "An operation is already in progress");
        return;
    }

    if (outputFileEdit->text().isEmpty()) {
        QMessageBox::warning(this, "Warning", "Please select an output file");
        return;
    }

    QString decodeUrl;
    if (!streamKeyEdit->text().isEmpty()) {
        decodeUrl = streamUrlEdit->text() + streamKeyEdit->text();
    } else if (!streamUrlEdit->text().isEmpty()) {
        decodeUrl = streamUrlEdit->text();
    } else {
        QMessageBox::warning(this, "Warning", "Please enter a stream URL to decode from");
        return;
    }

    isOperationRunning = true;
    currentOperation = "Stream Decoding";
    encodeButton->setEnabled(false);
    decodeButton->setEnabled(false);
    streamEncodeButton->setEnabled(false);
    streamDecodeButton->setEnabled(false);

    workerThread = std::make_unique<WorkerThread>(WorkerThread::StreamDecode,
                                                  QString(), outputFileEdit->text(), false,
                                                   passwordEdit->text(), decodeUrl,
                                                   0, 0, 0, 0,
                                                   DEFAULT_REPAIR_RATIO,
                                                   std::nullopt, false, this);

    connect(workerThread.get(), &WorkerThread::progressUpdated,
            this, &DriveManagerUI::onProgressUpdated);
    connect(workerThread.get(), &WorkerThread::statusUpdated,
            this, &DriveManagerUI::onStatusUpdated);
    connect(workerThread.get(), &WorkerThread::operationCompleted,
            this, &DriveManagerUI::onOperationCompleted);
    connect(workerThread.get(), &WorkerThread::logMessage,
            this, &DriveManagerUI::onLogMessage);

    workerThread->start();
}

void DriveManagerUI::clearLogs() const {
    logTextEdit->clear();
    logMessage("Logs cleared");
}

void DriveManagerUI::removeSelectedFiles() const {
    for (const QList<QListWidgetItem *> selectedItems = fileListWidget->selectedItems(); const QListWidgetItem *item:
         selectedItems) {
        delete fileListWidget->takeItem(fileListWidget->row(item));
    }
    updateFileList();
}

void DriveManagerUI::clearFileList() const {
    fileListWidget->clear();
    updateFileList();
}

void DriveManagerUI::updateFileList() const {
    permanentStatus->setText(QString("Files in queue: %1").arg(fileListWidget->count()));
}

void DriveManagerUI::onOperationCompleted(
    const bool success, const QString &message, const int status) {
    isOperationRunning = false;
    decodeButton->setEnabled(true);
    streamEncodeButton->setEnabled(true);
    streamDecodeButton->setEnabled(true);

    if (success) {
        logMessage("✓ " + message);
        QMessageBox::information(this, "Success", message);
        passwordEdit->clear();
    } else {
        logMessage("✗ " + message);
        QMessageBox::critical(this, "Error", message);
    }

    resetProgress();
    if (workerThread && workerThread->isRunning()) {
        workerThread->wait();
    }
    workerThread.reset();
    updateEncodeEligibility();

    if (status == MS_ERR_PREFLIGHT_STALE) {
        logMessage(
            "Preflight metadata became stale; refreshing estimate.");
        requestPreflight(true);
    }
}

void DriveManagerUI::onProgressUpdated(const int percentage) const {
    progressBar->setValue(percentage);
    progressLabel->setText(QString("%1% - %2").arg(percentage).arg(currentOperation));
}

void DriveManagerUI::onStatusUpdated(const QString &status) const {
    statusLabel->setText("Status: " + status);
    permanentStatus->setText(status);
}

void DriveManagerUI::onLogMessage(const QString &message) const {
    logMessage(message);
}

void DriveManagerUI::startTestLabProcess(
    const QStringList &arguments) {
    if (testLabProcess &&
        testLabProcess->state() != QProcess::NotRunning) {
        QMessageBox::information(
            this, "YouTube Test Lab",
            "A Test Lab operation is already running.");
        return;
    }
    if (!testLabProcess) {
        testLabProcess = new QProcess(this);
        testLabProcess->setProcessChannelMode(
            QProcess::MergedChannels);
        connect(testLabProcess, &QProcess::readyReadStandardOutput,
                this, [this] {
            testLabOutputBuffer += QString::fromLocal8Bit(
                testLabProcess->readAllStandardOutput());
            int newline = -1;
            while ((newline =
                        testLabOutputBuffer.indexOf('\n')) >= 0) {
                const QString line =
                    testLabOutputBuffer.left(newline);
                testLabOutputBuffer.remove(0, newline + 1);
                if (line.trimmed().isEmpty()) continue;
                logMessage("[Test Lab] " + line.trimmed());
                const QString marker = "Suite generated: ";
                const int position = line.indexOf(marker);
                if (position >= 0)
                    testLabManifestEdit->setText(
                        line.mid(position + marker.size()).trimmed());
                const QString capacityMarker =
                    "CAPACITY_MANIFEST ";
                const int capacityPosition =
                    line.indexOf(capacityMarker);
                if (capacityPosition >= 0)
                    capacityManifestEdit->setText(
                        line.mid(
                            capacityPosition +
                            capacityMarker.size()).trimmed());
                if (line.startsWith("CAPACITY_PROGRESS ")) {
                    const QString counts =
                        line.section(' ', 1, 1);
                    const int completed =
                        counts.section('/', 0, 0).toInt();
                    const int total =
                        counts.section('/', 1, 1).toInt();
                    if (total > 0) {
                        capacityProgress->setRange(0, total);
                        capacityProgress->setValue(completed);
                    }
                }
                if (line.contains(
                        "This video has already been analyzed for this case."))
                    testLabDuplicateWarning->setText(
                        "This video has already been analyzed for this case. "
                        "The existing observation details are in the log.");
                const auto previewColumns = line.trimmed().split('\t');
                if (previewColumns.size() == 7 &&
                    previewColumns.front() != "Filename") {
                    const int row = testLabBatchPreview->rowCount();
                    testLabBatchPreview->insertRow(row);
                    for (int column = 0;
                         column < previewColumns.size(); ++column)
                        testLabBatchPreview->setItem(
                            row, column,
                            new QTableWidgetItem(
                                previewColumns.at(column)));
                    QString userMapping;
                    for (const auto &mapping :
                         testLabMappingsEdit->text().split(
                             ';', Qt::SkipEmptyParts)) {
                        if (mapping.trimmed().startsWith(
                                previewColumns.front() + "=")) {
                            userMapping =
                                mapping.section('=', 1).trimmed();
                            break;
                        }
                    }
                    testLabBatchPreview->setItem(
                        row, 7,
                        new QTableWidgetItem(userMapping));
                }
            }
        });
        connect(
            testLabProcess,
            QOverload<int, QProcess::ExitStatus>::of(
                &QProcess::finished),
            this, [this](const int exitCode,
                         QProcess::ExitStatus status) {
                testLabProgress->setRange(0, 100);
                testLabProgress->setValue(
                    status == QProcess::NormalExit &&
                    exitCode == 0 ? 100 : 0);
                testLabCancelButton->setEnabled(false);
                QFile::remove(testLabCancelFile);
                testLabGenerateButton->setEnabled(true);
                testLabResumeButton->setEnabled(true);
                testLabSimulateButton->setEnabled(true);
                testLabAnalyzeButton->setEnabled(true);
                testLabPreviewFolderButton->setEnabled(true);
                testLabAnalyzeFolderButton->setEnabled(true);
                testLabNewSessionButton->setEnabled(true);
                testLabDeduplicateButton->setEnabled(true);
                testLabReportButton->setEnabled(true);
                refreshTestLabDashboard();
                capacityProgress->setRange(0, 100);
                capacityProgress->setValue(
                    status == QProcess::NormalExit &&
                    exitCode == 0 ? 100 : 0);
                capacityCancelButton->setEnabled(false);
                capacityEstimateButton->setEnabled(true);
                capacityStartButton->setEnabled(true);
                capacityResumeButton->setEnabled(true);
                capacityShortlistButton->setEnabled(true);
                capacityAnalyzeFolderButton->setEnabled(true);
                capacityReportButton->setEnabled(true);
                refreshCapacityLabDashboard();
            });
    }
    const QString executable =
        QCoreApplication::applicationDirPath() +
#if defined(_WIN32)
        "/media_storage.exe";
#else
        "/media_storage";
#endif
    testLabCancelFile =
        QDir::tempPath() + "/vidstorex-testlab-cancel-" +
        QString::number(QCoreApplication::applicationPid());
    QFile::remove(testLabCancelFile);
    testLabOutputBuffer.clear();
    QStringList processArguments = arguments;
    processArguments << "--cancel-file" << testLabCancelFile;
    testLabProgress->setRange(0, 0);
    testLabCancelButton->setEnabled(true);
    testLabGenerateButton->setEnabled(false);
    testLabResumeButton->setEnabled(false);
    testLabSimulateButton->setEnabled(false);
    testLabAnalyzeButton->setEnabled(false);
    testLabPreviewFolderButton->setEnabled(false);
    testLabAnalyzeFolderButton->setEnabled(false);
    testLabNewSessionButton->setEnabled(false);
    testLabDeduplicateButton->setEnabled(false);
    testLabReportButton->setEnabled(false);
    capacityProgress->setRange(0, 0);
    capacityCancelButton->setEnabled(true);
    capacityEstimateButton->setEnabled(false);
    capacityStartButton->setEnabled(false);
    capacityResumeButton->setEnabled(false);
    capacityShortlistButton->setEnabled(false);
    capacityAnalyzeFolderButton->setEnabled(false);
    capacityReportButton->setEnabled(false);
    logMessage(
        "[Test Lab] Starting: " + arguments.join(' '));
    testLabProcess->start(executable, processArguments);
}

void DriveManagerUI::refreshTestLabDashboard() {
    if (!testLabResults || !testLabManifestEdit ||
        testLabManifestEdit->text().isEmpty())
        return;
    QFile file(testLabManifestEdit->text());
    if (!file.open(QIODevice::ReadOnly)) return;
    QJsonParseError error;
    const QJsonDocument document =
        QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError ||
        !document.isObject())
        return;
    const QJsonObject manifestObject = document.object();
    const QJsonArray cases =
        manifestObject.value("cases").toArray();
    const QString activeSession =
        manifestObject.value("active_analysis_session_id").toString();
    testLabActiveSessionLabel->setText(
        "Active analysis session: " +
        (activeSession.isEmpty() ? QString("-") : activeSession));
    QSet<QString> observationKeys;
    int detectedDuplicates = 0;
    for (const auto &caseValue : cases) {
        const auto caseObject = caseValue.toObject();
        const auto caseName =
            caseObject.value("test_case_id").toString();
        for (const auto &resultValue :
             caseObject.value("results").toArray()) {
            const auto resultObject = resultValue.toObject();
            const QString key =
                caseName + "|" +
                resultObject.value("source_type").toString() + "|" +
                resultObject.value("source_file_sha256").toString() + "|" +
                resultObject.value("analysis_fingerprint").toString();
            if (!resultObject.value("source_file_sha256")
                     .toString().isEmpty() &&
                observationKeys.contains(key))
                ++detectedDuplicates;
            else
                observationKeys.insert(key);
        }
    }
    if (detectedDuplicates > 0)
        testLabDuplicateWarning->setText(
            QString("Duplicate observations detected: %1. "
                    "Use Deduplicate Results for a dry-run review.")
                .arg(detectedDuplicates));
    testLabResults->setRowCount(cases.size());
    for (int row = 0; row < cases.size(); ++row) {
        const QJsonObject item = cases.at(row).toObject();
        const QJsonObject video = item.value("video").toObject();
        const QJsonArray results = item.value("results").toArray();
        QJsonObject result;
        if (!results.isEmpty())
            result = results.last().toObject();
        const QJsonObject telemetry =
            result.value("telemetry").toObject();
        const QString resolution =
            QString("%1x%2")
                .arg(video.value("width").toInt())
                .arg(video.value("height").toInt());
        const QString packets = results.isEmpty()
            ? "-"
            : QString("%1 (%2%)")
                .arg(telemetry.value("valid_packets")
                         .toVariant().toULongLong())
                .arg(result.value("packet_recovery_percentage")
                         .toDouble(), 0, 'f', 2);
        const quint64 legacyInput =
            item.value("input_size").toVariant().toULongLong();
        const quint64 requested =
            item.contains("requested_input_size")
                ? item.value("requested_input_size")
                      .toVariant().toULongLong()
                : legacyInput;
        const quint64 effective =
            item.contains("effective_input_size")
                ? item.value("effective_input_size")
                      .toVariant().toULongLong()
                : legacyInput;
        const bool validationKnown =
            item.value(
                "candidate_duration_validation_known").toBool();
        const bool youtubeReady =
            validationKnown &&
            item.value("candidate_ready_for_youtube").toBool();
        const QString validation =
            validationKnown
                ? (item.value("candidate_validation_error")
                           .toString().isEmpty()
                       ? "Passed"
                       : item.value("candidate_validation_error")
                             .toString())
                : (item.value("candidate_validation_error")
                           .toString().isEmpty()
                       ? "Duration validation unknown"
                       : item.value("candidate_validation_error")
                             .toString());
        const QString status =
            results.isEmpty()
                ? item.value("processing_state").toString()
                : QString("%1 / %2")
                      .arg(result.value("sha256_match").toBool()
                               ? "SHA Yes" : "SHA No",
                           result.value("final_status").toString());
        const QStringList values{
            item.value("test_case_id").toString(),
            resolution,
            item.value("reliability_profile").toString(),
            QString::number(requested),
            QString::number(effective),
            QString("%1 s / %2 frames")
                .arg(item.value("minimum_duration_seconds")
                         .toDouble(2.0), 0, 'f', 2)
                .arg(item.value("minimum_required_frames")
                         .toVariant().toULongLong()),
            QString::number(
                item.value("expected_encoded_frames")
                    .toVariant().toULongLong()),
            QString::number(
                item.value("actual_candidate_frames")
                    .toVariant().toULongLong()),
            QString("%1 s")
                .arg(item.value("candidate_duration_seconds")
                         .toDouble(), 0, 'f', 3),
            validationKnown
                ? (youtubeReady ? "Yes" : "No")
                : "Unknown",
            validation,
            QString::number(
                item.value("upload_candidate_size")
                    .toVariant().toULongLong()),
            results.isEmpty()
                ? "-"
                : QString::number(
                    result.value("downloaded_video_size")
                        .toVariant().toULongLong()),
            packets,
            status
        };
        for (int column = 0; column < values.size(); ++column)
            testLabResults->setItem(
                row, column,
                new QTableWidgetItem(values.at(column)));
    }
}

void DriveManagerUI::refreshCapacityLabDashboard() {
    if (!capacityResults || !capacityManifestEdit ||
        capacityManifestEdit->text().isEmpty())
        return;
    QFile file(capacityManifestEdit->text());
    if (!file.open(QIODevice::ReadOnly)) return;
    QJsonParseError error;
    const QJsonDocument document =
        QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError ||
        !document.isObject())
        return;
    const QJsonObject manifest = document.object();
    if (manifest.value("manifest_type").toString() !=
        "youtube-capacity-lab")
        return;
    const QJsonArray cases = manifest.value("cases").toArray();
    capacityResults->setRowCount(cases.size());
    for (int row = 0; row < cases.size(); ++row) {
        const QJsonObject item = cases.at(row).toObject();
        const QJsonArray results = item.value("results").toArray();
        QJsonObject result;
        if (!results.isEmpty())
            result = results.last().toObject();
        const QString state = item.value("state").toString();
        const bool rejected =
            state == "Rejected" || state == "Failed";
        const bool eligible =
            item.value("eligible_for_shortlist").toBool();
        const bool selected =
            eligible && !rejected &&
            item.value("shortlisted").toBool();
        QString status;
        if (selected) {
            status = item.value("shortlist_reason").toString();
        } else {
            const QString failedProfile =
                item.value("failed_mandatory_profile").toString();
            const QString exclusion =
                item.value("shortlist_exclusion_reason").toString();
            const QString rejection =
                item.value("rejection_reason").toString();
            status = !exclusion.isEmpty()
                ? exclusion
                : !rejection.isEmpty() ? rejection : state;
            if (!failedProfile.isEmpty())
                status = failedProfile + ": " + status;
        }
        const QStringList values{
            item.value("config_id").toString(),
            QString::number(item.value("stage").toInt()),
            QString::number(item.value("block_width").toInt()),
            QString::number(item.value("bits_per_block").toInt()),
            QString::number(
                item.value("signal_milli").toInt() / 1000.0,
                'f', 2),
            QString::number(
                item.value("repair_basis_points").toInt() / 100.0,
                'f', 0),
            QString("%1x%2")
                .arg(item.value("resolution_width").toInt())
                .arg(item.value("resolution_height").toInt()),
            QString::number(
                item.value("useful_payload_bytes_per_second")
                    .toDouble() / 1024.0,
                'f', 2),
            QString::number(
                item.value("useful_payload_gain").toDouble(),
                'f', 2),
            QString::number(
                item.value("candidate_size")
                    .toVariant().toULongLong()),
            results.isEmpty()
                ? "-"
                : QString::number(
                    result.value("packet_recovery_percent")
                        .toDouble(), 'f', 2),
            results.isEmpty()
                ? "-"
                : QString::number(
                    result.value("recovery_margin_percent")
                        .toDouble(), 'f', 2),
            results.isEmpty()
                ? "-"
                : QString("%1/%2")
                    .arg(result.value("raw_ber").toDouble(),
                         0, 'g', 3)
                    .arg(result.value("raw_ser").toDouble(),
                         0, 'g', 3),
            results.isEmpty()
                ? "-"
                : (result.value("sha256_match").toBool()
                       ? "Exact" : "Mismatch"),
            item.value("pareto").toBool()
                ? "Frontier"
                : (item.value("dominated").toBool()
                       ? "Dominated" : "-"),
            status
        };
        for (int column = 0; column < values.size(); ++column)
            capacityResults->setItem(
                row, column,
                new QTableWidgetItem(values.at(column)));
    }
}

void DriveManagerUI::resetProgress() {
    progressBar->setValue(0);
    progressLabel->setText("Ready");
    statusLabel->setText("Status: Idle");
    currentOperation = "Idle";
}

void DriveManagerUI::logMessage(const QString &message) const {
    QString timestamp = QDateTime::currentDateTime().toString("hh:mm:ss");
    logTextEdit->append(QString("[%1] %2").arg(timestamp, message));
}

EncodingReliabilityOptions
DriveManagerUI::selectedReliabilityOptions() const {
    const double percentage = repairPercentSpinBox->value();
    return {repair_percentage_to_ratio(percentage)};
}

void DriveManagerUI::logReliabilityEstimate(
    const EncodingReliabilityEstimate &estimate,
    const EncodingReliabilityOptions &options) const {
    logMessage(QString("Reliability: %1% repair (ratio %2)")
        .arg(repair_ratio_to_percentage(options.repair_ratio), 0, 'f', 2)
        .arg(options.repair_ratio, 0, 'f', 4));
    logMessage(QString("Estimate: %1 source + %2 repair = %3 packets")
        .arg(estimate.source_packet_count)
        .arg(estimate.repair_packet_count)
        .arg(estimate.total_packet_count));
    logMessage(QString("Estimate: %1 frames, %2 seconds of video")
        .arg(estimate.frame_count)
        .arg(estimate.video_duration_seconds, 0, 'f', 2));
}

bool DriveManagerUI::validatePaths() {
    if (inputFileEdit->text().isEmpty()) {
        QMessageBox::warning(this, "Warning", "Please select an input file");
        return false;
    }

    if (outputFileEdit->text().isEmpty()) {
        QMessageBox::warning(this, "Warning", "Please select an output file");
        return false;
    }

    if (!QFile::exists(inputFileEdit->text())) {
        QMessageBox::warning(this, "Warning", "Input file does not exist");
        return false;
    }

    if (encodingModeCombo->currentData().toInt() ==
            MS_ENCODING_MODE_FAST_LOCAL &&
        QFileInfo(outputFileEdit->text()).suffix().compare(
            "mkv", Qt::CaseInsensitive) != 0) {
        QMessageBox::warning(
            this, "Fast Local requires MKV",
            "Fast Local uses FFV1/GRAY8 in Matroska. Choose an output "
            "file ending in .mkv; an MP4-named Matroska file will not "
            "be created.");
        return false;
    }

    return true;
}

void DriveManagerUI::loadSettings() {
    const QSettings settings;
    restoreGeometry(settings.value("geometry").toByteArray());
    restoreState(settings.value("windowState").toByteArray());
}

void DriveManagerUI::saveSettings() const {
    QSettings settings;
    settings.setValue("geometry", saveGeometry());
    settings.setValue("windowState", saveState());
}
