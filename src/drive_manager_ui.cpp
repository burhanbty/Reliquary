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
#include "instant_recovery.h"
#include "youtube_auth.h"
#include "youtube_sync_controller.h"
#include "youtube_upload_manager.h"
#include "encoding_reliability.h"
#include "media_storage.h"
#include "video_encoder.h"
#include "video_set.h"
#include "youtube_test_lab.h"
#include "interface_language.h"
#include "ui_theme.h"
#include "visual_components.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFile>
#include <QApplication>
#include <QClipboard>
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
#include <QFontMetrics>
#include <QDateTime>
#include <QDesktopServices>
#include <QFormLayout>
#include <QStyle>
#include <QLocale>
#include <QSignalBlocker>
#include <QSet>
#include <QUrl>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QTextCursor>
#include <QTextDocument>
#include <QCloseEvent>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QTabBar>
#include <QMenu>
#include <QKeyEvent>

#include <chrono>
#include <cmath>
#include <numeric>
#include <vector>

namespace {

[[maybe_unused]] constexpr const char *kVisualIdentityTranslationSources[]{
    QT_TRANSLATE_NOOP("DriveManagerUI", "General"),
    QT_TRANSLATE_NOOP("DriveManagerUI", "Language"),
    QT_TRANSLATE_NOOP("DriveManagerUI", "Storage"),
    QT_TRANSLATE_NOOP("DriveManagerUI", "Advanced"),
    QT_TRANSLATE_NOOP("DriveManagerUI",
        "Changes apply immediately and are saved for the next launch."),
    QT_TRANSLATE_NOOP("DriveManagerUI", "Advanced / YouTube Test Lab"),
    QT_TRANSLATE_NOOP("DriveManagerUI", "Advanced / Capacity Lab"),
    QT_TRANSLATE_NOOP("DriveManagerUI",
        "Technical tools, experiments and low-level controls."),
    QT_TRANSLATE_NOOP("DriveManagerUI", "Open the videos folder."),
    QT_TRANSLATE_NOOP("DriveManagerUI", "Upload every video as Unlisted."),
    QT_TRANSLATE_NOOP("DriveManagerUI",
        "Wait for 1080p processing to finish."),
    QT_TRANSLATE_NOOP("DriveManagerUI",
        "Put all videos in one playlist and copy its link."),
    QT_TRANSLATE_NOOP("DriveManagerUI",
        "VidStoreX never signs in and never uploads automatically."),
    QT_TRANSLATE_NOOP("DriveManagerUI",
        "VERIFIED BLOCKS → FULL-FILE SHA-256 → EXACT OUTPUT")};

} // namespace

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

static QString recent_opened_setting_key(const QString &manifestPath) {
    const auto digest = QCryptographicHash::hash(
        QDir::cleanPath(manifestPath).toUtf8(),
        QCryptographicHash::Sha256).toHex();
    return "videoSet/recentOpened/" + QString::fromLatin1(digest);
}

static youtube_sync::EndpointSet youtube_endpoint_set() {
    youtube_sync::EndpointSet endpoints;
#ifdef VIDSTOREX_ENABLE_TEST_HOOKS
    const QByteArray apiBase = qgetenv("VIDSTOREX_YOUTUBE_API_BASE");
    const QByteArray uploadBase = qgetenv("VIDSTOREX_YOUTUBE_UPLOAD_BASE");
    if (!apiBase.isEmpty()) endpoints.api_base = apiBase.toStdString();
    if (!uploadBase.isEmpty()) endpoints.upload_base = uploadBase.toStdString();
#endif
    return endpoints;
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
    loadSettings();
    uiTranslator = new QTranslator(this);
    setUiLanguage(uiLanguage, false);
    setWindowTitle(QStringLiteral("VidStoreX"));
    setMinimumSize(1024, 640);

    setupUI();
    setupMenuBar();
    setupStatusBar();
    connectSignals();
    retranslateUserInterface();

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
    if (videoSetProcess &&
        videoSetProcess->state() != QProcess::NotRunning) {
        videoSetProcess->terminate();
        if (!videoSetProcess->waitForFinished(3000))
            videoSetProcess->kill();
    }
    if (videoSetDownloadProcess &&
        videoSetDownloadProcess->state() != QProcess::NotRunning) {
        videoSetDownloadProcess->terminate();
        if (!videoSetDownloadProcess->waitForFinished(3000))
            videoSetDownloadProcess->kill();
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
    reliabilityProfileCombo->setObjectName("reliabilityProfileCombo");
    reliabilityProfileCombo->addItem(
        "High Capacity (5%)",
        static_cast<int>(ReliabilityProfile::HighCapacity));
    reliabilityProfileCombo->addItem(
        "Balanced (20%)",
        static_cast<int>(ReliabilityProfile::Balanced));
    reliabilityProfileCombo->addItem(
        "Resilient (5%)",
        static_cast<int>(ReliabilityProfile::Local));
    reliabilityProfileCombo->addItem(
        "Durable (50%)",
        static_cast<int>(ReliabilityProfile::Durable));
    reliabilityProfileCombo->addItem("Custom", -1);
    reliabilityProfileCombo->setCurrentIndex(2);
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

    {
        const QSettings settings;
        const int saved_id = settings.value(
            "encoding/reliabilityProfileId",
            static_cast<int>(ReliabilityProfile::Local)).toInt();
        const auto saved_profile =
            reliability_profile_from_id(saved_id);
        const int saved_index = reliabilityProfileCombo->findData(
            static_cast<int>(saved_profile));
        reliabilityProfileCombo->setCurrentIndex(
            saved_index >= 0 ? saved_index : 2);
        repairPercentSpinBox->setValue(
            reliability_profile_definition(saved_profile)
                .repair_percentage);
    }

    reliabilityHelpLabel = new QLabel(
        "Higher repair improves damage tolerance, but increases frames, time, and output size.");
    reliabilityHelpLabel->setObjectName("reliabilityHelpLabel");
    reliabilityHelpLabel->setWordWrap(true);
    reliabilityHelpLabel->setStyleSheet("color: palette(mid); font-size: 9pt;");
    fileOpsLayout->addWidget(reliabilityHelpLabel, 7, 0, 1, 3);

    videoSetCheckBox = new QCheckBox(
        "Split as Video Set (file encode only; opens Video Set tab)");
    videoSetCheckBox->setObjectName("videoSetCheckBox");
    fileOpsLayout->addWidget(videoSetCheckBox, 8, 0, 1, 3);

    encodeButton = new QPushButton("Encode to Video");
    encodeButton->setObjectName("encodeButton");
    encodeButton->setIcon(QIcon::fromTheme("media-record"));
    fileOpsLayout->addWidget(encodeButton, 9, 0, 1, 3);

    decodeButton = new QPushButton("Decode from Video");
    decodeButton->setIcon(QIcon::fromTheme("media-playback-start"));
    fileOpsLayout->addWidget(decodeButton, 10, 0, 1, 3);

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

    videoSetPage = new QWidget();
    auto *videoSetLayout = new QVBoxLayout(videoSetPage);
    videoSetIntroLabel = new QLabel(
        "<b>Video Set / Large Files</b><br>Splits one file into independently "
        "verified videos using the existing encoder. Resilient remains the "
        "default; High Capacity is explicit opt-in. Filenames and playlist "
        "order are never used as identities. Upload/download stays manual.");
    videoSetIntroLabel->setObjectName("videoSetIntroLabel");
    videoSetIntroLabel->setWordWrap(true);
    videoSetLayout->addWidget(videoSetIntroLabel);

    const auto &videoSetValidation = video_set::kRealYoutubeValidation;
    videoSetValidationLabel = new QLabel(
        QString::fromUtf8(videoSetValidation.gui_statement.data(),
                          static_cast<int>(videoSetValidation.gui_statement.size())) +
        "<br>" +
        QString::fromUtf8(videoSetValidation.recovery_requirement.data(),
                          static_cast<int>(videoSetValidation.recovery_requirement.size())));
    videoSetValidationLabel->setObjectName("videoSetValidationLabel");
    videoSetValidationLabel->setWordWrap(true);
    videoSetLayout->addWidget(videoSetValidationLabel);

    auto *videoSetEncodeGroup = new QGroupBox("Encode a Video Set");
    auto *videoSetEncodeLayout = new QGridLayout(videoSetEncodeGroup);
    videoSetInputEdit = new QLineEdit();
    videoSetInputEdit->setPlaceholderText("Source file");
    auto *videoSetInputBrowse = new QPushButton("File...");
    videoSetOutputEdit = new QLineEdit();
    videoSetOutputEdit->setPlaceholderText("Output root");
    auto *videoSetOutputBrowse = new QPushButton("Folder...");
    videoSetProfileCombo = new QComboBox();
    videoSetProfileCombo->addItem("Resilient (default)", "resilient");
    videoSetProfileCombo->addItem("High Capacity (opt-in, 6/6 exact)", "high-capacity");
    videoSetProfileCombo->addItem("Balanced", "balanced");
    videoSetProfileCombo->addItem("Durable", "durable");
    videoSetTargetSpin = new QSpinBox();
    videoSetTargetSpin->setRange(1, 86400);
    videoSetTargetSpin->setValue(600);
    videoSetTargetSpin->setSuffix(" s");
    videoSetMaximumSizeSpin = new QSpinBox();
    videoSetMaximumSizeSpin->setRange(0, 1024 * 1024);
    videoSetMaximumSizeSpin->setValue(1500);
    videoSetMaximumSizeSpin->setSuffix(" MiB (0 disables)");
    videoSetReserveSpin = new QDoubleSpinBox();
    videoSetReserveSpin->setRange(0.0, 99.0);
    videoSetReserveSpin->setValue(10.0);
    videoSetReserveSpin->setSuffix("%");
    videoSetPlanButton = new QPushButton("Plan");
    videoSetEncodeButton = new QPushButton("Encode Set");
    videoSetResumeButton = new QPushButton("Resume Encode");
    videoSetCancelButton = new QPushButton("Cancel");
    videoSetCancelButton->setEnabled(false);
    videoSetEncodeLayout->addWidget(new QLabel("Source:"), 0, 0);
    videoSetEncodeLayout->addWidget(videoSetInputEdit, 0, 1);
    videoSetEncodeLayout->addWidget(videoSetInputBrowse, 0, 2);
    videoSetEncodeLayout->addWidget(new QLabel("Output root:"), 1, 0);
    videoSetEncodeLayout->addWidget(videoSetOutputEdit, 1, 1);
    videoSetEncodeLayout->addWidget(videoSetOutputBrowse, 1, 2);
    videoSetEncodeLayout->addWidget(new QLabel("Profile:"), 2, 0);
    videoSetEncodeLayout->addWidget(videoSetProfileCombo, 2, 1, 1, 2);
    videoSetEncodeLayout->addWidget(new QLabel("Target duration:"), 3, 0);
    videoSetEncodeLayout->addWidget(videoSetTargetSpin, 3, 1);
    videoSetEncodeLayout->addWidget(new QLabel("Maximum actual size:"), 4, 0);
    videoSetEncodeLayout->addWidget(videoSetMaximumSizeSpin, 4, 1);
    videoSetEncodeLayout->addWidget(new QLabel("Safety reserve:"), 5, 0);
    videoSetEncodeLayout->addWidget(videoSetReserveSpin, 5, 1);
    auto *videoSetButtons = new QHBoxLayout();
    videoSetButtons->addWidget(videoSetPlanButton);
    videoSetButtons->addWidget(videoSetEncodeButton);
    videoSetButtons->addWidget(videoSetResumeButton);
    videoSetButtons->addWidget(videoSetCancelButton);
    videoSetEncodeLayout->addLayout(videoSetButtons, 6, 0, 1, 3);
    videoSetLayout->addWidget(videoSetEncodeGroup);

    videoSetPlanTable = new QTableWidget(0, 6);
    videoSetPlanTable->setHorizontalHeaderLabels(
        {"Part", "Offset", "Chunk bytes", "Frames", "Duration", "State"});
    videoSetPlanTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    videoSetLayout->addWidget(videoSetPlanTable);
    videoSetProgress = new QProgressBar();
    videoSetProgress->setRange(0, 100);
    videoSetProgress->setValue(0);
    videoSetProgress->setFormat("Video Set idle");
    videoSetLayout->addWidget(videoSetProgress);

    auto *videoSetRecoveryGroup = new QGroupBox("Inspect / Recover returned videos");
    auto *videoSetRecoveryLayout = new QGridLayout(videoSetRecoveryGroup);
    videoSetRecoveryInputEdit = new QLineEdit();
    videoSetRecoveryInputEdit->setPlaceholderText("Manifest, video, or mixed returned folder");
    auto *videoSetRecoveryBrowse = new QPushButton("Choose...");
    videoSetRecoveryOutputEdit = new QLineEdit();
    videoSetRecoveryOutputEdit->setPlaceholderText("Recovered output folder");
    auto *videoSetRecoveryOutputBrowse = new QPushButton("Folder...");
    videoSetScanButton = new QPushButton("Detect Sets / Scan");
    videoSetRecoverButton = new QPushButton("Recover");
    videoSetRecoverResumeButton = new QPushButton("Resume Recovery");
    videoSetRecoveryLayout->addWidget(new QLabel("Input:"), 0, 0);
    videoSetRecoveryLayout->addWidget(videoSetRecoveryInputEdit, 0, 1);
    videoSetRecoveryLayout->addWidget(videoSetRecoveryBrowse, 0, 2);
    videoSetRecoveryLayout->addWidget(new QLabel("Output:"), 1, 0);
    videoSetRecoveryLayout->addWidget(videoSetRecoveryOutputEdit, 1, 1);
    videoSetRecoveryLayout->addWidget(videoSetRecoveryOutputBrowse, 1, 2);
    videoSetRecoveryLayout->addWidget(videoSetScanButton, 2, 0);
    videoSetRecoveryLayout->addWidget(videoSetRecoverButton, 2, 1);
    videoSetRecoveryLayout->addWidget(videoSetRecoverResumeButton, 2, 2);
    videoSetLayout->addWidget(videoSetRecoveryGroup);
    videoSetLog = new QTextEdit();
    videoSetLog->setReadOnly(true);
    videoSetLayout->addWidget(videoSetLog);
    mainTabs->addTab(videoSetPage, "Video Set Assistant");

    videoSetProcess = new QProcess(this);
    videoSetProcess->setProcessChannelMode(QProcess::SeparateChannels);
    connect(videoSetProcess, &QProcess::readyReadStandardOutput, this, [this]() {
        const QString text = QString::fromUtf8(videoSetProcess->readAllStandardOutput());
        videoSetLog->moveCursor(QTextCursor::End);
        videoSetLog->insertPlainText(text);
        handleVideoSetOutput(text);
        const QRegularExpression expression(
            R"(P(\d+): offset=(\d+) bytes=(\d+) frames=(\d+) duration=([0-9.]+)s)");
        auto matches = expression.globalMatch(text);
        while (matches.hasNext()) {
            const auto match = matches.next();
            const int row = videoSetPlanTable->rowCount();
            videoSetPlanTable->insertRow(row);
            for (int column = 0; column < 5; ++column)
                videoSetPlanTable->setItem(
                    row, column, new QTableWidgetItem(match.captured(column + 1)));
            videoSetPlanTable->setItem(row, 5, new QTableWidgetItem("Planned"));
        }
    });
    connect(videoSetProcess, &QProcess::readyReadStandardError, this, [this]() {
        handleVideoSetProgressOutput(QString::fromUtf8(
            videoSetProcess->readAllStandardError()));
    });
    connect(videoSetProcess,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this](const int code, QProcess::ExitStatus) {
                const QString remainingOutput = QString::fromUtf8(
                    videoSetProcess->readAllStandardOutput());
                if (!remainingOutput.isEmpty()) {
                    videoSetLog->moveCursor(QTextCursor::End);
                    videoSetLog->insertPlainText(remainingOutput);
                    handleVideoSetOutput(remainingOutput);
                }
                const QString remainingProgress = QString::fromUtf8(
                    videoSetProcess->readAllStandardError());
                if (!remainingProgress.isEmpty())
                    handleVideoSetProgressOutput(remainingProgress);
                videoSetCancelButton->setEnabled(false);
                videoSetPlanButton->setEnabled(true);
                videoSetEncodeButton->setEnabled(true);
                videoSetResumeButton->setEnabled(true);
                videoSetScanButton->setEnabled(true);
                videoSetRecoverButton->setEnabled(true);
                videoSetRecoverResumeButton->setEnabled(true);
                videoSetProgress->setRange(0, 100);
                videoSetProgress->setValue(code == 0 ? 100 : 0);
                videoSetProgress->setFormat(
                    code == 0 ? "Video Set operation complete" :
                                "Video Set operation stopped");
                videoSetLog->append(code == 0 ? "Completed." :
                    QString("Stopped with exit code %1.").arg(code));
                handleVideoSetFinished(code, videoSetProcess->exitStatus());
            });
    connect(videoSetProcess, &QProcess::errorOccurred,
            this, [this](const QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart) return;
        videoSetCancelButton->setEnabled(false);
        videoSetPlanButton->setEnabled(true);
        videoSetEncodeButton->setEnabled(true);
        videoSetResumeButton->setEnabled(true);
        videoSetScanButton->setEnabled(true);
        videoSetRecoverButton->setEnabled(true);
        videoSetRecoverResumeButton->setEnabled(true);
        videoSetAssistantCancelButton->setEnabled(false);
        videoSetAssistantScanButton->setEnabled(true);
        videoSetLog->append(
            "The VidStoreX backend could not be started: " +
            videoSetProcess->errorString());
        if (videoSetAssistantOperation) {
            const auto id = videoSetOperationProgress.view().operation_id;
            (void) videoSetOperationProgress.fail(
                id, QDateTime::currentMSecsSinceEpoch(), -1,
                "The VidStoreX backend could not be started.",
                "Keep the media_storage executable beside the GUI, then retry.");
            videoSetWorkflow.fail(
                "The VidStoreX backend could not be started.",
                "Keep the media_storage executable beside the GUI, then retry.");
            updateVideoSetAssistant();
            renderVideoSetActivity();
        }
    });
    const auto launchVideoSet = [this](QStringList arguments) {
        if (videoSetProcess->state() != QProcess::NotRunning) return;
        videoSetAssistantOperation = false;
        videoSetActiveCommand = arguments.value(0);
        videoSetProcessBuffer.clear();
        videoSetCancelRequested = false;
        videoSetLog->clear();
        videoSetPlanTable->setRowCount(0);
        videoSetProgress->setRange(0, 0);
        videoSetProgress->setFormat("Planning / hashing / encode-decode verification...");
        videoSetCancelButton->setEnabled(true);
        videoSetPlanButton->setEnabled(false);
        videoSetEncodeButton->setEnabled(false);
        videoSetResumeButton->setEnabled(false);
        videoSetScanButton->setEnabled(false);
        videoSetRecoverButton->setEnabled(false);
        videoSetRecoverResumeButton->setEnabled(false);
#ifdef Q_OS_WIN
        const QString executable = QCoreApplication::applicationDirPath() + "/media_storage.exe";
#else
        const QString executable = QCoreApplication::applicationDirPath() + "/media_storage";
#endif
        videoSetProcess->start(executable, arguments);
    };
    const auto encodeArguments = [this](const QString &command) {
        return QStringList{command, videoSetInputEdit->text(), videoSetOutputEdit->text(),
            "--reliability-profile", videoSetProfileCombo->currentData().toString(),
            "--target-duration-seconds", QString::number(videoSetTargetSpin->value()),
            "--max-video-size-mib", QString::number(videoSetMaximumSizeSpin->value()),
            "--reserve-percent", QString::number(videoSetReserveSpin->value())};
    };
    connect(videoSetPlanButton, &QPushButton::clicked, this,
        [launchVideoSet, encodeArguments]() { launchVideoSet(encodeArguments("set-plan")); });
    connect(videoSetEncodeButton, &QPushButton::clicked, this,
        [launchVideoSet, encodeArguments]() { launchVideoSet(encodeArguments("set-encode")); });
    connect(videoSetResumeButton, &QPushButton::clicked, this,
        [launchVideoSet, encodeArguments]() {
            auto args = encodeArguments("set-encode"); args << "--resume"; launchVideoSet(args);
        });
    connect(videoSetScanButton, &QPushButton::clicked, this, [this, launchVideoSet]() {
        launchVideoSet({"set-inspect", videoSetRecoveryInputEdit->text()});
    });
    connect(videoSetRecoverButton, &QPushButton::clicked, this, [this, launchVideoSet]() {
        launchVideoSet({"set-recover", videoSetRecoveryInputEdit->text(),
                        videoSetRecoveryOutputEdit->text()});
    });
    connect(videoSetRecoverResumeButton, &QPushButton::clicked, this, [this, launchVideoSet]() {
        launchVideoSet({"set-recover", videoSetRecoveryInputEdit->text(),
                        videoSetRecoveryOutputEdit->text(), "--resume"});
    });
    connect(videoSetCancelButton, &QPushButton::clicked, this, [this]() {
        if (!videoSetProcess ||
            videoSetProcess->state() == QProcess::NotRunning) return;
        if (QMessageBox::question(
                this, "Pause Video Set operation",
                "Stop the current operation safely? Completed verified parts "
                "will be kept so you can continue later.",
                QMessageBox::Yes | QMessageBox::No,
                QMessageBox::No) != QMessageBox::Yes) return;
        videoSetCancelRequested = true;
        videoSetProcess->terminate();
        QTimer::singleShot(3000, videoSetProcess, [this]() {
            if (videoSetProcess &&
                videoSetProcess->state() != QProcess::NotRunning)
                videoSetProcess->kill();
        });
    });
    connect(videoSetInputBrowse, &QPushButton::clicked, this, [this]() {
        const auto file = QFileDialog::getOpenFileName(this, "Select Video Set source");
        if (!file.isEmpty()) videoSetInputEdit->setText(file);
    });
    connect(videoSetOutputBrowse, &QPushButton::clicked, this, [this]() {
        const auto folder = QFileDialog::getExistingDirectory(this, "Select Video Set output root");
        if (!folder.isEmpty()) videoSetOutputEdit->setText(folder);
    });
    connect(videoSetRecoveryBrowse, &QPushButton::clicked, this, [this]() {
        const auto folder = QFileDialog::getExistingDirectory(this, "Select returned video folder");
        if (!folder.isEmpty()) videoSetRecoveryInputEdit->setText(folder);
    });
    connect(videoSetRecoveryOutputBrowse, &QPushButton::clicked, this, [this]() {
        const auto folder = QFileDialog::getExistingDirectory(this, "Select recovery output folder");
        if (!folder.isEmpty()) videoSetRecoveryOutputEdit->setText(folder);
    });

    setupVideoSetAssistant(videoSetEncodeGroup, videoSetRecoveryGroup);

    auto *testLabPage = new QWidget();
    testLabPage->setObjectName("testLabPage");
    auto *testLabLayout = new QVBoxLayout(testLabPage);
    testLabLayout->setContentsMargins(24, 20, 24, 24);
    auto *testLabHeading = new QLabel("Advanced / YouTube Test Lab");
    testLabHeading->setObjectName("testLabPageHeading");
    testLabHeading->setProperty("pageTitle", true);
    testLabHeading->setProperty("i18nSource",
        "Advanced / YouTube Test Lab");
    auto *testLabDescription = new QLabel(
        "Technical tools, experiments and low-level controls.");
    testLabDescription->setObjectName("testLabPageDescription");
    testLabDescription->setProperty("muted", true);
    testLabDescription->setProperty("i18nSource",
        "Technical tools, experiments and low-level controls.");
    testLabLayout->addWidget(testLabHeading);
    testLabLayout->addWidget(testLabDescription);
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
    capacityPage->setObjectName("capacityLabPage");
    auto *capacityLayout = new QVBoxLayout(capacityPage);
    capacityLayout->setContentsMargins(24, 20, 24, 24);
    auto *capacityHeading = new QLabel("Advanced / Capacity Lab");
    capacityHeading->setObjectName("capacityLabPageHeading");
    capacityHeading->setProperty("pageTitle", true);
    capacityHeading->setProperty("i18nSource", "Advanced / Capacity Lab");
    auto *capacityDescription = new QLabel(
        "Technical tools, experiments and low-level controls.");
    capacityDescription->setObjectName("capacityLabPageDescription");
    capacityDescription->setProperty("muted", true);
    capacityDescription->setProperty("i18nSource",
        "Technical tools, experiments and low-level controls.");
    capacityLayout->addWidget(capacityHeading);
    capacityLayout->addWidget(capacityDescription);
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
        {"Smoke", "Staged Sweep", "YouTube Boundary 1080p",
         "YouTube 1-bit Verification", "1-bit Stress Validation",
         "Custom"});
    capacityOutputEdit = new QLineEdit();
    capacityOutputEdit->setPlaceholderText(
        "Experiment parent output directory");
    auto *capacityOutputBrowse = new QPushButton("Browse...");
    capacityManifestEdit = new QLineEdit();
    capacityManifestEdit->setPlaceholderText(
        "Capacity manifest.json");
    auto *capacityManifestBrowse = new QPushButton("Manifest...");
    capacitySourceManifestEdit = new QLineEdit();
    capacitySourceManifestEdit->setPlaceholderText(
        "Required source Boundary or verified 1-bit manifest.json");
    auto *capacitySourceBrowse =
        new QPushButton("Select Source Evidence Manifest");
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
    capacityIncludeSimulationFailuresCheck =
        new QCheckBox(
            "Include simulation failures in boundary test");
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
    capacityGrid->addWidget(new QLabel("Source Boundary Manifest:"), 2, 0);
    capacityGrid->addWidget(capacitySourceManifestEdit, 2, 1, 1, 3);
    capacityGrid->addWidget(capacitySourceBrowse, 2, 4);
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
    capacityGrid->addWidget(
        capacityIncludeSimulationFailuresCheck, 7, 2, 1, 2);
    capacityGrid->addWidget(capacityEstimateLabel, 8, 0, 1, 5);
    capacityGrid->addWidget(capacityEstimateButton, 9, 0);
    capacityGrid->addWidget(capacityStartButton, 9, 1);
    capacityGrid->addWidget(capacityResumeButton, 9, 2);
    capacityGrid->addWidget(capacityCancelButton, 9, 3);
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
    auto *capacityCopyDownloadButton =
        new QPushButton("Copy Batch Download Command");
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
    capacityActionsGrid->addWidget(capacityCopyDownloadButton, 1, 4);
    capacityLayout->addWidget(capacityActions);
    capacityProgress = new QProgressBar();
    capacityProgress->setRange(0, 100);
    capacityLayout->addWidget(capacityProgress);
    capacityResults = new QTableWidget(0, 22);
    capacityResults->setHorizontalHeaderLabels({
        "Case", "Config", "Session", "Payload instance", "Source SHA prefix",
        "Stage", "Block", "Bits", "Signal", "Repair",
        "Resolution", "Useful KiB/s", "Gain", "Candidate",
        "Recovery", "Margin", "BER/SER", "SHA", "Pareto",
        "Local evidence", "Real YouTube", "Overall / Boundary"});
    capacityResults->horizontalHeader()->setSectionResizeMode(
        QHeaderView::ResizeToContents);
    capacityResults->horizontalHeader()->setStretchLastSection(true);
    capacityLayout->addWidget(capacityResults, 1);
    mainTabs->addTab(capacityPage, "Capacity Lab");

    connect(capacityPresetCombo,
            QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](const int index) {
        const bool boundary = index == 2;
        const bool onebit = index == 3;
        const bool stress = index == 4;
        const bool fixedExperiment = boundary || onebit || stress;
        const bool custom = index == 5;
        for (auto *edit : {capacityBlocksEdit, capacityBitsEdit,
                           capacitySignalsEdit, capacityRepairsEdit})
            edit->setEnabled(custom);
        capacityResolutionCombo->setEnabled(custom);
        capacitySimulationCombo->setEnabled(!fixedExperiment);
        capacityMaximumCasesSpin->setEnabled(!fixedExperiment);
        capacityShortlistSpin->setEnabled(!fixedExperiment);
        capacitySourceManifestEdit->setEnabled(onebit || stress);
        capacityIncludeSimulationFailuresCheck->setEnabled(boundary || onebit);
        capacityShortlistButton->setEnabled(!fixedExperiment);
        capacityEstimateButton->setText(
            stress ? "Estimate Stress Validation" :
            onebit ? "Estimate 1-bit Verification" :
            boundary ? "Estimate Boundary Sweep" : "Estimate");
        capacityStartButton->setText(
            stress ? "Generate 9 Stress Videos" :
            onebit ? "Generate 6 Upload Videos" :
            boundary ? "Generate Boundary Videos" : "Start");
        capacityReportButton->setText(
            stress ? "Refresh Stress Report" :
            onebit ? "Refresh 1-bit Report" :
            boundary ? "Refresh Boundary Report"
                     : "Refresh Reports");
        capacityOpenFolderButton->setText(
            boundary ? "Open Boundary Folder"
                     : "Open Experiment Folder");
        capacityEstimateLabel->setText(
            stress
                ? "Nine cases: six independent 4x payload/session stress "
                  "videos plus a shared-payload 3x3 repair 20/35/50 sweep. "
                  "The verified 1-bit manifest is required as G05 control."
                : onebit
                ? "Exactly 6 videos: R00, R01, G04, R02, R03, G05. "
                  "Locked to 1920x1080, 30 FPS, 1-bit, signal 1.00x, "
                  "repair 5%. Source Boundary manifest is required."
                : boundary
                ? "B00-B06: exactly 7 videos, 1920x1080, 30 FPS, "
                  "signal 1.00x. Matrix: 8x8/1-bit r5; 6x6/1-bit "
                  "r2/r5; 8x8/2-bit r2/r5; 6x6/2-bit r5; "
                  "4x4/1-bit r5. Use Estimate Boundary Sweep for "
                  "disk sizing."
                : "Smoke: 12 local cases. Staged: 24 geometry/"
                  "modulation cases, then bounded repair and final "
                  "profiles. Custom is explicit.");
    });
    capacityBlocksEdit->setEnabled(false);
    capacityBitsEdit->setEnabled(false);
    capacitySignalsEdit->setEnabled(false);
    capacityRepairsEdit->setEnabled(false);
    capacityResolutionCombo->setEnabled(false);
    capacityIncludeSimulationFailuresCheck->setEnabled(false);
    capacitySourceManifestEdit->setEnabled(false);
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
    connect(capacitySourceBrowse, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getOpenFileName(
            this, "Select Source Evidence Manifest", {},
            "JSON manifest (manifest.json)");
        if (!path.isEmpty()) capacitySourceManifestEdit->setText(path);
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
                capacityPresetCombo->currentIndex() == 2
                    ? "boundary-1080p" :
                capacityPresetCombo->currentIndex() == 3
                    ? "onebit-verification-1080p" :
                capacityPresetCombo->currentIndex() == 4
                    ? "onebit-stress-1080p" :
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
            if (preset == "onebit-verification-1080p" ||
                preset == "onebit-stress-1080p")
                args << "--source-manifest"
                     << capacitySourceManifestEdit->text();
            if (capacityEstimateOnlyCheck->isChecked())
                args << "--estimate-only";
            if ((preset == "boundary-1080p" ||
                 preset == "onebit-verification-1080p") &&
                capacityIncludeSimulationFailuresCheck->isChecked())
                args << "--include-simulation-failures";
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
            "--session-label",
            capacityPresetCombo->currentIndex() == 2
                ? "Boundary initial YouTube test"
                : capacityPresetCombo->currentIndex() == 3
                    ? "1-bit verification retest"
                : capacityPresetCombo->currentIndex() == 4
                    ? "1-bit stress validation"
                : "Initial YouTube test"});
    });
    connect(capacityReportButton, &QPushButton::clicked, this, [this] {
        if (capacityManifestEdit->text().isEmpty()) return;
        startTestLabProcess({
            "capacitylab",
            capacityPresetCombo->currentIndex() == 2
                ? "boundary-report"
                : capacityPresetCombo->currentIndex() == 3
                    ? "onebit-report"
                : capacityPresetCombo->currentIndex() == 4
                    ? "stress-report" : "report",
            "--manifest",
            capacityManifestEdit->text(), "--format", "markdown"});
    });
    connect(capacityCopyDownloadButton, &QPushButton::clicked, this, [this] {
        if (capacityManifestEdit->text().isEmpty()) return;
        const QString root = QFileInfo(capacityManifestEdit->text()).absolutePath();
        QApplication::clipboard()->setText(
            "& '" + root + "/tools/download_returned_playlist.ps1' "
            "-PlaylistUrl \"<youtube-playlist-url>\"");
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

    setupSettingsPage();
    setupApplicationNavigation();

    // Main layout
    auto *mainLayout = new QVBoxLayout(centralWidget);
    mainLayout->setContentsMargins(12, 10, 12, 10);
    mainLayout->setSpacing(10);
    mainLayout->addWidget(applicationHeader);
    mainLayout->addWidget(mainTabs, 1);
    mainTabs->tabBar()->hide();
    mainTabs->setCurrentWidget(videoSetPage);
}

void DriveManagerUI::setupSettingsPage() {
    settingsPage = new QWidget();
    settingsPage->setObjectName("settingsPage");
    auto *layout = new QVBoxLayout(settingsPage);
    layout->setContentsMargins(24, 18, 24, 18);
    layout->setSpacing(14);

    settingsHeadingLabel = new QLabel();
    settingsHeadingLabel->setObjectName("settingsHeading");
    settingsHeadingLabel->setProperty("pageTitle", true);
    layout->addWidget(settingsHeadingLabel);
    settingsDescriptionLabel = new QLabel();
    settingsDescriptionLabel->setWordWrap(true);
    layout->addWidget(settingsDescriptionLabel);

    auto *card = new QFrame();
    card->setObjectName("settingsCard");
    card->setProperty("vsxSurface", "raised");
    auto *form = new QGridLayout(card);
    form->setContentsMargins(20, 18, 20, 20);
    form->setHorizontalSpacing(14);
    form->setVerticalSpacing(10);
    form->setColumnStretch(1, 1);
    const auto section = [](const QString &source, const QString &name) {
        auto *label = new QLabel(source);
        label->setObjectName(name);
        label->setProperty("sectionTitle", true);
        label->setProperty("i18nSource", source);
        return label;
    };
    auto *generalSection = section("General", "settingsGeneralSection");
    auto *languageSection = section("Language", "settingsLanguageSection");
    auto *storageSection = section("Storage", "settingsStorageSection");
    auto *advancedSection = section("Advanced", "settingsAdvancedSection");
    settingsLanguageLabel = new QLabel();
    settingsLanguageCombo = new QComboBox();
    settingsLanguageCombo->setObjectName("settingsLanguageCombo");
    settingsLanguageCombo->setAccessibleName("User interface language");
    settingsLanguageCombo->addItem(QStringLiteral("English"),
                           QStringLiteral("en"));
    settingsLanguageCombo->addItem(QString::fromUtf8("Türkçe"),
                           QStringLiteral("tr"));
    settingsOutputLabel = new QLabel();
    settingsOutputEdit = new QLineEdit();
    settingsOutputEdit->setObjectName("defaultVideoSetOutputFolder");
    settingsOutputEdit->setAccessibleName(
        "Default Video Set output folder");
    settingsOutputBrowseButton = new QPushButton();
    settingsOutputBrowseButton->setObjectName(
        "defaultVideoSetOutputBrowse");
    settingsOutputBrowseButton->setIcon(
        style()->standardIcon(QStyle::SP_DirOpenIcon));
    rememberRecentCheckBox = new QCheckBox();
    rememberRecentCheckBox->setObjectName("rememberRecentVideoSets");
    showAdvancedToolsCheckBox = new QCheckBox();
    showAdvancedToolsCheckBox->setObjectName("showAdvancedTools");
    auto *generalDescription = new QLabel(
        "Changes apply immediately and are saved for the next launch.");
    generalDescription->setObjectName("settingsGeneralDescription");
    generalDescription->setProperty("muted", true);
    generalDescription->setProperty("i18nSource",
        "Changes apply immediately and are saved for the next launch.");
    form->addWidget(generalSection, 0, 0, 1, 3);
    form->addWidget(generalDescription, 1, 0, 1, 3);
    form->addWidget(languageSection, 2, 0, 1, 3);
    form->addWidget(settingsLanguageLabel, 3, 0);
    form->addWidget(settingsLanguageCombo, 3, 1, 1, 2);
    form->addWidget(storageSection, 4, 0, 1, 3);
    form->addWidget(settingsOutputLabel, 5, 0);
    form->addWidget(settingsOutputEdit, 5, 1);
    form->addWidget(settingsOutputBrowseButton, 5, 2);
    form->addWidget(rememberRecentCheckBox, 6, 0, 1, 3);
    form->addWidget(advancedSection, 7, 0, 1, 3);
    form->addWidget(showAdvancedToolsCheckBox, 8, 0, 1, 3);
    layout->addWidget(card);
    layout->addStretch();

    auto *youtubeScroll = new QScrollArea();
    youtubeScroll->setObjectName("experimentalYouTubeSyncPage");
    youtubeScroll->setWidgetResizable(true);
    youtubeScroll->setFrameShape(QFrame::NoFrame);
    youtubeSyncPage = youtubeScroll;
    auto *youtubeContent = new QWidget();
    youtubeContent->setObjectName("experimentalYouTubeSyncContent");
    auto *youtubeLayout = new QVBoxLayout(youtubeContent);
    youtubeLayout->setContentsMargins(24, 18, 24, 24);
    youtubeLayout->setSpacing(14);
    auto *youtubeHeading = new QLabel("Advanced / Experimental / YouTube Sync");
    youtubeHeading->setObjectName("experimentalYouTubeSyncHeading");
    youtubeHeading->setProperty("pageTitle", true);
    youtubeHeading->setProperty("i18nSource",
        "Advanced / Experimental / YouTube Sync");
    youtubeLayout->addWidget(youtubeHeading);
    auto *experimentalBadge = new QLabel("Experimental");
    experimentalBadge->setObjectName("experimentalYouTubeSyncBadge");
    experimentalBadge->setProperty("vsxRole", "badge");
    experimentalBadge->setProperty("i18nSource", "Experimental");
    youtubeLayout->addWidget(experimentalBadge, 0, Qt::AlignLeft);
    auto *youtubeWarning = new QLabel(
        "<b>Experimental feature</b><br>"
        "YouTube Sync requires a Google Cloud project and OAuth configuration. "
        "It is not required for normal VidStoreX use. Most users should upload "
        "Video Set videos manually and use the playlist link for recovery.");
    youtubeWarning->setObjectName("experimentalYouTubeSyncWarning");
    youtubeWarning->setWordWrap(true);
    youtubeWarning->setProperty("vsxSurface", "raised");
    youtubeWarning->setProperty("i18nSource",
        "Experimental feature\n\nYouTube Sync requires a Google Cloud project and OAuth configuration. It is not required for normal VidStoreX use. Most users should upload Video Set videos manually and use the playlist link for recovery.");
    youtubeLayout->addWidget(youtubeWarning);

    auto *youtubeCard = new QFrame();
    youtubeCard->setObjectName("experimentalYouTubeSyncSettingsCard");
    youtubeCard->setProperty("vsxSurface", "raised");
    auto *youtubeForm = new QGridLayout(youtubeCard);
    youtubeForm->setContentsMargins(20, 18, 20, 20);
    youtubeForm->setHorizontalSpacing(14);
    youtubeForm->setVerticalSpacing(10);
    youtubeForm->setColumnStretch(1, 1);
    auto *youtubeSection = section("YouTube Sync (Experimental)",
                                   "experimentalYouTubeSyncSection");
    youtubeSyncConnectionLabel = new QLabel();
    youtubeSyncConnectionLabel->setObjectName("youtubeSyncConnection");
    youtubeSyncApiStatusLabel = new QLabel();
    youtubeSyncApiStatusLabel->setObjectName("youtubeSyncApiStatus");
    youtubeSyncChannelLabel = new QLabel();
    youtubeSyncChannelLabel->setObjectName("youtubeSyncChannel");
    youtubeOAuthConfigEdit = new QLineEdit();
    youtubeOAuthConfigEdit->setObjectName("youtubeOAuthConfigPath");
    youtubeOAuthConfigEdit->setPlaceholderText(
        tr("Local Desktop OAuth client JSON (not stored in Git)"));
    youtubeOAuthConfigEdit->setProperty("i18nPlaceholder",
        "Local Desktop OAuth client JSON (not stored in Git)");
    youtubeOAuthConfigBrowseButton = new QPushButton(tr("Choose..."));
    youtubeOAuthConfigBrowseButton->setObjectName("youtubeOAuthConfigBrowse");
    youtubeConnectButton = new QPushButton(tr("Connect YouTube"));
    youtubeConnectButton->setObjectName("youtubeConnectButton");
    youtubeDisconnectButton = new QPushButton(tr("Disconnect"));
    youtubeDisconnectButton->setObjectName("youtubeDisconnectButton");
    youtubeSetupInstructionsButton = new QPushButton(tr("Open setup instructions"));
    youtubeSetupInstructionsButton->setObjectName("youtubeSetupInstructions");
    youtubeDefaultPrivacyCombo = new QComboBox();
    youtubeDefaultPrivacyCombo->setObjectName("youtubeDefaultPrivacy");
    youtubeDefaultPrivacyCombo->addItem(tr("Unlisted"), "unlisted");
    youtubeDefaultPrivacyCombo->addItem(tr("Private"), "private");
    youtubeDefaultPrivacyCombo->addItem(tr("Public"), "public");
    youtubePrivacyTitlesCheckBox = new QCheckBox(tr("Privacy-friendly titles"));
    youtubePrivacyTitlesCheckBox->setObjectName("youtubePrivacyFriendlyTitles");
    youtubeAutoDownloadCheckBox = new QCheckBox(tr(
        "Download and verify after YouTube finishes processing"));
    youtubeAutoDownloadCheckBox->setObjectName("youtubeAutoDownload");
    youtubeForm->addWidget(youtubeSection, 0, 0, 1, 3);
    auto *connectionLabel = new QLabel(tr("Connection:"));
    connectionLabel->setObjectName("experimentalYouTubeConnectionLabel");
    auto *channelLabel = new QLabel(tr("Channel:"));
    channelLabel->setObjectName("experimentalYouTubeChannelLabel");
    auto *apiLabel = new QLabel(tr("API configuration:"));
    apiLabel->setObjectName("experimentalYouTubeApiLabel");
    auto *oauthLabel = new QLabel(tr("OAuth client configuration:"));
    oauthLabel->setObjectName("experimentalYouTubeOAuthLabel");
    auto *uploadDefaultLabel = new QLabel(tr("Upload default:"));
    uploadDefaultLabel->setObjectName("experimentalYouTubePrivacyLabel");
    youtubeForm->addWidget(connectionLabel, 1, 0);
    youtubeForm->addWidget(youtubeSyncConnectionLabel, 1, 1, 1, 2);
    youtubeForm->addWidget(channelLabel, 2, 0);
    youtubeForm->addWidget(youtubeSyncChannelLabel, 2, 1, 1, 2);
    youtubeForm->addWidget(apiLabel, 3, 0);
    youtubeForm->addWidget(youtubeSyncApiStatusLabel, 3, 1, 1, 2);
    youtubeForm->addWidget(oauthLabel, 4, 0);
    youtubeForm->addWidget(youtubeOAuthConfigEdit, 4, 1);
    youtubeForm->addWidget(youtubeOAuthConfigBrowseButton, 4, 2);
    youtubeForm->addWidget(uploadDefaultLabel, 5, 0);
    youtubeForm->addWidget(youtubeDefaultPrivacyCombo, 5, 1, 1, 2);
    youtubeForm->addWidget(youtubePrivacyTitlesCheckBox, 6, 0, 1, 3);
    youtubeForm->addWidget(youtubeAutoDownloadCheckBox, 7, 0, 1, 3);
    auto *youtubeActions = new QHBoxLayout();
    youtubeActions->addWidget(youtubeConnectButton);
    youtubeActions->addWidget(youtubeDisconnectButton);
    youtubeActions->addWidget(youtubeSetupInstructionsButton);
    youtubeActions->addStretch();
    youtubeForm->addLayout(youtubeActions, 8, 0, 1, 3);
    youtubeLayout->addWidget(youtubeCard);
    youtubeLayout->addWidget(youtubeSyncOperationCard);
    youtubeLayout->addStretch();
    youtubeScroll->setWidget(youtubeContent);

    QSettings settings;
    settingsOutputEdit->setText(settings.value(
        "ui/defaultVideoSetOutputFolder",
        videoSetAssistantOutputEdit->text()).toString());
    rememberRecentCheckBox->setChecked(settings.value(
        "ui/rememberRecentSets", true).toBool());
    showAdvancedToolsCheckBox->setChecked(settings.value(
        "ui/showAdvancedTools", true).toBool());
    youtubeOAuthConfigEdit->setText(settings.value(
        "youtube/oauthClientConfigPath").toString());
    youtubePrivacyTitlesCheckBox->setChecked(settings.value(
        "youtube/privacyFriendlyTitles", true).toBool());
    youtubeAutoDownloadCheckBox->setChecked(settings.value(
        "youtube/autoDownload", true).toBool());
    const int privacyIndex = youtubeDefaultPrivacyCombo->findData(
        settings.value("youtube/defaultPrivacy", "unlisted"));
    youtubeDefaultPrivacyCombo->setCurrentIndex((std::max)(0, privacyIndex));
    const auto refreshYouTubeSettings = [this]() {
        const QSettings current;
        const bool configured = !youtubeOAuthConfigEdit->text().isEmpty() &&
            QFileInfo::exists(youtubeOAuthConfigEdit->text());
        const bool connected = current.value(
            "youtube/connected", false).toBool();
        youtubeSyncApiStatusLabel->setText(configured
            ? tr("Ready") : tr("Missing — YouTube Sync is not configured for this build."));
        youtubeSyncConnectionLabel->setText(connected
            ? tr("Connected") : tr("Not connected"));
        youtubeSyncChannelLabel->setText(connected
            ? current.value("youtube/channelTitle",
                tr("Connected account")).toString()
            : tr("—"));
        youtubeConnectButton->setEnabled(configured && !connected);
        youtubeDisconnectButton->setEnabled(connected);
    };
    refreshYouTubeSettings();

    mainTabs->addTab(settingsPage, QStringLiteral("Settings"));
    mainTabs->addTab(youtubeSyncPage,
                     QStringLiteral("YouTube Sync (Experimental)"));

    connect(settingsLanguageCombo,
            QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](const int index) {
        setUiLanguage(settingsLanguageCombo->itemData(index).toString());
    });
    connect(settingsOutputBrowseButton, &QPushButton::clicked,
            this, [this]() {
        const QString folder = QFileDialog::getExistingDirectory(
            this, tr("Choose the default Video Set output folder"),
            settingsOutputEdit->text());
        if (!folder.isEmpty()) settingsOutputEdit->setText(folder);
    });
    connect(settingsOutputEdit, &QLineEdit::textChanged,
            this, [this](const QString &folder) {
        QSettings().setValue("ui/defaultVideoSetOutputFolder", folder);
        if (videoSetAssistantOutputEdit &&
            videoSetAssistantInputEdit->text().isEmpty())
            videoSetAssistantOutputEdit->setText(folder);
    });
    connect(rememberRecentCheckBox, &QCheckBox::toggled,
            this, [this](const bool checked) {
        QSettings().setValue("ui/rememberRecentSets", checked);
        refreshRecentVideoSets();
    });
    connect(showAdvancedToolsCheckBox, &QCheckBox::toggled,
            this, [this](const bool checked) {
        QSettings().setValue("ui/showAdvancedTools", checked);
        if (advancedNavigationButton)
            advancedNavigationButton->setVisible(checked);
    });
    connect(youtubeOAuthConfigBrowseButton, &QPushButton::clicked,
            this, [this, refreshYouTubeSettings]() {
        const QString file = QFileDialog::getOpenFileName(
            this, tr("Choose Desktop OAuth client configuration"),
            QFileInfo(youtubeOAuthConfigEdit->text()).absolutePath(),
            tr("JSON files (*.json);;All files (*)"));
        if (file.isEmpty()) return;
        youtubeOAuthConfigEdit->setText(file);
        QSettings().setValue("youtube/oauthClientConfigPath", file);
        refreshYouTubeSettings();
    });
    connect(youtubeOAuthConfigEdit, &QLineEdit::editingFinished,
            this, [this, refreshYouTubeSettings]() {
        QSettings().setValue("youtube/oauthClientConfigPath",
                             youtubeOAuthConfigEdit->text());
        refreshYouTubeSettings();
    });
    connect(youtubePrivacyTitlesCheckBox, &QCheckBox::toggled,
            this, [](const bool checked) {
        QSettings().setValue("youtube/privacyFriendlyTitles", checked);
    });
    connect(youtubeAutoDownloadCheckBox, &QCheckBox::toggled,
            this, [](const bool checked) {
        QSettings().setValue("youtube/autoDownload", checked);
    });
    connect(youtubeDefaultPrivacyCombo,
            QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](const int index) {
        QSettings().setValue("youtube/defaultPrivacy",
            youtubeDefaultPrivacyCombo->itemData(index));
    });
    youtubeNetworkService = new youtube_sync::YouTubeNetworkService(this);
    connect(youtubeConnectButton, &QPushButton::clicked,
            this, [this]() {
        try {
            const auto config = youtube_sync::read_oauth_client_config(
                std::filesystem::path(youtubeOAuthConfigEdit->text().toStdWString()));
            if (!config.configured()) {
                youtubeSyncApiStatusLabel->setText(tr(
                    "The selected OAuth client configuration is invalid."));
                return;
            }
            youtubeSyncConnectionLabel->setText(tr(
                "VidStoreX needs permission to upload Video Set videos and manage the playlist created for this set."));
            if (!youtubeNetworkService->beginAuthorization(config))
                youtubeSyncConnectionLabel->setText(tr(
                    "The system browser or local callback listener could not be opened."));
        } catch (const std::exception &error) {
            youtubeSyncConnectionLabel->setText(
                QString::fromUtf8(error.what()));
        }
    });
    connect(youtubeNetworkService,
            &youtube_sync::YouTubeNetworkService::tokenResponseReady,
            this, [this, refreshYouTubeSettings](const QByteArray json) {
        try {
            auto token = youtube_sync::parse_token_record(json.toStdString());
            auto store = youtube_sync::make_platform_credential_store();
            if (token.refresh_token.empty()) {
                if (const auto existing = store->load("youtube-oauth")) {
                    try {
                        token.refresh_token = youtube_sync::parse_token_record(
                            *existing).refresh_token;
                    } catch (...) {}
                }
            }
            store->save("youtube-oauth", youtube_sync::serialize_token_record(token));
            QSettings settings;
            settings.setValue("youtube/connected", true);
            // Tokens and authorization codes are intentionally never written
            // to QSettings or logs.
            youtubeAwaitingChannel = true;
            youtube_sync::HttpRequest request{
                "GET",
                "https://www.googleapis.com/youtube/v3/channels?part=snippet&mine=true",
                {{"Authorization", "Bearer " + token.access_token}}, {}};
            youtubeNetworkService->execute(request);
            refreshYouTubeSettings();
            if (youtubePendingSyncAfterRefresh) {
                youtubePendingSyncAfterRefresh = false;
                QTimer::singleShot(0, this,
                    &DriveManagerUI::startYouTubeSync);
            }
        } catch (const std::exception &error) {
            youtubeSyncConnectionLabel->setText(tr(
                "The OAuth token response was invalid: %1")
                .arg(QString::fromUtf8(error.what())));
        }
    });
    connect(youtubeNetworkService,
            &youtube_sync::YouTubeNetworkService::responseReady,
            this, [this, refreshYouTubeSettings](const int status,
                const QByteArray body,
                const QList<QPair<QByteArray, QByteArray>> &) {
        if (!youtubeAwaitingChannel) return;
        youtubeAwaitingChannel = false;
        if (status >= 200 && status < 300) {
            const auto document = QJsonDocument::fromJson(body);
            const auto items = document.object().value("items").toArray();
            if (!items.isEmpty()) {
                const auto item = items.at(0).toObject();
                QSettings settings;
                settings.setValue("youtube/channelId", item.value("id").toString());
                settings.setValue("youtube/channelTitle",
                    item.value("snippet").toObject().value("title").toString());
            }
        }
        refreshYouTubeSettings();
    });
    connect(youtubeNetworkService,
            &youtube_sync::YouTubeNetworkService::responseReady,
            this, [this](const int status, const QByteArray body,
                const QList<QPair<QByteArray, QByteArray>> headers) {
        if (!youtubeSyncOperation.isEmpty())
            handleYouTubeSyncResponse(status, body, headers);
    });
    connect(youtubeDisconnectButton, &QPushButton::clicked,
            this, [this, refreshYouTubeSettings]() {
        try {
            auto store = youtube_sync::make_platform_credential_store();
            store->remove("youtube-oauth");
        } catch (...) {}
        QSettings settings;
        settings.remove("youtube/connected");
        settings.remove("youtube/channelId");
        settings.remove("youtube/channelTitle");
        refreshYouTubeSettings();
    });
    connect(youtubeSetupInstructionsButton, &QPushButton::clicked,
            this, []() {
        QDesktopServices::openUrl(QUrl::fromLocalFile(
            QDir(QCoreApplication::applicationDirPath())
                .filePath("../docs/YOUTUBE_SYNC_SETUP.md")));
    });
}

void DriveManagerUI::setupApplicationNavigation() {
    applicationHeader = new QFrame();
    applicationHeader->setObjectName("applicationHeader");
    applicationHeader->setProperty("surface", true);
    auto *layout = new QVBoxLayout(applicationHeader);
    layout->setContentsMargins(
        vidstorex_ui::Spacing::Lg, 6,
        vidstorex_ui::Spacing::Lg, vidstorex_ui::Spacing::Xs);
    layout->setSpacing(vidstorex_ui::Spacing::Xs);

    auto *brandRow = new QHBoxLayout();
    brandLabel = new QLabel(QStringLiteral("VidStoreX"));
    brandLabel->setObjectName("brandLabel");
    brandLabel->setProperty("brand", true);
    brandSubtitleLabel = new QLabel();
    brandSubtitleLabel->setObjectName("brandSubtitle");
    brandSubtitleLabel->setWordWrap(false);
    brandSubtitleLabel->setProperty("muted", true);
    brandRow->addWidget(brandLabel);
    auto *brandRail = new VidStoreXSignalRail();
    brandRail->setObjectName("brandSignalRail");
    brandRail->setFixedWidth(84);
    brandRow->addWidget(brandRail);
    brandRow->addWidget(brandSubtitleLabel, 1);
    layout->addLayout(brandRow);

    auto *navigation = new QHBoxLayout();
    homeNavigationButton = new QPushButton();
    homeNavigationButton->setObjectName("homeNavigationButton");
    createNavigationButton = new QPushButton();
    createNavigationButton->setObjectName("createNavigationButton");
    recoverNavigationButton = new QPushButton();
    recoverNavigationButton->setObjectName("recoverNavigationButton");
    recentNavigationButton = new QPushButton();
    recentNavigationButton->setObjectName("recentNavigationButton");
    advancedNavigationButton = new QToolButton();
    advancedNavigationButton->setObjectName("advancedNavigationButton");
    advancedNavigationButton->setPopupMode(QToolButton::InstantPopup);
    settingsNavigationButton = new QPushButton();
    settingsNavigationButton->setObjectName("settingsNavigationButton");
    languageCombo = new QComboBox();
    languageCombo->setObjectName("uiLanguageCombo");
    languageCombo->addItem(QStringLiteral("English"), QStringLiteral("en"));
    languageCombo->addItem(QString::fromUtf8("Türkçe"), QStringLiteral("tr"));

    homeNavigationButton->setIcon(
        style()->standardIcon(QStyle::SP_ComputerIcon));
    createNavigationButton->setIcon(
        style()->standardIcon(QStyle::SP_FileIcon));
    recoverNavigationButton->setIcon(
        style()->standardIcon(QStyle::SP_DialogOpenButton));
    recentNavigationButton->setIcon(
        style()->standardIcon(QStyle::SP_FileDialogDetailedView));
    settingsNavigationButton->setIcon(
        style()->standardIcon(QStyle::SP_FileDialogInfoView));

    auto *advancedMenu = new QMenu(advancedNavigationButton);
    auto *storageAction = advancedMenu->addAction(QStringLiteral("Storage"));
    storageAction->setObjectName("advancedStorageAction");
    auto *testLabAction = advancedMenu->addAction(
        QStringLiteral("YouTube Test Lab"));
    testLabAction->setObjectName("advancedTestLabAction");
    auto *capacityAction = advancedMenu->addAction(
        QStringLiteral("Capacity Lab"));
    capacityAction->setObjectName("advancedCapacityLabAction");
    auto *classicAction = advancedMenu->addAction(
        QStringLiteral("Classic Video Set Tools"));
    classicAction->setObjectName("advancedClassicVideoSetAction");
    advancedMenu->addSeparator();
    auto *experimentalHeading = advancedMenu->addAction(
        QStringLiteral("Experimental"));
    experimentalHeading->setObjectName("advancedExperimentalHeading");
    experimentalHeading->setEnabled(false);
    auto *youtubeSyncAction = advancedMenu->addAction(
        QStringLiteral("YouTube Sync (Experimental)"));
    youtubeSyncAction->setObjectName("advancedYouTubeSyncAction");
    advancedNavigationButton->setMenu(advancedMenu);

    navigation->addWidget(homeNavigationButton);
    navigation->addWidget(createNavigationButton);
    navigation->addWidget(recoverNavigationButton);
    navigation->addWidget(recentNavigationButton);
    navigation->addStretch();
    navigation->addWidget(advancedNavigationButton);
    navigation->addWidget(settingsNavigationButton);
    auto *languageLabel = new QLabel();
    languageLabel->setObjectName("headerLanguageLabel");
    languageLabel->setVisible(false);
    navigation->addWidget(languageLabel);
    navigation->addWidget(languageCombo);
    layout->addLayout(navigation);

    connect(homeNavigationButton, &QPushButton::clicked,
            this, &DriveManagerUI::showVideoSetHome);
    connect(createNavigationButton, &QPushButton::clicked,
            this, &DriveManagerUI::showVideoSetCreate);
    connect(recoverNavigationButton, &QPushButton::clicked,
            this, &DriveManagerUI::showVideoSetRecover);
    connect(recentNavigationButton, &QPushButton::clicked, this, [this]() {
        showVideoSetHome();
        videoSetRecentList->setFocus();
        if (videoSetRecentList->count() > 0)
            videoSetRecentList->setCurrentRow(0);
        homeNavigationButton->setProperty("selected", false);
        recentNavigationButton->setProperty("selected", true);
        for (auto *button : {homeNavigationButton,
                             recentNavigationButton}) {
            button->style()->unpolish(button);
            button->style()->polish(button);
        }
    });
    connect(settingsNavigationButton, &QPushButton::clicked,
            this, [this]() { mainTabs->setCurrentWidget(settingsPage); });
    connect(mainTabs, &QTabWidget::currentChanged,
            this, [this]() { updateNavigationVisuals(); });
    connect(videoSetAssistantStack, &QStackedWidget::currentChanged,
            this, [this]() { updateNavigationVisuals(); });
    connect(languageCombo,
            QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](const int index) {
        setUiLanguage(languageCombo->itemData(index).toString());
    });
    connect(storageAction, &QAction::triggered,
            this, [this]() { mainTabs->setCurrentIndex(0); });
    connect(testLabAction, &QAction::triggered,
            this, [this]() { mainTabs->setCurrentIndex(2); });
    connect(capacityAction, &QAction::triggered,
            this, [this]() { mainTabs->setCurrentIndex(3); });
    connect(youtubeSyncAction, &QAction::triggered,
            this, [this]() { mainTabs->setCurrentWidget(youtubeSyncPage); });
    connect(classicAction, &QAction::triggered, this, [this]() {
        mainTabs->setCurrentWidget(videoSetPage);
        videoSetClassicToolsGroup->setVisible(true);
        videoSetClassicToolsGroup->setChecked(true);
        videoSetClassicToolsGroup->setFocus();
        updateNavigationVisuals();
    });

    advancedNavigationButton->setVisible(
        showAdvancedToolsCheckBox->isChecked());

    for (auto *button : {homeNavigationButton, createNavigationButton,
                         recoverNavigationButton, recentNavigationButton,
                         settingsNavigationButton})
        button->setProperty("nav", true);
    advancedNavigationButton->setProperty("nav", true);
    applySemanticVisualRoles();
    vidstorex_ui::applyTheme(centralWidget);
    updateNavigationVisuals();

    const int languageIndex = languageCombo->findData(uiLanguage);
    {
        const QSignalBlocker blocker(languageCombo);
        languageCombo->setCurrentIndex(languageIndex >= 0 ? languageIndex : 0);
    }
    {
        const QSignalBlocker blocker(settingsLanguageCombo);
        settingsLanguageCombo->setCurrentIndex(
            languageIndex >= 0 ? languageIndex : 0);
    }
}

void DriveManagerUI::applySemanticVisualRoles() {
    const auto role = [](QWidget *widget, const char *value) {
        if (widget) widget->setProperty("vsxRole", value);
    };
    for (auto *button : {videoSetWelcomeCreateButton,
                         videoSetWelcomeRecoverButton,
                         videoSetSourceContinueButton,
                         videoSetModeContinueButton,
                         videoSetCreateVideosButton,
                         videoSetProgressContinueButton,
                         videoSetProgressResumeButton,
                         videoSetUploadedButton,
                         videoSetDownloadButton,
                         videoSetAssistantRecoverButton,
                         videoSetOpenRecoveredButton,
                         videoSetRecentContinueButton})
        role(button, "primary");
    for (auto *button : {videoSetAssistantInputBrowseButton,
                         videoSetAssistantOutputBrowseButton,
                         videoSetProgressOpenFolderButton,
                         videoSetOpenVideosButton,
                         videoSetOpenYouTubeButton,
                         videoSetOpenChecklistButton,
                         videoSetSelectYtDlpButton,
                         videoSetManualReturnedButton,
                         videoSetAssistantScanButton,
                         videoSetOpenReturnedButton,
                         videoSetRecentOpenFolderButton,
                         videoSetOpenSetFolderButton})
        role(button, "secondary");
    role(videoSetOpenVideosButton, "primary");
    for (auto *button : {videoSetCopyShaButton,
                         videoSetReturnHomeButton,
                         videoSetRecentRemoveButton})
        role(button, "ghost");
    role(videoSetAssistantCancelButton, "danger");
    role(videoSetActivityRetryButton, "secondary");
    role(videoSetCreateCard, "actionCard");
    role(videoSetRecoverCard, "actionCard");
    role(videoSetResilientCard, "profileCard");
    role(videoSetHighCapacityCard, "profileCard");
    role(videoSetRecentGroup, "section");
    if (videoSetTechnicalLogButton)
        videoSetTechnicalLogButton->setProperty("vsxRole", "ghost");
    if (videoSetAdvancedSettingsButton)
        videoSetAdvancedSettingsButton->setProperty("vsxRole", "ghost");
    if (videoSetSuccessDetailsLabel)
        videoSetSuccessDetailsLabel->setProperty("technical", true);
}

void DriveManagerUI::updateNavigationVisuals() {
    if (!mainTabs || !videoSetAssistantStack) return;
    const bool assistant = mainTabs->currentWidget() == videoSetPage;
    const int page = videoSetAssistantStack->currentIndex();
    const auto selected = [](QWidget *widget, const bool value) {
        if (!widget) return;
        widget->setProperty("selected", value);
        widget->style()->unpolish(widget);
        widget->style()->polish(widget);
    };
    selected(homeNavigationButton, assistant && page == 0);
    selected(createNavigationButton, assistant && page >= 1 && page <= 6);
    selected(recoverNavigationButton, assistant && page >= 7);
    selected(recentNavigationButton, false);
    selected(settingsNavigationButton,
             mainTabs->currentWidget() == settingsPage);
    selected(advancedNavigationButton,
             mainTabs->currentIndex() == 0 ||
             mainTabs->currentIndex() == 2 ||
             mainTabs->currentIndex() == 3 ||
             mainTabs->currentWidget() == youtubeSyncPage);
}

void DriveManagerUI::showVideoSetHome() {
    mainTabs->setCurrentWidget(videoSetPage);
    videoSetAssistantStack->setCurrentIndex(0);
    if (videoSetClassicToolsGroup)
        videoSetClassicToolsGroup->setVisible(false);
    refreshRecentVideoSets();
    videoSetStepIndicator->setVisible(false);
    videoSetPrimaryMessage->setVisible(false);
    videoSetSuggestedAction->setVisible(false);
    videoSetActivityPanel->setVisible(false);
    updateNavigationVisuals();
}

void DriveManagerUI::showVideoSetCreate() {
    if (videoSetOperationProgress.view().is_busy) {
        mainTabs->setCurrentWidget(videoSetPage);
        return;
    }
    videoSetWorkflow.reset();
    videoSetWorkflow.choose_create();
    if (videoSetClassicToolsGroup)
        videoSetClassicToolsGroup->setVisible(false);
    videoSetResilientRadio->setChecked(true);
    videoSetAssistantStack->setCurrentIndex(1);
    mainTabs->setCurrentWidget(videoSetPage);
    updateVideoSetAssistant();
    updateNavigationVisuals();
}

void DriveManagerUI::showVideoSetRecover() {
    if (videoSetOperationProgress.view().is_busy) {
        mainTabs->setCurrentWidget(videoSetPage);
        return;
    }
    videoSetWorkflow.reset();
    videoSetWorkflow.choose_recover();
    if (videoSetClassicToolsGroup)
        videoSetClassicToolsGroup->setVisible(false);
    videoSetAssistantStack->setCurrentIndex(7);
    mainTabs->setCurrentWidget(videoSetPage);
    updateVideoSetAssistant();
    updateNavigationVisuals();
}

QString DriveManagerUI::translatedWorkflowText(
    const std::string &english) const {
    if (english.empty()) return {};
    return QCoreApplication::translate("DriveManagerUI", english.c_str());
}

void DriveManagerUI::setUiLanguage(const QString &language,
                                   const bool persist) {
    const QString resolved = vidstorex_ui::resolve_language(
        language, QLocale::system().name());
    const int assistantPage = videoSetAssistantStack
        ? videoSetAssistantStack->currentIndex() : -1;
    QWidget *topPage = mainTabs ? mainTabs->currentWidget() : nullptr;

    if (uiTranslator) qApp->removeTranslator(uiTranslator);
    uiTranslationLoaded = false;
    uiLanguage = resolved;
    if (uiLanguage == QLatin1String(vidstorex_ui::kTurkishLanguage) &&
        uiTranslator) {
        uiTranslationLoaded = uiTranslator->load(
            QStringLiteral(":/i18n/vidstorex_tr.qm"));
        if (uiTranslationLoaded) qApp->installTranslator(uiTranslator);
    }
    setProperty("uiLanguage", uiLanguage);
    setProperty("uiTranslationLoaded", uiTranslationLoaded);
    if (persist) QSettings().setValue("ui/language", uiLanguage);

    if (languageCombo) {
        const QSignalBlocker blocker(languageCombo);
        const int index = languageCombo->findData(uiLanguage);
        languageCombo->setCurrentIndex(index >= 0 ? index : 0);
    }
    if (settingsLanguageCombo) {
        const QSignalBlocker blocker(settingsLanguageCombo);
        const int index = settingsLanguageCombo->findData(uiLanguage);
        settingsLanguageCombo->setCurrentIndex(index >= 0 ? index : 0);
    }
    if (centralWidget) {
        retranslateUserInterface();
        refreshRecentVideoSets();
        updateVideoSetAssistant();
        if (videoSetAssistantStack && assistantPage >= 0) {
            videoSetAssistantStack->setCurrentIndex(assistantPage);
            if (assistantPage == 0) {
                videoSetStepIndicator->setVisible(false);
                videoSetPrimaryMessage->setVisible(false);
                videoSetSuggestedAction->setVisible(false);
                videoSetActivityPanel->setVisible(false);
            }
        }
        if (mainTabs && topPage) mainTabs->setCurrentWidget(topPage);
    }
}

void DriveManagerUI::retranslateUserInterface() {
    setWindowTitle(QStringLiteral("VidStoreX"));
    if (!centralWidget) return;

    const auto translateStored = [this](QObject *root) {
        if (!root) return;
        const auto widgets = root->findChildren<QWidget *>();
        for (auto *widget : widgets) {
            const QString source = widget->property("i18nSource").toString();
            if (!source.isEmpty()) {
                const QString translated = QCoreApplication::translate(
                    "DriveManagerUI", source.toUtf8().constData());
                if (auto *button = qobject_cast<QAbstractButton *>(widget))
                    button->setText(translated);
                else if (auto *label = qobject_cast<QLabel *>(widget))
                    label->setText(translated);
                else if (auto *group = qobject_cast<QGroupBox *>(widget))
                    group->setTitle(translated);
            }
            const QString placeholder =
                widget->property("i18nPlaceholder").toString();
            if (!placeholder.isEmpty()) {
                if (auto *edit = qobject_cast<QLineEdit *>(widget))
                    edit->setPlaceholderText(QCoreApplication::translate(
                        "DriveManagerUI", placeholder.toUtf8().constData()));
            }
        }
    };
    translateStored(centralWidget);

    brandSubtitleLabel->setText(tr(
        "FILE → VIDEO → FILE · DIGITAL ARCHIVE"));
    homeNavigationButton->setText(tr("Home"));
    createNavigationButton->setText(tr("Create"));
    recoverNavigationButton->setText(tr("Recover"));
    recentNavigationButton->setText(tr("Recent"));
    advancedNavigationButton->setText(tr("Advanced"));
    settingsNavigationButton->setText(tr("Settings"));
    homeNavigationButton->setAccessibleName(homeNavigationButton->text());
    homeNavigationButton->setAccessibleDescription(tr(
        "Open the Video Set home screen without clearing current work."));
    createNavigationButton->setAccessibleName(createNavigationButton->text());
    createNavigationButton->setAccessibleDescription(tr(
        "Start a new guided Video Set creation workflow."));
    recoverNavigationButton->setAccessibleName(recoverNavigationButton->text());
    recoverNavigationButton->setAccessibleDescription(tr(
        "Choose videos or a set and recover the original file."));
    recentNavigationButton->setAccessibleName(recentNavigationButton->text());
    advancedNavigationButton->setAccessibleName(
        advancedNavigationButton->text());
    settingsNavigationButton->setAccessibleName(
        settingsNavigationButton->text());
    languageCombo->setAccessibleName(tr("User interface language"));
    settingsLanguageCombo->setAccessibleName(tr("User interface language"));
    if (auto *headerLanguage = applicationHeader->findChild<QLabel *>(
            "headerLanguageLabel"))
        headerLanguage->setVisible(false);

    settingsHeadingLabel->setText(tr("Settings"));
    settingsDescriptionLabel->setText(tr(
        "Manage language, storage, and advanced access. Changes apply immediately."));
    settingsLanguageLabel->setText(tr("Interface language:"));
    settingsOutputLabel->setText(tr("Default output folder:"));
    settingsOutputBrowseButton->setText(tr("Choose folder"));
    rememberRecentCheckBox->setText(tr("Remember recent Video Sets"));
    showAdvancedToolsCheckBox->setText(tr("Show Advanced tools"));
    if (auto *action = applicationHeader->findChild<QAction *>(
            "advancedStorageAction"))
        action->setText(tr("Storage"));
    if (auto *action = applicationHeader->findChild<QAction *>(
            "advancedTestLabAction"))
        action->setText(tr("YouTube Test Lab"));
    if (auto *action = applicationHeader->findChild<QAction *>(
            "advancedCapacityLabAction"))
        action->setText(tr("Capacity Lab"));
    if (auto *action = applicationHeader->findChild<QAction *>(
            "advancedClassicVideoSetAction"))
        action->setText(tr("Classic Video Set Tools"));
    if (auto *action = applicationHeader->findChild<QAction *>(
            "advancedExperimentalHeading"))
        action->setText(tr("Experimental"));
    if (auto *action = applicationHeader->findChild<QAction *>(
            "advancedYouTubeSyncAction"))
        action->setText(tr("YouTube Sync (Experimental)"));
    const auto setExperimentalLabel = [this](const char *name,
                                              const char *source) {
        if (auto *label = youtubeSyncPage->findChild<QLabel *>(name))
            label->setText(tr(source));
    };
    setExperimentalLabel("experimentalYouTubeConnectionLabel", "Connection:");
    setExperimentalLabel("experimentalYouTubeChannelLabel", "Channel:");
    setExperimentalLabel("experimentalYouTubeApiLabel", "API configuration:");
    setExperimentalLabel("experimentalYouTubeOAuthLabel",
                         "OAuth client configuration:");
    setExperimentalLabel("experimentalYouTubePrivacyLabel", "Upload default:");
    youtubeConnectButton->setText(tr("Connect YouTube"));
    youtubeDisconnectButton->setText(tr("Disconnect"));
    youtubeSetupInstructionsButton->setText(tr("Open setup instructions"));
    youtubeOAuthConfigBrowseButton->setText(tr("Choose..."));
    youtubeDefaultPrivacyCombo->setItemText(0, tr("Unlisted"));
    youtubeDefaultPrivacyCombo->setItemText(1, tr("Private"));
    youtubeDefaultPrivacyCombo->setItemText(2, tr("Public"));
    youtubePrivacyTitlesCheckBox->setText(tr("Privacy-friendly titles"));
    youtubeAutoDownloadCheckBox->setText(tr(
        "Download and verify after YouTube finishes processing"));
    const QSettings youtubeSettings;
    const bool youtubeConfigured = !youtubeOAuthConfigEdit->text().isEmpty() &&
        QFileInfo::exists(youtubeOAuthConfigEdit->text());
    const bool youtubeConnected = youtubeSettings.value(
        "youtube/connected", false).toBool();
    youtubeSyncApiStatusLabel->setText(youtubeConfigured
        ? tr("Ready")
        : tr("Missing — YouTube Sync is not configured for this build."));
    youtubeSyncConnectionLabel->setText(youtubeConnected
        ? tr("Connected") : tr("Not connected"));
    youtubeSyncChannelLabel->setText(youtubeConnected
        ? youtubeSettings.value("youtube/channelTitle",
              tr("Connected account")).toString()
        : tr("—"));
    if (auto *menu = menuBar()->findChild<QMenu *>("fileMenu"))
        menu->setTitle(tr("&File"));
    if (auto *menu = menuBar()->findChild<QMenu *>("toolsMenu"))
        menu->setTitle(tr("&Tools"));
    if (auto *menu = menuBar()->findChild<QMenu *>("helpMenu"))
        menu->setTitle(tr("&Help"));
    if (auto *action = menuBar()->findChild<QAction *>("exitAction"))
        action->setText(tr("E&xit"));
    if (auto *action = menuBar()->findChild<QAction *>("clearLogsAction"))
        action->setText(tr("&Clear Logs"));
    if (auto *action = menuBar()->findChild<QAction *>("aboutAction"))
        action->setText(tr("&About"));
    if (permanentStatus) permanentStatus->setText(tr("Ready"));

    videoSetIntroLabel->setText(tr(
        "<b>Video Set Assistant</b><br>Create resilient videos or recover an original file with a guided workflow. Advanced technical controls stay available separately."));
    videoSetValidationLabel->setText(tr(
        "<b>Tested with real YouTube processing</b><br>6/6 single-video exact recoveries and 4/4 Video Set parts with an exact full-file SHA-256. Results describe the tested configuration and are not a guarantee for every future upload."));

    const QStringList headings{
        tr("Store your files safely in videos"),
        tr("Choose your file"),
        tr("Choose a mode"),
        tr("Review and create videos"),
        tr("Create and verify videos"),
        tr("Upload to YouTube"),
        tr("Download YouTube's processed copies"),
        tr("Check parts and recover"),
        tr("Recover and verify the file"),
        tr("Done")};
    const QStringList subtitles{
        tr("Create a resilient Video Set or recover an exact original."),
        tr("Choose the file you want to turn into videos. Any file type is supported; the source is never moved, modified, or deleted."),
        tr("Choose how you want to balance reliability and video count."),
        tr("VidStoreX calculates real packet, frame, repair, and capacity values before creating any video."),
        tr("Each completed part is decoded and checked locally before it is accepted."),
        tr("Upload your VidStoreX videos to YouTube and add them to one playlist."),
        tr("Paste the playlist link, or choose the returned videos manually."),
        tr("Select a set, manifest, video, or returned-video folder. Recovery starts only when you choose Recover."),
        tr("VidStoreX verifies every part and the final full-file SHA-256 before publishing the recovered file."),
        tr("Recovery is successful only when the final SHA-256 matches.")};
    for (int index = 0; index < videoSetAssistantPageHeadings.size() &&
                            index < headings.size(); ++index)
        videoSetAssistantPageHeadings[index]->setText(headings[index]);
    for (int index = 0; index < videoSetAssistantPageSubtitles.size() &&
                            index < subtitles.size(); ++index)
        videoSetAssistantPageSubtitles[index]->setText(subtitles[index]);

    videoSetCreateCardTitle->setText(tr("Create a Video Set"));
    videoSetCreateCardDescription->setText(
        tr("Turn one file into one or more videos."));
    videoSetWelcomeCreateButton->setText(tr("Choose a file"));
    videoSetWelcomeCreateButton->setAccessibleName(
        tr("Create a Video Set"));
    videoSetWelcomeCreateButton->setAccessibleDescription(
        tr("Turn one file into one or more videos."));
    videoSetCreateFlowLabel->setText(tr("SOURCE → VIDEO SET"));
    videoSetRecoverCardTitle->setText(tr("Recover a File"));
    videoSetRecoverCardDescription->setText(tr(
        "Rebuild the original file from downloaded Video Set videos."));
    videoSetWelcomeRecoverButton->setText(tr("Choose videos or set"));
    videoSetWelcomeRecoverButton->setAccessibleName(tr("Recover a File"));
    videoSetWelcomeRecoverButton->setAccessibleDescription(tr(
        "Rebuild the original file from downloaded Video Set videos."));
    videoSetRecoverFlowLabel->setText(tr("VIDEO → FILE"));
    videoSetTrustLabel->setText(tr("✓ Real YouTube tested"));
    videoSetTrustDetailsButton->setText(tr("Details"));
    videoSetRecentTitle->setText(tr("Recent Video Sets"));
    videoSetRecentContinueButton->setText(tr("Continue"));
    videoSetRecentOpenFolderButton->setText(tr("Open Folder"));
    videoSetRecentRemoveButton->setAccessibleName(
        tr("More recent set actions"));
    videoSetRecentEmptyCreateButton->setText(tr("Create your first set"));
    videoSetRecentEmptyRecoverButton->setText(tr("Recover returned videos"));
    videoSetAssistantScanButton->setAccessibleName(tr("Check Videos"));
    videoSetAssistantScanButton->setAccessibleDescription(tr(
        "Inspect embedded Video Set information without rebuilding the original file."));
    videoSetAssistantRecoverButton->setAccessibleName(
        tr("Recover Original File"));
    videoSetAssistantRecoverButton->setAccessibleDescription(tr(
        "Rebuild the original file from all verified parts."));

    videoSetResilientCard->setTitle(tr("Most Reliable"));
    videoSetHighCapacityCard->setTitle(tr("Fewer & Shorter Videos"));
    if (auto *badge = videoSetResilientCard->findChild<QLabel *>(
            "videoSetResilientBadge"))
        badge->setText(tr("Recommended"));
    if (auto *badge = videoSetHighCapacityCard->findChild<QLabel *>(
            "videoSetHighCapacityBadge"))
        badge->setText(tr("Real YouTube tested"));
    videoSetResilientDescription->setText(tr(
        "Uses more video time for the most conservative storage mode.\nTechnical: 8x8, 1-bit, signal 1.0, repair 5%."));
    videoSetHighCapacityDescription->setText(tr(
        "Stores more data in each video and produces fewer or shorter videos.\nReal YouTube tested: 6/6 single-video exact and 4/4 Video Set parts with full-file SHA exact.\nTechnical: 4x4, 1-bit, signal 1.0, repair 5%; config 538F2B009FAB."));
    videoSetResilientRadio->setAccessibleDescription(
        videoSetResilientDescription->text());
    videoSetHighCapacityRadio->setAccessibleDescription(
        videoSetHighCapacityDescription->text());
    videoSetAssistantInputBrowseButton->setText(
        QFileInfo(videoSetAssistantInputEdit->text()).isReadable()
            ? tr("Change") : tr("Choose file"));
    videoSetAdvancedSettingsButton->setText(tr("Advanced Settings"));
    videoSetPartDetailsButton->setText(
        videoSetPartDetailsButton->isChecked()
            ? tr("Hide part details") : tr("Show part details"));
    videoSetTechnicalLogButton->setText(
        videoSetTechnicalLogButton->isChecked()
            ? tr("Hide technical log") : tr("Show technical log"));
    videoSetSuccessLabel->setText(tr("Your file was recovered exactly."));
    videoSetDownloadButton->setText(tr("Download Processed Videos"));
    videoSetOpenRecoveredButton->setText(tr("Open File Location"));
    videoSetOpenSetFolderButton->setText(tr("Open Set Folder"));
    videoSetCopyShaButton->setText(tr("Copy SHA-256"));
    videoSetReturnHomeButton->setText(tr("Return Home"));
    videoSetClassicToolsGroup->setTitle(tr(
        "Advanced / Classic Video Set Tools"));
    if (auto *action = videoSetPage->findChild<QAction *>(
            "videoSetTrustEvidenceAction"))
        action->setText(tr(
            "6/6 single-video exact · 4/4 set parts + full SHA-256 exact"));
    if (auto *action = videoSetPage->findChild<QAction *>(
            "videoSetTrustCaveatAction"))
        action->setText(tr(
            "Measured results for the tested configuration; not an absolute guarantee."));
    if (auto *action = videoSetPage->findChild<QAction *>(
            "recentShowManifestAction"))
        action->setText(tr("Technical details"));
    if (auto *action = videoSetPage->findChild<QAction *>(
            "recentCopyManifestAction"))
        action->setText(tr("Copy manifest location"));
    if (auto *action = videoSetPage->findChild<QAction *>(
            "recentOpenReportAction"))
        action->setText(tr("Open report"));
    if (auto *action = videoSetPage->findChild<QAction *>(
            "recentRemoveAction"))
        action->setText(tr("Remove from list"));
    videoSetAdvancedProfileLabel->setText(
        videoSetHighCapacityRadio->isChecked()
            ? tr("High Capacity: 4x4, 1-bit, signal 1.0, repair 5%, 1920x1080 at 30 FPS; config 538F2B009FAB")
            : tr("Resilient: 8x8, 1-bit, signal 1.0, repair 5%, 1920x1080 at 30 FPS"));
    videoSetAssistantTargetSpin->setSuffix(tr(" seconds"));
    videoSetAssistantMaximumSizeSpin->setSuffix(
        tr(" MiB (0 disables cap)"));
    videoSetAssistantTargetSpin->setToolTip(tr(
        "Preferred duration for each planned video; actual duration follows packet and frame boundaries."));
    videoSetAssistantMaximumSizeSpin->setToolTip(tr(
        "Optional actual-size ceiling. Set 0 to disable the ceiling."));
    videoSetAssistantReserveSpin->setToolTip(tr(
        "Keeps capacity in reserve so estimates do not use the absolute limit."));
    videoSetAdvancedProfileLabel->setToolTip(tr(
        "Repair data adds recoverable packets. The selected profile and config ID remain part of the manifest."));

    const QStringList metricTitles{
        tr("Verified"), tr("Missing"), tr("Corrupt"),
        tr("Duplicates"), tr("Conflicts")};
    for (int index = 0; index < videoSetScanMetricTitles.size() &&
                            index < metricTitles.size(); ++index)
        videoSetScanMetricTitles[index]->setText(metricTitles[index]);

    videoSetAssistantPlanTable->setHorizontalHeaderLabels({
        tr("Part"), tr("Payload bytes"), tr("Frames"),
        tr("Duration"), tr("Estimated size"), tr("Status")});
    updateProfileCardVisuals();
}

void DriveManagerUI::updateProfileCardVisuals() {
    if (!videoSetResilientRadio || !videoSetHighCapacityRadio) return;
    const bool resilient = videoSetResilientRadio->isChecked();
    videoSetResilientCard->setProperty("selected", resilient);
    videoSetHighCapacityCard->setProperty("selected", !resilient);
    videoSetResilientRadio->setText(resilient
        ? tr("✓ Selected — Resilient") : tr("Resilient"));
    videoSetHighCapacityRadio->setText(!resilient
        ? tr("✓ Selected — High Capacity") : tr("High Capacity (opt-in)"));
    for (auto *card : {videoSetResilientCard, videoSetHighCapacityCard}) {
        card->style()->unpolish(card);
        card->style()->polish(card);
        card->update();
    }
}

void DriveManagerUI::setupVideoSetAssistant(
    QGroupBox *classicEncodeGroup,
    QGroupBox *classicRecoveryGroup) {
    auto *root = qobject_cast<QVBoxLayout *>(videoSetPage->layout());
    if (!root) return;

    videoSetIntroLabel->setVisible(false);
    videoSetValidationLabel->setVisible(false);
    videoSetStepIndicator = new VidStoreXStepper();
    root->insertWidget(2, videoSetStepIndicator);

    videoSetPrimaryMessage = new QLabel();
    videoSetPrimaryMessage->setObjectName("videoSetAssistantPrimaryMessage");
    videoSetPrimaryMessage->setWordWrap(true);
    videoSetPrimaryMessage->setProperty("sectionTitle", true);
    root->insertWidget(3, videoSetPrimaryMessage);
    videoSetSuggestedAction = new QLabel();
    videoSetSuggestedAction->setObjectName("videoSetAssistantSuggestedAction");
    videoSetSuggestedAction->setWordWrap(true);
    root->insertWidget(4, videoSetSuggestedAction);

    videoSetActivityPanel = new QFrame();
    videoSetActivityPanel->setObjectName("videoSetActivityPanel");
    videoSetActivityPanel->setFrameShape(QFrame::StyledPanel);
    videoSetActivityPanel->setProperty("vsxSurface", "raised");
    videoSetActivityPanel->setProperty("observedScan", false);
    videoSetActivityPanel->setProperty("observedRecovery", false);
    videoSetActivityPanel->setProperty("observedFinalHash", false);
    auto *activityLayout = new QVBoxLayout(videoSetActivityPanel);
    activityLayout->setContentsMargins(10, 8, 10, 8);
    activityLayout->setSpacing(4);
    auto *activityHeading = new QHBoxLayout();
    videoSetActivityIcon = new QLabel("●");
    videoSetActivityIcon->setObjectName("videoSetActivityIcon");
    videoSetActivityTitle = new QLabel("No operation is running");
    videoSetActivityTitle->setObjectName("videoSetActivityTitle");
    videoSetActivityTitle->setProperty("cardTitle", true);
    videoSetActivityElapsed = new QLabel("Elapsed: 0:00");
    videoSetActivityElapsed->setObjectName("videoSetElapsedTime");
    videoSetActivityRemaining = new QLabel();
    videoSetActivityRemaining->setObjectName("videoSetEstimatedRemaining");
    activityHeading->addWidget(videoSetActivityIcon);
    activityHeading->addWidget(videoSetActivityTitle, 1);
    activityHeading->addWidget(videoSetActivityElapsed);
    activityHeading->addWidget(videoSetActivityRemaining);
    activityLayout->addLayout(activityHeading);
    videoSetActivityDescription = new QLabel();
    videoSetActivityDescription->setObjectName("videoSetActivityDescription");
    videoSetActivityDescription->setWordWrap(true);
    activityLayout->addWidget(videoSetActivityDescription);
    videoSetActivityProgress = new QProgressBar();
    videoSetActivityProgress->setObjectName("videoSetProgressBar");
    activityLayout->addWidget(videoSetActivityProgress);
    auto *activityDetails = new QHBoxLayout();
    videoSetActivityCounter = new QLabel();
    videoSetActivityCounter->setObjectName("videoSetProgressCounter");
    videoSetActivityCurrentItem = new QLabel();
    videoSetActivityCurrentItem->setObjectName("videoSetCurrentItem");
    activityDetails->addWidget(videoSetActivityCounter);
    activityDetails->addWidget(videoSetActivityCurrentItem, 1);
    activityLayout->addLayout(activityDetails);
    videoSetActivityWatchdog = new QLabel(
        "This is taking longer than usual. VidStoreX is still working; no action is required.");
    videoSetActivityWatchdog->setObjectName("videoSetActivityWatchdog");
    videoSetActivityWatchdog->setWordWrap(true);
    videoSetActivityWatchdog->setVisible(false);
    activityLayout->addWidget(videoSetActivityWatchdog);
    auto *activityActions = new QHBoxLayout();
    videoSetAssistantCancelButton = new QPushButton("Cancel safely");
    videoSetAssistantCancelButton->setObjectName("videoSetCancelButton");
    videoSetAssistantCancelButton->setAccessibleName("Cancel current Video Set operation safely");
    videoSetActivityRetryButton = new QPushButton("Retry");
    videoSetActivityRetryButton->setObjectName("videoSetRetryButton");
    activityActions->addWidget(videoSetAssistantCancelButton);
    activityActions->addWidget(videoSetActivityRetryButton);
    activityActions->addStretch();
    activityLayout->addLayout(activityActions);
    root->insertWidget(5, videoSetActivityPanel);

    videoSetAssistantStack = new QStackedWidget();
    videoSetAssistantStack->setObjectName("videoSetAssistantStack");
    auto *assistantScroll = new QScrollArea();
    assistantScroll->setObjectName("videoSetAssistantScrollArea");
    assistantScroll->setWidgetResizable(true);
    assistantScroll->setFrameShape(QFrame::NoFrame);
    assistantScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    assistantScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    assistantScroll->setWidget(videoSetAssistantStack);
    assistantScroll->setMinimumHeight(330);
    root->insertWidget(6, assistantScroll, 1);

    const auto makePage = [this](const QString &title,
                                 const QString &description) {
        auto *page = new QWidget();
        auto *layout = new QVBoxLayout(page);
        layout->setContentsMargins(24, 16, 24, 18);
        layout->setSpacing(10);
        auto *heading = new QLabel(title);
        heading->setObjectName(QString("videoSetPageHeading%1")
            .arg(videoSetAssistantPageHeadings.size()));
        heading->setWordWrap(true);
        heading->setProperty("pageTitle", true);
        layout->addWidget(heading);
        auto *subtitle = new QLabel(description);
        subtitle->setObjectName(QString("videoSetPageSubtitle%1")
            .arg(videoSetAssistantPageSubtitles.size()));
        subtitle->setWordWrap(true);
        subtitle->setProperty("muted", true);
        layout->addWidget(subtitle);
        videoSetAssistantPageHeadings.push_back(heading);
        videoSetAssistantPageSubtitles.push_back(subtitle);
        videoSetAssistantStack->addWidget(page);
        return page;
    };
    const auto pageLayout = [](QWidget *page) {
        return qobject_cast<QVBoxLayout *>(page->layout());
    };
    const auto addNavigation = [](QVBoxLayout *layout,
                                  QPushButton *back,
                                  QPushButton *primary) {
        auto *buttons = new QHBoxLayout();
        if (back) buttons->addWidget(back);
        buttons->addStretch();
        if (primary) buttons->addWidget(primary);
        layout->addLayout(buttons);
    };

    // 0: Welcome
    auto *welcome = makePage(
        "Video Set Assistant",
        "Store one file across multiple videos, or recover a file from downloaded videos.");
    welcome->setObjectName("videoSetAssistantWelcomePage");
    videoSetWelcomePage = welcome;
    videoSetWelcomePage->installEventFilter(this);
    auto *welcomeLayout = pageLayout(welcome);
    auto *choiceLayout = new QHBoxLayout();
    videoSetCreateCard = new QFrame();
    videoSetCreateCard->setObjectName("videoSetCreateCard");
    videoSetCreateCard->setProperty("card", true);
    videoSetCreateCard->setSizePolicy(QSizePolicy::Expanding,
                                      QSizePolicy::Preferred);
    auto *createCardLayout = new QVBoxLayout(videoSetCreateCard);
    createCardLayout->setContentsMargins(
        vidstorex_ui::Layout::HeroPadding,
        vidstorex_ui::Layout::HeroPadding,
        vidstorex_ui::Layout::HeroPadding,
        vidstorex_ui::Layout::HeroPadding);
    createCardLayout->setSpacing(8);
    auto *createGlyph = new VidStoreXDataGlyph(
        VidStoreXDataGlyph::Mode::FileToBlocks);
    createGlyph->setObjectName("videoSetCreateGlyph");
    createCardLayout->addWidget(createGlyph, 0, Qt::AlignLeft);
    videoSetCreateCardTitle = new QLabel("Create a Video Set");
    videoSetCreateCardTitle->setObjectName("videoSetCreateCardTitle");
    videoSetCreateCardTitle->setProperty("cardTitle", true);
    createCardLayout->addWidget(videoSetCreateCardTitle);
    videoSetCreateCardDescription = new QLabel(
        "Turn one file into one or more videos.");
    videoSetCreateCardDescription->setWordWrap(true);
    videoSetCreateCardDescription->setProperty("muted", true);
    createCardLayout->addWidget(videoSetCreateCardDescription);
    videoSetCreateFlowLabel = new QLabel("SOURCE → VIDEO SET");
    videoSetCreateFlowLabel->setObjectName("videoSetCreateFlowLabel");
    videoSetCreateFlowLabel->setProperty("eyebrow", true);
    createCardLayout->addWidget(videoSetCreateFlowLabel);
    auto *createRail = new VidStoreXSignalRail();
    createRail->setObjectName("videoSetCreateSignalRail");
    createRail->setFixedWidth(96);
    createCardLayout->addWidget(createRail, 0, Qt::AlignLeft);
    videoSetWelcomeCreateButton = new QPushButton("Choose a file");
    videoSetWelcomeCreateButton->setObjectName("videoSetAssistantCreateChoice");
    videoSetWelcomeCreateButton->setAccessibleName("Create a Video Set");
    videoSetWelcomeCreateButton->setAccessibleDescription(
        "Turn one file into one or more videos.");
    videoSetWelcomeCreateButton->setProperty("primary", true);
    videoSetWelcomeCreateButton->setIcon(
        style()->standardIcon(QStyle::SP_FileIcon));
    createCardLayout->addWidget(videoSetWelcomeCreateButton);

    videoSetRecoverCard = new QFrame();
    videoSetRecoverCard->setObjectName("videoSetRecoverCard");
    videoSetRecoverCard->setProperty("card", true);
    videoSetRecoverCard->setSizePolicy(QSizePolicy::Expanding,
                                       QSizePolicy::Preferred);
    auto *recoverCardLayout = new QVBoxLayout(videoSetRecoverCard);
    recoverCardLayout->setContentsMargins(
        vidstorex_ui::Layout::HeroPadding,
        vidstorex_ui::Layout::HeroPadding,
        vidstorex_ui::Layout::HeroPadding,
        vidstorex_ui::Layout::HeroPadding);
    recoverCardLayout->setSpacing(8);
    auto *recoverGlyph = new VidStoreXDataGlyph(
        VidStoreXDataGlyph::Mode::BlocksToFile);
    recoverGlyph->setObjectName("videoSetRecoverGlyph");
    recoverCardLayout->addWidget(recoverGlyph, 0, Qt::AlignLeft);
    videoSetRecoverCardTitle = new QLabel("Recover a File");
    videoSetRecoverCardTitle->setObjectName("videoSetRecoverCardTitle");
    videoSetRecoverCardTitle->setProperty("cardTitle", true);
    recoverCardLayout->addWidget(videoSetRecoverCardTitle);
    videoSetRecoverCardDescription = new QLabel(
        "Rebuild the original file from downloaded Video Set videos.");
    videoSetRecoverCardDescription->setWordWrap(true);
    videoSetRecoverCardDescription->setProperty("muted", true);
    recoverCardLayout->addWidget(videoSetRecoverCardDescription);
    videoSetRecoverFlowLabel = new QLabel("VIDEO → FILE");
    videoSetRecoverFlowLabel->setObjectName("videoSetRecoverFlowLabel");
    videoSetRecoverFlowLabel->setProperty("eyebrow", true);
    recoverCardLayout->addWidget(videoSetRecoverFlowLabel);
    auto *recoverRail = new VidStoreXSignalRail();
    recoverRail->setObjectName("videoSetRecoverSignalRail");
    recoverRail->setFixedWidth(96);
    recoverCardLayout->addWidget(recoverRail, 0, Qt::AlignLeft);
    videoSetWelcomeRecoverButton = new QPushButton("Choose videos or set");
    videoSetWelcomeRecoverButton->setObjectName("videoSetAssistantRecoverChoice");
    videoSetWelcomeRecoverButton->setAccessibleName("Recover a File");
    videoSetWelcomeRecoverButton->setAccessibleDescription(
        "Rebuild the original file from downloaded Video Set videos.");
    videoSetWelcomeRecoverButton->setProperty("primary", true);
    videoSetWelcomeRecoverButton->setIcon(
        style()->standardIcon(QStyle::SP_DialogOpenButton));
    recoverCardLayout->addWidget(videoSetWelcomeRecoverButton);
    choiceLayout->addWidget(videoSetCreateCard);
    choiceLayout->addWidget(videoSetRecoverCard);
    welcomeLayout->addLayout(choiceLayout);
    auto *trustStrip = new QFrame();
    trustStrip->setObjectName("videoSetTrustStrip");
    trustStrip->setProperty("vsxSurface", "raised");
    auto *trustLayout = new QHBoxLayout(trustStrip);
    trustLayout->setContentsMargins(12, 7, 8, 7);
    videoSetTrustLabel = new QLabel(QString::fromUtf8(
        "✓ Real YouTube tested"));
    videoSetTrustLabel->setObjectName("videoSetTrustLabel");
    videoSetTrustLabel->setProperty("vsxState", "success");
    videoSetTrustDetailsButton = new QToolButton();
    videoSetTrustDetailsButton->setObjectName("videoSetTrustDetails");
    videoSetTrustDetailsButton->setText("Details");
    videoSetTrustDetailsButton->setProperty("vsxRole", "ghost");
    videoSetTrustDetailsButton->setPopupMode(QToolButton::InstantPopup);
    auto *trustMenu = new QMenu(videoSetTrustDetailsButton);
    auto *trustEvidence = trustMenu->addAction(
        "6/6 single-video exact · 4/4 set parts + full SHA-256 exact");
    trustEvidence->setObjectName("videoSetTrustEvidenceAction");
    trustEvidence->setEnabled(false);
    auto *trustCaveat = trustMenu->addAction(
        "Measured results for the tested configuration; not an absolute guarantee.");
    trustCaveat->setObjectName("videoSetTrustCaveatAction");
    trustCaveat->setEnabled(false);
    videoSetTrustDetailsButton->setMenu(trustMenu);
    trustLayout->addWidget(videoSetTrustLabel);
    trustLayout->addStretch();
    trustLayout->addWidget(videoSetTrustDetailsButton);
    welcomeLayout->addWidget(trustStrip);
    videoSetRecentGroup = new QFrame();
    videoSetRecentGroup->setObjectName("videoSetRecentGroup");
    videoSetRecentGroup->setProperty("card", true);
    auto *recentLayout = new QVBoxLayout(videoSetRecentGroup);
    recentLayout->setContentsMargins(16, 12, 16, 12);
    recentLayout->setSpacing(8);
    auto *recentHeader = new QHBoxLayout();
    recentHeader->setSpacing(vidstorex_ui::Layout::CompactActionGap);
    videoSetRecentTitle = new QLabel("Recent Video Sets");
    videoSetRecentTitle->setObjectName("videoSetRecentTitle");
    videoSetRecentTitle->setProperty("sectionTitle", true);
    recentHeader->addWidget(videoSetRecentTitle);
    recentHeader->addStretch();
    recentLayout->addLayout(recentHeader);
    videoSetRecentList = new QListWidget();
    videoSetRecentList->setObjectName("videoSetRecentList");
    videoSetRecentList->setSizePolicy(QSizePolicy::Preferred,
                                      QSizePolicy::Fixed);
    videoSetRecentList->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    videoSetRecentList->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    videoSetRecentList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    recentLayout->addWidget(videoSetRecentList);
    videoSetRecentEmptyState = new QFrame();
    videoSetRecentEmptyState->setObjectName("videoSetRecentEmptyState");
    auto *emptyLayout = new QHBoxLayout(videoSetRecentEmptyState);
    emptyLayout->setContentsMargins(8, 4, 8, 4);
    auto *emptyGlyph = new VidStoreXDataGlyph(
        VidStoreXDataGlyph::Mode::Empty);
    emptyGlyph->setObjectName("videoSetRecentEmptyGlyph");
    emptyLayout->addWidget(emptyGlyph);
    auto *emptyTextLayout = new QVBoxLayout();
    videoSetRecentEmptyLabel = new QLabel("No recent Video Sets yet.");
    videoSetRecentEmptyLabel->setObjectName("videoSetRecentEmptyLabel");
    videoSetRecentEmptyLabel->setProperty("cardTitle", true);
    emptyTextLayout->addWidget(videoSetRecentEmptyLabel);
    auto *emptyActions = new QHBoxLayout();
    videoSetRecentEmptyCreateButton = new QPushButton("Create your first set");
    videoSetRecentEmptyCreateButton->setObjectName("videoSetRecentEmptyCreate");
    videoSetRecentEmptyCreateButton->setProperty("vsxRole", "secondary");
    videoSetRecentEmptyRecoverButton = new QPushButton("Recover returned videos");
    videoSetRecentEmptyRecoverButton->setObjectName("videoSetRecentEmptyRecover");
    videoSetRecentEmptyRecoverButton->setProperty("vsxRole", "ghost");
    emptyActions->addWidget(videoSetRecentEmptyCreateButton);
    emptyActions->addWidget(videoSetRecentEmptyRecoverButton);
    emptyActions->addStretch();
    emptyTextLayout->addLayout(emptyActions);
    emptyLayout->addLayout(emptyTextLayout, 1);
    recentLayout->addWidget(videoSetRecentEmptyState);
    videoSetRecentContinueButton = new QPushButton("Continue");
    videoSetRecentContinueButton->setObjectName(
        "videoSetRecentContinueButton");
    videoSetRecentOpenFolderButton = new QPushButton("Open folder");
    videoSetRecentRemoveButton = new QPushButton(QString::fromUtf8("•••"));
    videoSetRecentRemoveButton->setObjectName("videoSetRecentOverflow");
    videoSetRecentRemoveButton->setAccessibleName("More recent set actions");
    auto *recentMenu = new QMenu(videoSetRecentRemoveButton);
    auto *recentShowManifestAction = recentMenu->addAction("Technical details");
    recentShowManifestAction->setObjectName("recentShowManifestAction");
    auto *recentCopyManifestAction = recentMenu->addAction(
        "Copy manifest location");
    recentCopyManifestAction->setObjectName("recentCopyManifestAction");
    auto *recentOpenReportAction = recentMenu->addAction("Open report");
    recentOpenReportAction->setObjectName("recentOpenReportAction");
    recentMenu->addSeparator();
    auto *recentRemoveAction = recentMenu->addAction("Remove from list");
    recentRemoveAction->setObjectName("recentRemoveAction");
    videoSetRecentRemoveButton->setMenu(recentMenu);
    for (auto *button : {videoSetRecentContinueButton,
                         videoSetRecentOpenFolderButton,
                         videoSetRecentRemoveButton})
        button->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    videoSetRecentRemoveButton->setFixedWidth(42);
    recentHeader->addWidget(videoSetRecentContinueButton);
    recentHeader->addWidget(videoSetRecentOpenFolderButton);
    recentHeader->addWidget(videoSetRecentRemoveButton);
    welcomeLayout->addWidget(videoSetRecentGroup);
    welcomeLayout->addStretch();

    // 1: Source
    auto *source = makePage(
        "1. Choose a file",
        "Choose the file you want to turn into videos. Any file type is supported; the source is never moved, modified, or deleted.");
    source->setObjectName("videoSetAssistantSourcePage");
    auto *sourceLayout = pageLayout(source);
    videoSetSourceDropLabel = new QLabel(
        "Drop one file here, or use Choose file below.");
    videoSetSourceDropLabel->setObjectName("videoSetSourceDropArea");
    videoSetSourceDropLabel->setAlignment(Qt::AlignCenter);
    videoSetSourceDropLabel->setMinimumHeight(70);
    videoSetSourceDropLabel->setProperty("vsxRole", "dropZone");
    videoSetSourceDropLabel->setAcceptDrops(true);
    videoSetSourceDropLabel->installEventFilter(this);
    sourceLayout->addWidget(videoSetSourceDropLabel);
    auto *sourceForm = new QGridLayout();
    videoSetAssistantInputEdit = new QLineEdit();
    videoSetAssistantInputEdit->setObjectName("videoSetAssistantSourcePath");
    videoSetAssistantInputEdit->setPlaceholderText("Source file");
    videoSetAssistantInputBrowseButton = new QPushButton("Choose file...");
    videoSetAssistantInputBrowseButton->setObjectName("videoSetAssistantChooseFile");
    videoSetAssistantOutputEdit = new QLineEdit();
    videoSetAssistantOutputEdit->setObjectName("videoSetAssistantOutputRoot");
    videoSetAssistantOutputEdit->setPlaceholderText("Video Set output folder");
    videoSetAssistantOutputBrowseButton = new QPushButton("Choose folder...");
    sourceForm->addWidget(new QLabel("File:"), 0, 0);
    sourceForm->addWidget(videoSetAssistantInputEdit, 0, 1);
    sourceForm->addWidget(videoSetAssistantInputBrowseButton, 0, 2);
    sourceForm->addWidget(new QLabel("Output folder:"), 1, 0);
    sourceForm->addWidget(videoSetAssistantOutputEdit, 1, 1);
    sourceForm->addWidget(videoSetAssistantOutputBrowseButton, 1, 2);
    sourceLayout->addLayout(sourceForm);
    videoSetSourceInfoLabel = new QLabel("No file selected.");
    videoSetSourceInfoLabel->setObjectName("videoSetAssistantSourceInfo");
    videoSetSourceInfoLabel->setWordWrap(true);
    sourceLayout->addWidget(videoSetSourceInfoLabel);
    sourceLayout->addStretch();
    auto *sourceBack = new QPushButton("Back");
    sourceBack->setProperty("vsxRole", "ghost");
    videoSetSourceContinueButton = new QPushButton("Continue");
    videoSetSourceContinueButton->setObjectName("videoSetAssistantSourceContinue");
    addNavigation(sourceLayout, sourceBack, videoSetSourceContinueButton);

    // 2: Mode
    auto *mode = makePage(
        "2. Choose a mode",
        "Choose how you want to balance reliability and video count.");
    mode->setObjectName("videoSetAssistantModePage");
    auto *modeLayout = pageLayout(mode);
    videoSetProfileCards = new QButtonGroup(this);
    auto *profileCardsLayout = new QHBoxLayout();
    videoSetResilientCard = new QGroupBox("Most Reliable");
    videoSetResilientCard->setObjectName("videoSetResilientCard");
    videoSetResilientCard->setProperty("card", true);
    auto *resilientLayout = new QVBoxLayout(videoSetResilientCard);
    resilientLayout->setContentsMargins(18, 20, 18, 18);
    resilientLayout->setSpacing(9);
    auto *resilientGlyph = new VidStoreXDataGlyph(
        VidStoreXDataGlyph::Mode::Verified);
    resilientGlyph->setObjectName("videoSetResilientGlyph");
    resilientLayout->addWidget(resilientGlyph, 0, Qt::AlignLeft);
    videoSetResilientRadio = new QRadioButton("Resilient (recommended default)");
    videoSetResilientRadio->setObjectName("videoSetResilientChoice");
    videoSetResilientRadio->setChecked(true);
    resilientLayout->addWidget(videoSetResilientRadio);
    auto *resilientBadge = new QLabel("Recommended");
    resilientBadge->setObjectName("videoSetResilientBadge");
    resilientBadge->setProperty("badge", true);
    resilientBadge->setProperty("vsxRole", "badge");
    resilientLayout->addWidget(resilientBadge, 0, Qt::AlignLeft);
    videoSetResilientDescription = new QLabel(
        "Uses more video time for the most conservative storage mode.\n"
        "Technical: 8x8, 1-bit, signal 1.0, repair 5%.");
    videoSetResilientDescription->setWordWrap(true);
    videoSetResilientDescription->setProperty("muted", true);
    resilientLayout->addWidget(videoSetResilientDescription);
    auto *resilientRail = new VidStoreXSignalRail();
    resilientRail->setObjectName("videoSetResilientSignalRail");
    resilientLayout->addWidget(resilientRail);
    videoSetHighCapacityCard = new QGroupBox("Fewer & Shorter Videos");
    videoSetHighCapacityCard->setObjectName("videoSetHighCapacityCard");
    videoSetHighCapacityCard->setProperty("card", true);
    auto *capacityLayout = new QVBoxLayout(videoSetHighCapacityCard);
    capacityLayout->setContentsMargins(18, 20, 18, 18);
    capacityLayout->setSpacing(9);
    auto *capacityGlyph = new VidStoreXDataGlyph(
        VidStoreXDataGlyph::Mode::FileToBlocks);
    capacityGlyph->setObjectName("videoSetHighCapacityGlyph");
    capacityLayout->addWidget(capacityGlyph, 0, Qt::AlignLeft);
    videoSetHighCapacityRadio = new QRadioButton("High Capacity (opt-in)");
    videoSetHighCapacityRadio->setObjectName("videoSetHighCapacityChoice");
    capacityLayout->addWidget(videoSetHighCapacityRadio);
    auto *capacityBadge = new QLabel("Real YouTube tested");
    capacityBadge->setObjectName("videoSetHighCapacityBadge");
    capacityBadge->setProperty("badge", true);
    capacityBadge->setProperty("vsxRole", "badge");
    capacityLayout->addWidget(capacityBadge, 0, Qt::AlignLeft);
    videoSetHighCapacityDescription = new QLabel(
        "Stores more data in each video and produces fewer or shorter videos.\n"
        "Real YouTube tested: 6/6 single-video exact and 4/4 Video Set parts with full-file SHA exact.\n"
        "Technical: 4x4, 1-bit, signal 1.0, repair 5%; config 538F2B009FAB.");
    videoSetHighCapacityDescription->setWordWrap(true);
    videoSetHighCapacityDescription->setProperty("muted", true);
    capacityLayout->addWidget(videoSetHighCapacityDescription);
    auto *capacityRail = new VidStoreXSignalRail();
    capacityRail->setObjectName("videoSetHighCapacitySignalRail");
    capacityLayout->addWidget(capacityRail);
    videoSetProfileCards->addButton(videoSetResilientRadio, 0);
    videoSetProfileCards->addButton(videoSetHighCapacityRadio, 1);
    profileCardsLayout->addWidget(videoSetResilientCard);
    profileCardsLayout->addWidget(videoSetHighCapacityCard);
    modeLayout->addLayout(profileCardsLayout);
    videoSetAdvancedSettingsButton = new QToolButton();
    videoSetAdvancedSettingsButton->setObjectName("videoSetAdvancedSettingsToggle");
    videoSetAdvancedSettingsButton->setText("Advanced settings");
    videoSetAdvancedSettingsButton->setCheckable(true);
    videoSetAdvancedSettingsButton->setArrowType(Qt::RightArrow);
    modeLayout->addWidget(videoSetAdvancedSettingsButton);
    videoSetAdvancedSettingsWidget = new QWidget();
    videoSetAdvancedSettingsWidget->setObjectName("videoSetAdvancedSettingsPanel");
    auto *advancedLayout = new QGridLayout(videoSetAdvancedSettingsWidget);
    videoSetAssistantTargetSpin = new QSpinBox();
    videoSetAssistantTargetSpin->setObjectName("videoSetAssistantTargetDuration");
    videoSetAssistantTargetSpin->setRange(1, 86400);
    videoSetAssistantTargetSpin->setValue(600);
    videoSetAssistantTargetSpin->setSuffix(" seconds");
    videoSetAssistantMaximumSizeSpin = new QSpinBox();
    videoSetAssistantMaximumSizeSpin->setObjectName("videoSetAssistantMaximumSize");
    videoSetAssistantMaximumSizeSpin->setRange(0, 1024 * 1024);
    videoSetAssistantMaximumSizeSpin->setValue(1500);
    videoSetAssistantMaximumSizeSpin->setSuffix(" MiB (0 disables cap)");
    videoSetAssistantReserveSpin = new QDoubleSpinBox();
    videoSetAssistantReserveSpin->setObjectName("videoSetAssistantReserve");
    videoSetAssistantReserveSpin->setRange(0.0, 99.0);
    videoSetAssistantReserveSpin->setValue(10.0);
    videoSetAssistantReserveSpin->setSuffix("%");
    videoSetAdvancedProfileLabel = new QLabel(
        "Resilient: 8x8, 1-bit, signal 1.0, repair 5%, 1920x1080 at 30 FPS");
    videoSetAdvancedProfileLabel->setObjectName("videoSetAdvancedProfileDetails");
    videoSetAdvancedProfileLabel->setWordWrap(true);
    advancedLayout->addWidget(new QLabel("Target duration:"), 0, 0);
    advancedLayout->addWidget(videoSetAssistantTargetSpin, 0, 1);
    advancedLayout->addWidget(new QLabel("Maximum actual video size:"), 1, 0);
    advancedLayout->addWidget(videoSetAssistantMaximumSizeSpin, 1, 1);
    advancedLayout->addWidget(new QLabel("Safety reserve:"), 2, 0);
    advancedLayout->addWidget(videoSetAssistantReserveSpin, 2, 1);
    advancedLayout->addWidget(new QLabel("Container:"), 3, 0);
    advancedLayout->addWidget(new QLabel("MKV (current backend format)"), 3, 1);
    advancedLayout->addWidget(videoSetAdvancedProfileLabel, 4, 0, 1, 2);
    videoSetAdvancedSettingsWidget->setVisible(false);
    modeLayout->addWidget(videoSetAdvancedSettingsWidget);
    modeLayout->addStretch();
    auto *modeBack = new QPushButton("Back");
    modeBack->setProperty("vsxRole", "ghost");
    videoSetModeContinueButton = new QPushButton("Calculate plan");
    videoSetModeContinueButton->setObjectName("videoSetAssistantCalculatePlan");
    addNavigation(modeLayout, modeBack, videoSetModeContinueButton);

    // 3: Plan
    auto *plan = makePage(
        "3. Review the plan",
        "The plan uses the existing encoder's real packet, frame, repair, and capacity calculations.");
    plan->setObjectName("videoSetAssistantPlanPage");
    auto *planLayout = pageLayout(plan);
    videoSetPlanSummaryLabel = new QLabel("Calculating plan...");
    videoSetPlanSummaryLabel->setObjectName("videoSetAssistantPlanSummary");
    videoSetPlanSummaryLabel->setWordWrap(true);
    videoSetPlanSummaryLabel->setProperty("sectionTitle", true);
    planLayout->addWidget(videoSetPlanSummaryLabel);
    videoSetPlanMetricsLabel = new QLabel();
    videoSetPlanMetricsLabel->setWordWrap(true);
    planLayout->addWidget(videoSetPlanMetricsLabel);
    auto *planSafety = new QLabel(
        "Estimates may differ from actual encoded sizes. The source file will not be modified. "
        "Final recovery is accepted only after SHA-256 verification.");
    planSafety->setWordWrap(true);
    planLayout->addWidget(planSafety);
    videoSetPartDetailsButton = new QToolButton();
    videoSetPartDetailsButton->setObjectName("videoSetPartDetailsToggle");
    videoSetPartDetailsButton->setText("Show part details");
    videoSetPartDetailsButton->setCheckable(true);
    videoSetPartDetailsButton->setArrowType(Qt::RightArrow);
    planLayout->addWidget(videoSetPartDetailsButton);
    videoSetAssistantPlanTable = new QTableWidget(0, 6);
    videoSetAssistantPlanTable->setObjectName("videoSetAssistantPlanTable");
    videoSetAssistantPlanTable->setHorizontalHeaderLabels(
        {"Part", "Payload bytes", "Frames", "Duration", "Estimated size", "Status"});
    videoSetAssistantPlanTable->horizontalHeader()->setSectionResizeMode(
        QHeaderView::Stretch);
    videoSetAssistantPlanTable->setVisible(false);
    planLayout->addWidget(videoSetAssistantPlanTable);
    planLayout->addStretch();
    auto *planBack = new QPushButton("Back");
    planBack->setProperty("vsxRole", "ghost");
    videoSetCreateVideosButton = new QPushButton("Create Videos");
    videoSetCreateVideosButton->setObjectName("videoSetAssistantCreateVideos");
    addNavigation(planLayout, planBack, videoSetCreateVideosButton);

    // 4: Encode progress
    auto *progress = makePage(
        "4. Create and verify videos",
        "Each completed part is decoded and checked locally before it is accepted.");
    progress->setObjectName("videoSetAssistantProgressPage");
    auto *progressLayout = pageLayout(progress);
    videoSetAssistantProgress = new QProgressBar();
    videoSetAssistantProgress->setObjectName("videoSetAssistantProgress");
    videoSetAssistantProgress->setRange(0, 100);
    progressLayout->addWidget(videoSetAssistantProgress);
    videoSetProgressPhaseLabel = new QLabel("Reading source file");
    videoSetProgressPhaseLabel->setObjectName("videoSetAssistantPhase");
    videoSetProgressPartLabel = new QLabel("Waiting to start");
    videoSetProgressPartLabel->setObjectName("videoSetAssistantCurrentPart");
    progressLayout->addWidget(videoSetProgressPhaseLabel);
    progressLayout->addWidget(videoSetProgressPartLabel);
    progressLayout->addStretch();
    auto *progressButtons = new QHBoxLayout();
    videoSetProgressResumeButton = new QPushButton("Resume");
    videoSetProgressResumeButton->setObjectName("videoSetAssistantResume");
    videoSetProgressOpenFolderButton = new QPushButton("Open Videos Folder");
    videoSetProgressOpenFolderButton->setObjectName(
        "videoSetAssistantProgressOpenFolder");
    videoSetProgressOpenFolderButton->setIcon(
        style()->standardIcon(QStyle::SP_DirOpenIcon));
    videoSetProgressContinueButton = new QPushButton("Continue to upload guide");
    videoSetProgressContinueButton->setObjectName("videoSetAssistantProgressContinue");
    progressButtons->addWidget(videoSetProgressResumeButton);
    progressButtons->addWidget(videoSetProgressOpenFolderButton);
    progressButtons->addStretch();
    progressButtons->addWidget(videoSetProgressContinueButton);
    progressLayout->addLayout(progressButtons);

    // 5: Upload guide
    auto *upload = makePage(
        "5. Upload to YouTube",
        "Upload your VidStoreX videos to YouTube and add them to one playlist.");
    upload->setObjectName("videoSetAssistantUploadPage");
    auto *uploadLayout = pageLayout(upload);
    videoSetUploadInstructionsLabel = new QLabel(
        "1. Open the videos folder.\n"
        "2. Upload all videos to YouTube as Unlisted.\n"
        "3. Wait until 1080p processing finishes for every video.\n"
        "4. Put all videos into one playlist.\n"
        "5. Copy the playlist link and return here.\n\n"
        "VidStoreX never signs in to your account and never uploads automatically.");
    videoSetUploadInstructionsLabel->setWordWrap(true);
    videoSetUploadInstructionsLabel->setVisible(false);
    auto *instructionRow = new QHBoxLayout();
    const QStringList instructionTexts{
        "Open the videos folder.",
        "Upload all parts to YouTube.",
        "Set them to Unlisted when possible.",
        "Wait for 1080p processing to finish.",
        "Add the videos to one playlist."};
    for (int index = 0; index < instructionTexts.size(); ++index) {
        auto *instruction = new QFrame();
        instruction->setObjectName(
            QString("videoSetUploadInstruction%1").arg(index + 1));
        instruction->setProperty("vsxSurface", "raised");
        auto *instructionLayout = new QVBoxLayout(instruction);
        auto *number = new QLabel(QString::number(index + 1));
        number->setProperty("vsxRole", "badge");
        auto *text = new QLabel(instructionTexts[index]);
        text->setWordWrap(true);
        text->setProperty("i18nSource", instructionTexts[index]);
        instructionLayout->addWidget(number, 0, Qt::AlignLeft);
        instructionLayout->addWidget(text);
        instructionRow->addWidget(instruction, 1);
        videoSetUploadInstructionLabels.push_back(text);
    }
    uploadLayout->addLayout(instructionRow);
    youtubeSyncOperationCard = new QFrame();
    youtubeSyncOperationCard->setObjectName("youtubeSyncCard");
    youtubeSyncOperationCard->setProperty("vsxSurface", "raised");
    auto *syncLayout = new QVBoxLayout(youtubeSyncOperationCard);
    auto *syncTitle = new QLabel("YouTube Sync  •  Experimental");
    syncTitle->setObjectName("youtubeSyncTitle");
    syncTitle->setProperty("sectionTitle", true);
    syncTitle->setProperty("i18nSource", "YouTube Sync  •  Experimental");
    syncLayout->addWidget(syncTitle);
    videoSetYouTubeSyncStatus = new QLabel(
        "Optional official API upload. Manual upload always remains available.");
    videoSetYouTubeSyncStatus->setObjectName("youtubeSyncStatus");
    videoSetYouTubeSyncStatus->setWordWrap(true);
    videoSetYouTubeSyncStatus->setProperty(
        "i18nSource",
        "Optional official API upload. Manual upload always remains available.");
    syncLayout->addWidget(videoSetYouTubeSyncStatus);
    videoSetYouTubeSyncProgress = new QProgressBar();
    videoSetYouTubeSyncProgress->setObjectName("youtubeSyncProgress");
    videoSetYouTubeSyncProgress->setRange(0, 100);
    syncLayout->addWidget(videoSetYouTubeSyncProgress);
    auto *syncActions = new QHBoxLayout();
    videoSetYouTubeSyncButton = new QPushButton("Upload with YouTube Sync");
    videoSetYouTubeSyncButton->setObjectName("youtubeSyncStartButton");
    videoSetYouTubeSyncPauseButton = new QPushButton("Pause Upload");
    videoSetYouTubeSyncPauseButton->setObjectName("youtubeSyncPauseButton");
    videoSetYouTubeSyncPauseButton->setEnabled(false);
    syncActions->addWidget(videoSetYouTubeSyncButton);
    syncActions->addWidget(videoSetYouTubeSyncPauseButton);
    syncActions->addStretch();
    syncLayout->addLayout(syncActions);
    auto *uploadPrivacy = new QLabel(
        "Upload the videos manually. You can recover your file later by pasting the playlist link into VidStoreX.");
    uploadPrivacy->setObjectName("videoSetUploadPrivacyNotice");
    uploadPrivacy->setProperty("muted", true);
    uploadPrivacy->setProperty("i18nSource",
        "Upload the videos manually. You can recover your file later by pasting the playlist link into VidStoreX.");
    uploadLayout->addWidget(uploadPrivacy);
    auto *uploadActions = new QHBoxLayout();
    videoSetOpenVideosButton = new QPushButton("Open Videos Folder");
    videoSetOpenVideosButton->setObjectName("videoSetOpenVideosFolderButton");
    videoSetOpenYouTubeButton = new QPushButton("Open YouTube");
    videoSetOpenYouTubeButton->setObjectName("videoSetOpenYouTubeButton");
    videoSetOpenChecklistButton = new QPushButton("Open Upload Checklist");
    uploadActions->addWidget(videoSetOpenVideosButton);
    uploadActions->addWidget(videoSetOpenYouTubeButton);
    uploadActions->addWidget(videoSetOpenChecklistButton);
    uploadLayout->addLayout(uploadActions);
    uploadLayout->addStretch();
    videoSetUploadedButton = new QPushButton("I have uploaded the videos");
    videoSetUploadedButton->setObjectName("videoSetAssistantUploaded");
    addNavigation(uploadLayout, nullptr, videoSetUploadedButton);

    // 6: Download
    auto *download = makePage(
        "6. Download YouTube's processed copies",
        "Paste the playlist link. VidStoreX calls yt-dlp directly without PowerShell, cookies, login, or shell command construction.");
    download->setObjectName("videoSetAssistantDownloadPage");
    auto *downloadLayout = pageLayout(download);
    auto *downloadForm = new QGridLayout();
    videoSetPlaylistUrlEdit = new QLineEdit();
    videoSetPlaylistUrlEdit->setObjectName("videoSetPlaylistUrl");
    videoSetPlaylistUrlEdit->setPlaceholderText(
        "https://www.youtube.com/playlist?list=...");
    videoSetDownloadButton = new QPushButton("Download Processed Videos");
    videoSetDownloadButton->setObjectName("videoSetDownloadReturnedVideos");
    downloadForm->addWidget(new QLabel("Playlist URL:"), 0, 0);
    downloadForm->addWidget(videoSetPlaylistUrlEdit, 0, 1);
    downloadForm->addWidget(videoSetDownloadButton, 0, 2);
    downloadLayout->addLayout(downloadForm);
    auto *downloadAlternatives = new QHBoxLayout();
    videoSetSelectYtDlpButton = new QPushButton("Select yt-dlp executable");
    videoSetManualReturnedButton = new QPushButton("Choose returned videos manually");
    downloadAlternatives->addWidget(videoSetSelectYtDlpButton);
    downloadAlternatives->addWidget(videoSetManualReturnedButton);
    downloadLayout->addLayout(downloadAlternatives);
    videoSetDownloadStatusLabel = new QLabel("Waiting for a playlist link.");
    videoSetDownloadStatusLabel->setObjectName("videoSetDownloadStatus");
    videoSetDownloadStatusLabel->setWordWrap(true);
    downloadLayout->addWidget(videoSetDownloadStatusLabel);
    videoSetDownloadProgress = new QProgressBar();
    videoSetDownloadProgress->setRange(0, 100);
    downloadLayout->addWidget(videoSetDownloadProgress);
    downloadLayout->addStretch();

    // 7: Scan and recovery setup
    auto *scan = makePage(
        "7. Check parts and recover",
        "Select a set, manifest, or returned-video folder. Embedded metadata identifies parts even when files are renamed or shuffled.");
    scan->setObjectName("videoSetAssistantRecoverPage");
    auto *scanLayout = pageLayout(scan);
    auto *instantCard = new QFrame();
    instantCard->setObjectName("instantPlaylistRecoveryCard");
    instantCard->setProperty("vsxSurface", "raised");
    instantCard->setMaximumHeight(72);
    auto *instantLayout = new QGridLayout(instantCard);
    instantLayout->setContentsMargins(8, 4, 8, 4);
    instantLayout->setVerticalSpacing(2);
    auto *instantTitle = new QLabel("Recover from Playlist");
    instantTitle->setObjectName("instantPlaylistRecoveryTitle");
    instantTitle->setProperty("sectionTitle", true);
    instantTitle->setProperty("i18nSource", "Recover from Playlist");
    instantTitle->setVisible(false);
    videoSetInstantPlaylistEdit = new QLineEdit();
    videoSetInstantPlaylistEdit->setObjectName("instantPlaylistUrl");
    videoSetInstantPlaylistEdit->setPlaceholderText(
        "Paste a YouTube playlist link");
    videoSetInstantRecoverButton = new QPushButton("Recover from Playlist");
    videoSetInstantRecoverButton->setObjectName("instantPlaylistRecoverButton");
    videoSetInstantRecoveryStatus = new QLabel(
        "Paste a playlist link to download, scan, and recover one complete set.");
    videoSetInstantRecoveryStatus->setObjectName("instantPlaylistRecoveryStatus");
    videoSetInstantRecoveryStatus->setWordWrap(false);
    instantLayout->addWidget(instantTitle, 0, 0, 1, 3);
    instantLayout->addWidget(videoSetInstantPlaylistEdit, 1, 0, 1, 2);
    instantLayout->addWidget(videoSetInstantRecoverButton, 1, 2);
    instantLayout->addWidget(videoSetInstantRecoveryStatus, 2, 0, 1, 3);
    scanLayout->addWidget(instantCard);
    auto *scanForm = new QGridLayout();
    videoSetAssistantRecoveryInputEdit = new QLineEdit();
    videoSetAssistantRecoveryInputEdit->setObjectName("videoSetAssistantReturnedPath");
    videoSetAssistantRecoveryInputEdit->setPlaceholderText(
        "Video Set folder, set_manifest.json, video, or returned folder");
    auto *scanBrowse = new QPushButton("Choose Set or Videos...");
    auto *scanFileBrowse = new QPushButton("Choose manifest or video...");
    videoSetAssistantRecoveryOutputEdit = new QLineEdit();
    videoSetAssistantRecoveryOutputEdit->setObjectName("videoSetAssistantRecoveryOutput");
    videoSetAssistantRecoveryOutputEdit->setPlaceholderText("Recovered output folder");
    auto *scanOutputBrowse = new QPushButton("Output folder...");
    scanForm->addWidget(new QLabel("Set or videos:"), 0, 0);
    scanForm->addWidget(videoSetAssistantRecoveryInputEdit, 0, 1);
    scanForm->addWidget(scanBrowse, 0, 2);
    scanForm->addWidget(new QLabel("Recovered file folder:"), 1, 0);
    scanForm->addWidget(videoSetAssistantRecoveryOutputEdit, 1, 1);
    scanForm->addWidget(scanOutputBrowse, 1, 2);
    scanForm->addWidget(scanFileBrowse, 2, 2);
    scanLayout->addLayout(scanForm);
    videoSetScanSummaryLabel = new QLabel("Choose a folder or manifest, then scan.");
    videoSetScanSummaryLabel->setObjectName("videoSetAssistantScanSummary");
    videoSetScanSummaryLabel->setWordWrap(true);
    videoSetScanSummaryLabel->setProperty("sectionTitle", true);
    scanLayout->addWidget(videoSetScanSummaryLabel);
    videoSetScanCountsLabel = new QLabel("Scan has not started.");
    videoSetScanCountsLabel->setObjectName("videoSetAssistantScanCounts");
    videoSetScanCountsLabel->setVisible(false);
    scanLayout->addWidget(videoSetScanCountsLabel);
    auto *scanMetrics = new QHBoxLayout();
    const QStringList metricNames{
        "Verified", "Missing", "Corrupt", "Duplicates", "Conflicts"};
    for (int index = 0; index < metricNames.size(); ++index) {
        auto *metric = new QFrame();
        metric->setObjectName(QString("videoSetScanMetric%1").arg(index));
        metric->setProperty("metricCard", true);
        auto *metricLayout = new QVBoxLayout(metric);
        auto *value = new QLabel("0");
        value->setObjectName(QString("videoSetScanMetricValue%1").arg(index));
        value->setAlignment(Qt::AlignCenter);
        value->setProperty("metricValue", true);
        auto *title = new QLabel(metricNames[index]);
        title->setObjectName(QString("videoSetScanMetricTitle%1").arg(index));
        title->setAlignment(Qt::AlignCenter);
        metricLayout->addWidget(value);
        metricLayout->addWidget(title);
        videoSetScanMetricValues.push_back(value);
        videoSetScanMetricTitles.push_back(title);
        scanMetrics->addWidget(metric);
    }
    scanLayout->addLayout(scanMetrics);
    videoSetDetectedSetsList = new QListWidget();
    videoSetDetectedSetsList->setObjectName("videoSetDetectedSetsList");
    videoSetDetectedSetsList->setMaximumHeight(48);
    scanLayout->addWidget(videoSetDetectedSetsList);
    auto *scanActions = new QHBoxLayout();
    videoSetAssistantScanButton = new QPushButton("Check Videos");
    videoSetAssistantScanButton->setObjectName("videoSetAssistantScan");
    videoSetOpenReturnedButton = new QPushButton("Open Returned Folder");
    videoSetAssistantRecoverButton = new QPushButton("Recover Original File");
    videoSetAssistantRecoverButton->setObjectName("videoSetAssistantRecover");
    scanActions->addWidget(videoSetAssistantScanButton);
    scanActions->addWidget(videoSetOpenReturnedButton);
    scanActions->addStretch();
    scanActions->addWidget(videoSetAssistantRecoverButton);
    scanLayout->addLayout(scanActions);
    videoSetRecoveryAvailabilityLabel = new QLabel(
        "Recovery will become available after scanning finishes and every required part is verified.");
    videoSetRecoveryAvailabilityLabel->setObjectName(
        "videoSetRecoveryAvailability");
    videoSetRecoveryAvailabilityLabel->setWordWrap(true);
    scanLayout->addWidget(videoSetRecoveryAvailabilityLabel);
    scanLayout->addStretch();

    // 8: Recovery progress
    auto *recovery = makePage(
        "8. Recover and verify the file",
        "The partial output remains private to this set until every part and the final full-file SHA-256 are verified.");
    recovery->setObjectName("videoSetAssistantRecoveryProgressPage");
    auto *recoveryLayout = pageLayout(recovery);
    videoSetRecoveryProgressBar = new QProgressBar();
    videoSetRecoveryProgressBar->setRange(0, 0);
    recoveryLayout->addWidget(videoSetRecoveryProgressBar);
    videoSetRecoveryProgressLabel = new QLabel("Reading returned video");
    videoSetRecoveryProgressLabel->setObjectName("videoSetAssistantRecoveryPhase");
    recoveryLayout->addWidget(videoSetRecoveryProgressLabel);
    recoveryLayout->addStretch();

    // 9: Done
    auto *done = makePage(
        "Done",
        "Recovery is successful only when the final SHA-256 matches.");
    done->setObjectName("videoSetAssistantDonePage");
    auto *doneLayout = pageLayout(done);
    videoSetSuccessIcon = new QLabel();
    videoSetSuccessIcon->setObjectName("videoSetSuccessIcon");
    videoSetSuccessIcon->setPixmap(style()->standardIcon(
        QStyle::SP_DialogApplyButton).pixmap(48, 48));
    videoSetSuccessIcon->setAlignment(Qt::AlignCenter);
    doneLayout->addWidget(videoSetSuccessIcon);
    videoSetSuccessLabel = new QLabel("Your file was recovered exactly.");
    videoSetSuccessLabel->setObjectName("videoSetAssistantExactSuccess");
    videoSetSuccessLabel->setProperty("pageTitle", true);
    doneLayout->addWidget(videoSetSuccessLabel);
    videoSetSuccessRail = new VidStoreXSignalRail();
    videoSetSuccessRail->setObjectName("videoSetSuccessSignalRail");
    videoSetSuccessRail->setAccessibleName({});
    doneLayout->addWidget(videoSetSuccessRail);
    auto *successVerification = new QLabel(
        "VERIFIED BLOCKS → FULL-FILE SHA-256 → EXACT OUTPUT");
    successVerification->setObjectName("videoSetSuccessVerificationStrip");
    successVerification->setProperty("eyebrow", true);
    successVerification->setProperty("i18nSource",
        "VERIFIED BLOCKS → FULL-FILE SHA-256 → EXACT OUTPUT");
    doneLayout->addWidget(successVerification);
    videoSetSuccessDetailsLabel = new QLabel();
    videoSetSuccessDetailsLabel->setWordWrap(true);
    auto *successDetailsCard = new QFrame();
    successDetailsCard->setObjectName("videoSetSuccessDetailsCard");
    successDetailsCard->setProperty("vsxSurface", "raised");
    auto *successDetailsLayout = new QVBoxLayout(successDetailsCard);
    successDetailsLayout->addWidget(videoSetSuccessDetailsLabel);
    doneLayout->addWidget(successDetailsCard);
    auto *doneActions = new QHBoxLayout();
    videoSetOpenRecoveredButton = new QPushButton("Open Recovered File Location");
    videoSetOpenRecoveredButton->setObjectName("videoSetOpenRecoveredLocation");
    videoSetOpenSetFolderButton = new QPushButton("Open Set Folder");
    videoSetOpenSetFolderButton->setObjectName("videoSetOpenSetFolder");
    videoSetCopyShaButton = new QPushButton("Copy SHA-256");
    videoSetReturnHomeButton = new QPushButton("Return Home");
    doneActions->addWidget(videoSetOpenRecoveredButton);
    doneActions->addWidget(videoSetOpenSetFolderButton);
    doneActions->addWidget(videoSetCopyShaButton);
    doneActions->addStretch();
    doneActions->addWidget(videoSetReturnHomeButton);
    doneLayout->addLayout(doneActions);
    doneLayout->addStretch();

    // Technical output is available but hidden in the normal workflow.
    root->removeWidget(videoSetLog);
    videoSetTechnicalLogButton = new QToolButton();
    videoSetTechnicalLogButton->setObjectName("videoSetTechnicalLogToggle");
    videoSetTechnicalLogButton->setText("Show technical log");
    videoSetTechnicalLogButton->setCheckable(true);
    videoSetTechnicalLogButton->setArrowType(Qt::RightArrow);
    activityLayout->addWidget(videoSetTechnicalLogButton);
    videoSetLog->setObjectName("videoSetTechnicalLog");
    videoSetLog->setVisible(false);
    videoSetLog->setFontFamily("Consolas");
    videoSetLog->setMaximumHeight(180);
    videoSetLog->document()->setMaximumBlockCount(5000);
    activityLayout->addWidget(videoSetLog);

    // Keep every established manual control in a collapsed Classic area.
    root->removeWidget(classicEncodeGroup);
    root->removeWidget(videoSetPlanTable);
    root->removeWidget(videoSetProgress);
    root->removeWidget(classicRecoveryGroup);
    videoSetClassicToolsGroup = new QGroupBox("Advanced / Classic Video Set Tools");
    videoSetClassicToolsGroup->setObjectName("videoSetClassicTools");
    videoSetClassicToolsGroup->setCheckable(true);
    auto *classicLayout = new QVBoxLayout(videoSetClassicToolsGroup);
    classicLayout->addWidget(classicEncodeGroup);
    classicLayout->addWidget(videoSetPlanTable);
    classicLayout->addWidget(videoSetProgress);
    classicLayout->addWidget(classicRecoveryGroup);
    root->addWidget(videoSetClassicToolsGroup);

    const QSettings settings;
    const bool advancedVisible = settings.value(
        "videoSet/advancedVisible", false).toBool();
    videoSetAdvancedSettingsButton->setChecked(advancedVisible);
    videoSetAdvancedSettingsWidget->setVisible(advancedVisible);
    videoSetAdvancedSettingsButton->setArrowType(
        advancedVisible ? Qt::DownArrow : Qt::RightArrow);
    const bool classicVisible = settings.value(
        "videoSet/classicVisible", false).toBool();
    videoSetClassicToolsGroup->setChecked(classicVisible);
    videoSetClassicToolsGroup->setVisible(false);
    classicEncodeGroup->setVisible(classicVisible);
    videoSetPlanTable->setVisible(classicVisible);
    videoSetProgress->setVisible(classicVisible);
    classicRecoveryGroup->setVisible(classicVisible);
    videoSetAssistantOutputEdit->setText(settings.value(
        "ui/defaultVideoSetOutputFolder",
        settings.value("videoSet/lastOutputRoot",
            QStandardPaths::writableLocation(
                QStandardPaths::DocumentsLocation))).toString());
    videoSetPlaylistUrlEdit->setText(settings.value(
        "videoSet/lastPlaylistUrl").toString());
    videoSetAssistantRecoveryOutputEdit->setText(settings.value(
        "videoSet/lastRecoveryOutput",
        QStandardPaths::writableLocation(
            QStandardPaths::DocumentsLocation)).toString());

    videoSetPlanDebounceTimer = new QTimer(this);
    videoSetPlanDebounceTimer->setSingleShot(true);
    videoSetPlanDebounceTimer->setInterval(450);
    connect(videoSetPlanDebounceTimer, &QTimer::timeout,
            this, &DriveManagerUI::calculateVideoSetPlan);
    videoSetOperationTimer = new QTimer(this);
    videoSetOperationTimer->setInterval(1000);
    connect(videoSetOperationTimer, &QTimer::timeout, this, [this]() {
        (void) videoSetOperationProgress.tick(
            QDateTime::currentMSecsSinceEpoch());
        renderVideoSetActivity();
    });
    videoSetOperationTimer->start();

    connect(videoSetWelcomeCreateButton, &QPushButton::clicked, this, [this]() {
        showVideoSetCreate();
    });
    connect(videoSetWelcomeRecoverButton, &QPushButton::clicked, this, [this]() {
        showVideoSetRecover();
    });
    connect(videoSetRecentEmptyCreateButton, &QPushButton::clicked,
            this, &DriveManagerUI::showVideoSetCreate);
    connect(videoSetRecentEmptyRecoverButton, &QPushButton::clicked,
            this, &DriveManagerUI::showVideoSetRecover);
    connect(sourceBack, &QPushButton::clicked, this, [this]() {
        videoSetWorkflow.reset();
        videoSetAssistantStack->setCurrentIndex(0);
        updateVideoSetAssistant();
    });
    const auto updateSource = [this]() {
        const QFileInfo file(videoSetAssistantInputEdit->text());
        const bool readable = file.exists() && file.isFile() && file.isReadable();
        const bool outputReady = !videoSetAssistantOutputEdit->text().trimmed().isEmpty();
        if (readable) {
            videoSetSourceInfoLabel->setText(
                tr("Name: %1\nSize: %2\nType: %3\nPath: %4\nThe original file will remain unchanged.")
                    .arg(file.fileName(),
                         QLocale().formattedDataSize(file.size()),
                         file.suffix().isEmpty()
                            ? tr("File")
                            : file.suffix().toUpper(),
                         file.absoluteFilePath()));
            videoSetSourceInfoLabel->setToolTip(file.absoluteFilePath());
            videoSetAssistantInputBrowseButton->setText(tr("Change"));
            videoSetWorkflow.select_source(
                file.fileName().toStdString(),
                static_cast<uint64_t>(file.size()));
        } else {
            videoSetSourceInfoLabel->setText(
                videoSetAssistantInputEdit->text().isEmpty()
                    ? tr("No file selected.")
                    : tr("This source file is missing or cannot be read."));
            videoSetAssistantInputBrowseButton->setText(tr("Choose file"));
        }
        videoSetSourceContinueButton->setEnabled(readable && outputReady);
        updateVideoSetAssistant();
    };
    connect(videoSetAssistantInputEdit, &QLineEdit::textChanged,
            this, updateSource);
    connect(videoSetAssistantOutputEdit, &QLineEdit::textChanged,
            this, updateSource);
    connect(videoSetAssistantInputBrowseButton, &QPushButton::clicked,
            this, [this]() {
        const auto file = QFileDialog::getOpenFileName(
            this, "Choose the file to turn into videos",
            QFileInfo(videoSetAssistantInputEdit->text()).absolutePath());
        if (!file.isEmpty()) videoSetAssistantInputEdit->setText(file);
    });
    connect(videoSetAssistantOutputBrowseButton, &QPushButton::clicked,
            this, [this]() {
        const auto folder = QFileDialog::getExistingDirectory(
            this, "Choose Video Set output folder",
            videoSetAssistantOutputEdit->text());
        if (!folder.isEmpty()) videoSetAssistantOutputEdit->setText(folder);
    });
    connect(videoSetSourceContinueButton, &QPushButton::clicked,
            this, [this]() {
        videoSetAssistantStack->setCurrentIndex(2);
        updateVideoSetAssistant();
    });
    connect(modeBack, &QPushButton::clicked, this, [this]() {
        videoSetAssistantStack->setCurrentIndex(1);
        updateVideoSetAssistant();
    });
    const auto profileChanged = [this]() {
        const bool high = videoSetHighCapacityRadio->isChecked();
        videoSetWorkflow.select_profile(
            high ? "high-capacity" : "resilient",
            QString::fromStdString(reliability_profile_config_id(
                high ? ReliabilityProfile::HighCapacity
                     : ReliabilityProfile::Local)).toStdString());
        videoSetAdvancedProfileLabel->setText(high
            ? "High Capacity: 4x4, 1-bit, signal 1.0, repair 5%, 1920x1080 at 30 FPS; config 538F2B009FAB"
            : "Resilient: 8x8, 1-bit, signal 1.0, repair 5%, 1920x1080 at 30 FPS");
        updateProfileCardVisuals();
        updateVideoSetAssistant();
    };
    connect(videoSetResilientRadio, &QRadioButton::toggled,
            this, profileChanged);
    connect(videoSetAdvancedSettingsButton, &QToolButton::toggled,
            this, [this](const bool checked) {
        videoSetAdvancedSettingsWidget->setVisible(checked);
        videoSetAdvancedSettingsButton->setArrowType(
            checked ? Qt::DownArrow : Qt::RightArrow);
    });
    const auto invalidatePlan = [this]() {
        videoSetWorkflow.invalidate_plan();
        if (videoSetAssistantStack->currentIndex() == 3)
            videoSetPlanDebounceTimer->start();
        updateVideoSetAssistant();
    };
    connect(videoSetAssistantTargetSpin,
            QOverload<int>::of(&QSpinBox::valueChanged), this,
            [invalidatePlan](int) { invalidatePlan(); });
    connect(videoSetAssistantMaximumSizeSpin,
            QOverload<int>::of(&QSpinBox::valueChanged), this,
            [invalidatePlan](int) { invalidatePlan(); });
    connect(videoSetAssistantReserveSpin,
            QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
            [invalidatePlan](double) { invalidatePlan(); });
    connect(videoSetModeContinueButton, &QPushButton::clicked,
            this, &DriveManagerUI::calculateVideoSetPlan);
    connect(planBack, &QPushButton::clicked, this, [this]() {
        videoSetAssistantStack->setCurrentIndex(2);
        updateVideoSetAssistant();
    });
    connect(videoSetPartDetailsButton, &QToolButton::toggled,
            this, [this](const bool checked) {
        videoSetAssistantPlanTable->setVisible(checked);
        videoSetPartDetailsButton->setText(
            checked ? "Hide part details" : "Show part details");
        videoSetPartDetailsButton->setArrowType(
            checked ? Qt::DownArrow : Qt::RightArrow);
    });
    connect(videoSetCreateVideosButton, &QPushButton::clicked,
            this, [this]() { startVideoSetEncode(false); });
    connect(videoSetProgressResumeButton, &QPushButton::clicked,
            this, [this]() { startVideoSetEncode(true); });
    connect(videoSetProgressContinueButton, &QPushButton::clicked,
            this, [this]() {
        videoSetWorkflow.show_upload_guide();
        updateVideoSetAssistant();
    });
    connect(videoSetProgressOpenFolderButton, &QPushButton::clicked,
            this, [this]() {
        if (!videoSetCurrentSetRoot.isEmpty())
            QDesktopServices::openUrl(QUrl::fromLocalFile(
                QDir(videoSetCurrentSetRoot).filePath("videos")));
    });
    connect(videoSetAssistantCancelButton, &QPushButton::clicked,
            this, [this]() {
        if (videoSetDownloadProcess &&
            videoSetDownloadProcess->state() != QProcess::NotRunning) {
            if (QMessageBox::question(
                    this, "Cancel playlist download",
                    "Stop the yt-dlp child process? Already downloaded files "
                    "will remain in the returned folder.",
                    QMessageBox::Yes | QMessageBox::No,
                    QMessageBox::No) != QMessageBox::Yes) return;
            (void) videoSetOperationProgress.request_cancel(
                QDateTime::currentMSecsSinceEpoch());
            renderVideoSetActivity();
            videoSetCancelRequested = true;
            videoSetDownloadProcess->terminate();
            QTimer::singleShot(3000, videoSetDownloadProcess, [this]() {
                if (videoSetDownloadProcess &&
                    videoSetDownloadProcess->state() != QProcess::NotRunning)
                    videoSetDownloadProcess->kill();
            });
            return;
        }
        if (!videoSetProcess ||
            videoSetProcess->state() == QProcess::NotRunning) return;
        if (QMessageBox::question(
                this, "Pause Video Set operation",
                "Stop the current operation safely? Completed verified parts "
                "will be kept so you can continue later.",
                QMessageBox::Yes | QMessageBox::No,
                QMessageBox::No) != QMessageBox::Yes) return;
        videoSetCancelRequested = true;
        (void) videoSetOperationProgress.request_cancel(
            QDateTime::currentMSecsSinceEpoch());
        renderVideoSetActivity();
        videoSetProcess->terminate();
        QTimer::singleShot(3000, videoSetProcess, [this]() {
            if (videoSetProcess &&
                videoSetProcess->state() != QProcess::NotRunning)
                videoSetProcess->kill();
        });
    });
    connect(videoSetActivityRetryButton, &QPushButton::clicked,
            this, [this]() {
        if (videoSetLastAssistantArguments.isEmpty()) return;
        const auto command = videoSetLastAssistantArguments.front();
        if (command == "set-inspect") startVideoSetScan();
        else if (command == "set-recover") startVideoSetRecovery(true);
        else if (command == "set-encode") startVideoSetEncode(true);
        else if (command == "set-plan") calculateVideoSetPlan();
        else if (command == "yt-dlp") startVideoSetDownload();
    });
    connect(videoSetOpenVideosButton, &QPushButton::clicked, this, [this]() {
        if (!videoSetCurrentSetRoot.isEmpty())
            QDesktopServices::openUrl(QUrl::fromLocalFile(
                QDir(videoSetCurrentSetRoot).filePath("videos")));
    });
    connect(videoSetOpenYouTubeButton, &QPushButton::clicked, this, []() {
        QDesktopServices::openUrl(QUrl(QStringLiteral(
            "https://www.youtube.com/upload")));
    });
    connect(videoSetOpenChecklistButton, &QPushButton::clicked,
            this, [this]() {
        if (!videoSetCurrentSetRoot.isEmpty())
            QDesktopServices::openUrl(QUrl::fromLocalFile(
                QDir(videoSetCurrentSetRoot).filePath("upload_checklist.md")));
    });
    connect(videoSetUploadedButton, &QPushButton::clicked, this, [this]() {
        videoSetWorkflow.acknowledge_upload();
        updateVideoSetAssistant();
    });
    connect(videoSetYouTubeSyncButton, &QPushButton::clicked,
            this, &DriveManagerUI::startYouTubeSync);
    connect(videoSetYouTubeSyncPauseButton, &QPushButton::clicked,
            this, [this]() {
        youtubeSyncOperation.clear();
        youtubeProcessingStartedMs = 0;
        youtubeProcessingPollAttempt = 0;
        if (youtubeReadinessProcess &&
            youtubeReadinessProcess->state() != QProcess::NotRunning)
            youtubeReadinessProcess->terminate();
        videoSetYouTubeSyncPauseButton->setEnabled(false);
        videoSetYouTubeSyncButton->setEnabled(true);
        videoSetYouTubeSyncButton->setText(tr("Resume Upload"));
        videoSetYouTubeSyncStatus->setText(tr(
            "Upload paused. Already uploaded videos remain on YouTube."));
        for (auto &part : youtubeRuntimeSyncState.parts)
            if (part.upload_state == youtube_sync::UploadState::Uploading ||
                part.upload_state == youtube_sync::UploadState::SessionCreated)
                part.upload_state = youtube_sync::UploadState::Paused;
        try {
            youtube_sync::write_sync_state_atomic(
                std::filesystem::path(youtubeSyncStatePath.toStdWString()),
                youtubeRuntimeSyncState);
        } catch (...) {}
    });
    connect(videoSetPlaylistUrlEdit, &QLineEdit::textChanged,
            this, [this](const QString &value) {
        const bool valid = video_set_workflow::is_youtube_playlist_url(
            value.toStdString());
        videoSetDownloadButton->setEnabled(valid);
        videoSetDownloadStatusLabel->setText(valid
            ? "Ready to download into this set's returned folder."
            : "Enter a valid YouTube playlist URL containing list=.");
    });
    connect(videoSetDownloadButton, &QPushButton::clicked,
            this, &DriveManagerUI::startVideoSetDownload);
    connect(videoSetSelectYtDlpButton, &QPushButton::clicked,
            this, [this]() {
        const auto executable = QFileDialog::getOpenFileName(
            this, "Select yt-dlp executable", {},
#ifdef Q_OS_WIN
            "Executable (yt-dlp.exe);;All files (*)"
#else
            "yt-dlp executable (yt-dlp);;All files (*)"
#endif
        );
        if (!executable.isEmpty()) {
            QSettings().setValue("videoSet/ytdlpPath", executable);
            videoSetDownloadStatusLabel->setText(
                "yt-dlp selected. Paste a playlist URL to continue.");
        }
    });
    connect(videoSetManualReturnedButton, &QPushButton::clicked,
            this, [this]() {
        const auto folder = QFileDialog::getExistingDirectory(
            this, "Choose returned videos folder",
            videoSetAssistantRecoveryInputEdit->text());
        if (!folder.isEmpty()) {
            videoSetAssistantRecoveryInputEdit->setText(folder);
            videoSetAssistantStack->setCurrentIndex(7);
            videoSetScanSummaryLabel->setText(tr(
                "Returned videos selected. Choose Check Videos when you are ready."));
            videoSetAssistantScanButton->setFocus();
        }
    });
    connect(scanBrowse, &QPushButton::clicked, this, [this]() {
        const auto folder = QFileDialog::getExistingDirectory(
            this, "Choose Video Set or returned videos folder",
            videoSetAssistantRecoveryInputEdit->text());
        if (!folder.isEmpty()) videoSetAssistantRecoveryInputEdit->setText(folder);
    });
    connect(scanFileBrowse, &QPushButton::clicked, this, [this]() {
        const auto file = QFileDialog::getOpenFileName(
            this, "Choose set_manifest.json or a Video Set video",
            QFileInfo(videoSetAssistantRecoveryInputEdit->text()).absolutePath(),
            "Video Set inputs (set_manifest.json *.mkv *.mp4 *.webm *.avi *.mov);;All files (*)");
        if (!file.isEmpty()) videoSetAssistantRecoveryInputEdit->setText(file);
    });
    connect(scanOutputBrowse, &QPushButton::clicked, this, [this]() {
        const auto folder = QFileDialog::getExistingDirectory(
            this, "Choose recovered output folder",
            videoSetAssistantRecoveryOutputEdit->text());
        if (!folder.isEmpty()) videoSetAssistantRecoveryOutputEdit->setText(folder);
    });
    connect(videoSetAssistantScanButton, &QPushButton::clicked,
            this, &DriveManagerUI::startVideoSetScan);
    connect(videoSetInstantPlaylistEdit, &QLineEdit::textChanged,
            this, [this](const QString &url) {
        const bool valid = video_set_workflow::is_youtube_playlist_url(
            url.toStdString());
        videoSetInstantRecoverButton->setEnabled(valid);
        videoSetInstantRecoveryStatus->setText(valid
            ? tr("Ready. VidStoreX will download, identify one complete set, recover it, and verify the full-file SHA-256.")
            : tr("Paste a valid YouTube playlist link containing list=."));
    });
    connect(videoSetInstantRecoverButton, &QPushButton::clicked,
            this, [this]() {
        const QString url = videoSetInstantPlaylistEdit->text().trimmed();
        if (!video_set_workflow::is_youtube_playlist_url(url.toStdString()))
            return;
        const QString output = videoSetAssistantRecoveryOutputEdit->text().trimmed();
        if (output.isEmpty() || !QDir().mkpath(output)) {
            videoSetInstantRecoveryStatus->setText(tr(
                "Choose a writable recovered-file folder before continuing."));
            return;
        }
        try {
            const auto root = instant_recovery::default_jobs_root() /
                std::filesystem::u8path(instant_recovery::make_job_id());
            instant_recovery::initialize_job_directories(root);
            instant_recovery::JobState state;
            state.job_id = root.filename().string();
            state.playlist_url = url.toStdString();
            state.output_directory = output.toStdString();
            state.phase = instant_recovery::Phase::Downloading;
            state.updated_at_epoch_seconds = QDateTime::currentSecsSinceEpoch();
            instant_recovery::write_job_state_atomic(root / "job_state.json", state);
            videoSetInstantRecoveryJobRoot = QString::fromStdWString(root.wstring());
            videoSetCurrentSetRoot = videoSetInstantRecoveryJobRoot;
            videoSetPlaylistUrlEdit->setText(url);
            videoSetInstantRecoveryActive = true;
            videoSetInstantRecoveryStatus->setText(tr("Downloading playlist videos..."));
            startVideoSetDownload();
        } catch (const std::exception &error) {
            videoSetInstantRecoveryStatus->setText(
                tr("Recovery job could not be prepared: %1")
                    .arg(QString::fromUtf8(error.what())));
        }
    });
    connect(videoSetAssistantRecoverButton, &QPushButton::clicked,
            this, [this]() { startVideoSetRecovery(true); });
    connect(videoSetOpenReturnedButton, &QPushButton::clicked,
            this, [this]() {
        const QFileInfo input(videoSetAssistantRecoveryInputEdit->text());
        const auto folder = input.isDir() ? input.absoluteFilePath()
                                         : input.absolutePath();
        if (!folder.isEmpty())
            QDesktopServices::openUrl(QUrl::fromLocalFile(folder));
    });
    connect(videoSetOpenRecoveredButton, &QPushButton::clicked,
            this, [this]() {
        const QFileInfo output(
            QString::fromStdString(videoSetWorkflow.view().final_output_path));
        if (!output.absolutePath().isEmpty())
            QDesktopServices::openUrl(QUrl::fromLocalFile(
                    output.absolutePath()));
    });
    connect(videoSetOpenSetFolderButton, &QPushButton::clicked,
            this, [this]() {
        if (!videoSetCurrentSetRoot.isEmpty())
            QDesktopServices::openUrl(QUrl::fromLocalFile(
                videoSetCurrentSetRoot));
    });
    connect(videoSetCopyShaButton, &QPushButton::clicked, this, [this]() {
        QApplication::clipboard()->setText(videoSetFinalSha);
    });
    connect(videoSetReturnHomeButton, &QPushButton::clicked, this, [this]() {
        videoSetWorkflow.reset();
        videoSetAssistantStack->setCurrentIndex(0);
        refreshRecentVideoSets();
        updateVideoSetAssistant();
    });
    connect(videoSetTechnicalLogButton, &QToolButton::toggled,
            this, [this](const bool checked) {
        videoSetLog->setVisible(checked);
        videoSetTechnicalLogButton->setText(
            checked ? "Hide technical log" : "Show technical log");
        videoSetTechnicalLogButton->setArrowType(
            checked ? Qt::DownArrow : Qt::RightArrow);
    });
    connect(videoSetClassicToolsGroup, &QGroupBox::toggled,
            this, [=](const bool checked) {
        classicEncodeGroup->setVisible(checked);
        videoSetPlanTable->setVisible(checked);
        videoSetProgress->setVisible(checked);
        classicRecoveryGroup->setVisible(checked);
    });
    connect(videoSetRecentContinueButton, &QPushButton::clicked,
            this, [this]() {
        if (auto *item = videoSetRecentList->currentItem())
            openRecentVideoSet(item->data(Qt::UserRole).toString());
    });
    connect(videoSetRecentOpenFolderButton, &QPushButton::clicked,
            this, [this]() {
        if (auto *item = videoSetRecentList->currentItem()) {
            const QFileInfo manifest(item->data(Qt::UserRole).toString());
            if (manifest.exists())
                QDesktopServices::openUrl(QUrl::fromLocalFile(
                    manifest.absolutePath()));
        }
    });
    connect(recentRemoveAction, &QAction::triggered,
            this, [this]() {
        auto *item = videoSetRecentList->currentItem();
        if (!item) return;
        QSettings settings;
        const QString path = item->data(Qt::UserRole).toString();
        auto paths = settings.value("videoSet/recentManifests").toStringList();
        paths.removeAll(path);
        settings.setValue("videoSet/recentManifests", paths);
        settings.remove(recent_opened_setting_key(path));
        refreshRecentVideoSets();
    });
    connect(recentShowManifestAction, &QAction::triggered,
            this, [this]() {
        if (const auto *item = videoSetRecentList->currentItem()) {
            const QString path = item->data(Qt::UserRole).toString();
            if (QFileInfo::exists(path))
                QDesktopServices::openUrl(QUrl::fromLocalFile(path));
        }
    });
    connect(recentCopyManifestAction, &QAction::triggered,
            this, [this]() {
        if (const auto *item = videoSetRecentList->currentItem())
            QApplication::clipboard()->setText(
                item->data(Qt::UserRole).toString());
    });
    connect(recentOpenReportAction, &QAction::triggered,
            this, [this]() {
        if (const auto *item = videoSetRecentList->currentItem()) {
            const QFileInfo manifest(item->data(Qt::UserRole).toString());
            const QStringList candidates{
                QDir(manifest.absolutePath()).filePath("recovery_report.md"),
                QDir(manifest.absolutePath()).filePath("reports/report.md")};
            for (const auto &candidate : candidates) {
                if (QFileInfo::exists(candidate)) {
                    QDesktopServices::openUrl(QUrl::fromLocalFile(candidate));
                    return;
                }
            }
        }
    });
    connect(videoSetRecentList, &QListWidget::itemDoubleClicked,
            this, [this](QListWidgetItem *item) {
        openRecentVideoSet(item->data(Qt::UserRole).toString());
    });

    videoSetDownloadProcess = new QProcess(this);
    videoSetDownloadProcess->setProcessChannelMode(QProcess::MergedChannels);
    connect(videoSetDownloadProcess, &QProcess::readyReadStandardOutput,
            this, [this]() {
        const QString text = QString::fromLocal8Bit(
            videoSetDownloadProcess->readAllStandardOutput());
        videoSetLog->append("[yt-dlp] " + text.trimmed());
        const auto progress = video_set_workflow::parse_ytdlp_progress(
            text.toStdString());
        video_set_workflow::OperationEvent event;
        event.operation_id = videoSetOperationProgress.view().operation_id;
        event.operation_type = video_set_workflow::OperationType::Download;
        event.phase = video_set_workflow::OperationPhase::Preparing;
        event.primary_message = "Downloading processed videos";
        if (progress.percent.has_value()) {
            videoSetDownloadProgress->setRange(0, 100);
            videoSetDownloadProgress->setValue(
                static_cast<int>(*progress.percent));
            event.progress_is_determinate = true;
            event.progress_current = static_cast<uint64_t>((std::clamp)(
                *progress.percent, 0.0, 100.0) * 10.0);
            event.progress_total = 1000;
        }
        if (progress.current_item.has_value() &&
            progress.total_items.has_value())
            videoSetDownloadStatusLabel->setText(
                QString("Downloading playlist item %1 of %2...")
                    .arg(*progress.current_item)
                    .arg(*progress.total_items));
        if (progress.current_item) event.current_index = *progress.current_item;
        if (progress.total_items) event.total_items = *progress.total_items;
        if (!progress.destination_filename.empty())
            event.current_item_name = progress.destination_filename;
        if (progress.eta_seconds)
            event.estimated_remaining_seconds = *progress.eta_seconds;
        QStringList details;
        if (progress.downloaded_bytes && progress.total_bytes)
            details << QString("%1 of %2")
                .arg(QLocale().formattedDataSize(
                    static_cast<qint64>(*progress.downloaded_bytes)))
                .arg(QLocale().formattedDataSize(
                    static_cast<qint64>(*progress.total_bytes)));
        if (progress.speed_bytes_per_second)
            details << QString("%1/s").arg(QLocale().formattedDataSize(
                static_cast<qint64>(*progress.speed_bytes_per_second)));
        event.secondary_message = details.join("   ").toStdString();
        (void) videoSetOperationProgress.apply(
            event, QDateTime::currentMSecsSinceEpoch());
        renderVideoSetActivity();
    });
    connect(videoSetDownloadProcess,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this](const int code, const QProcess::ExitStatus status) {
        videoSetDownloadButton->setEnabled(true);
        videoSetAssistantCancelButton->setEnabled(false);
        if (status == QProcess::NormalExit && code == 0) {
            (void) videoSetOperationProgress.complete(
                videoSetOperationProgress.view().operation_id,
                QDateTime::currentMSecsSinceEpoch(),
                "Download complete");
            videoSetDownloadProgress->setRange(0, 100);
            videoSetDownloadProgress->setValue(100);
            videoSetDownloadStatusLabel->setText(
                tr("Download complete. Choose Check Videos to inspect the returned copies."));
            const auto returned = QDir(videoSetCurrentSetRoot).filePath(
                "returned");
            videoSetAssistantRecoveryInputEdit->setText(returned);
            videoSetAssistantStack->setCurrentIndex(7);
            videoSetWorkflow.cancel_download();
            updateVideoSetAssistant();
            if (videoSetInstantRecoveryActive) {
                videoSetInstantRecoveryStatus->setText(
                    tr("Download complete. Checking embedded Video Set information..."));
                try {
                    auto state = instant_recovery::read_job_state(
                        std::filesystem::path(
                            videoSetInstantRecoveryJobRoot.toStdWString()) /
                        "job_state.json");
                    state.phase = instant_recovery::Phase::Scanning;
                    state.updated_at_epoch_seconds = QDateTime::currentSecsSinceEpoch();
                    instant_recovery::write_job_state_atomic(
                        std::filesystem::path(
                            videoSetInstantRecoveryJobRoot.toStdWString()) /
                        "job_state.json", state);
                } catch (...) {}
                QTimer::singleShot(0, this,
                    &DriveManagerUI::startVideoSetScan);
            } else {
                videoSetAssistantScanButton->setFocus();
            }
        } else {
            if (videoSetInstantRecoveryActive)
                videoSetInstantRecoveryStatus->setText(tr(
                    "Playlist download failed. Retry or continue with the manual recovery controls."));
            if (videoSetCancelRequested)
                (void) videoSetOperationProgress.cancel(
                    videoSetOperationProgress.view().operation_id,
                    QDateTime::currentMSecsSinceEpoch());
            else
                (void) videoSetOperationProgress.fail(
                    videoSetOperationProgress.view().operation_id,
                    QDateTime::currentMSecsSinceEpoch(), code,
                    "The playlist download stopped.",
                    "Retry, choose yt-dlp again, or download the videos manually.");
            videoSetWorkflow.cancel_download();
            videoSetDownloadStatusLabel->setText(
                QString("Download stopped with exit code %1. Retry, choose yt-dlp, or download manually.")
                    .arg(code));
            updateVideoSetAssistant();
        }
    });
    connect(videoSetDownloadProcess, &QProcess::errorOccurred,
            this, [this](const QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart) return;
        videoSetWorkflow.cancel_download();
        (void) videoSetOperationProgress.fail(
            videoSetOperationProgress.view().operation_id,
            QDateTime::currentMSecsSinceEpoch(), -1,
            "yt-dlp could not be started.",
            "Select the executable again or download the videos manually.");
        videoSetDownloadButton->setEnabled(true);
        videoSetAssistantCancelButton->setEnabled(false);
        videoSetDownloadProgress->setRange(0, 100);
        videoSetDownloadProgress->setValue(0);
        videoSetDownloadStatusLabel->setText(
            "yt-dlp could not be started: " +
            videoSetDownloadProcess->errorString() +
            ". Select the executable again or download the videos manually.");
        updateVideoSetAssistant();
    });

    const QSet<QString> dynamicTextObjects{
        "videoSetAssistantStepIndicator",
        "videoSetAssistantPrimaryMessage",
        "videoSetAssistantSuggestedAction",
        "videoSetActivityIcon",
        "videoSetActivityTitle",
        "videoSetElapsedTime",
        "videoSetEstimatedRemaining",
        "videoSetActivityDescription",
        "videoSetProgressCounter",
        "videoSetCurrentItem",
        "videoSetAssistantSourceInfo",
        "videoSetAssistantChooseFile",
        "videoSetAssistantPlanSummary",
        "videoSetAssistantPhase",
        "videoSetAssistantCurrentPart",
        "videoSetAssistantScanSummary",
        "videoSetAssistantScanCounts",
        "videoSetRecoveryAvailability",
        "videoSetAssistantRecoveryPhase",
        "videoSetAssistantExactSuccess",
        "videoSetAdvancedProfileDetails",
        "videoSetDownloadStatus",
        "videoSetAdvancedSettingsToggle",
        "videoSetPartDetailsToggle",
        "videoSetTechnicalLogToggle",
        "videoSetResilientChoice",
        "videoSetHighCapacityChoice"};
    const auto rememberTranslationSource = [&dynamicTextObjects](
        QWidget *widget) {
        if (!widget || dynamicTextObjects.contains(widget->objectName()))
            return;
        QString text;
        if (auto *button = qobject_cast<QAbstractButton *>(widget))
            text = button->text();
        else if (auto *label = qobject_cast<QLabel *>(widget))
            text = label->text();
        else if (auto *group = qobject_cast<QGroupBox *>(widget))
            text = group->title();
        if (!text.isEmpty()) widget->setProperty("i18nSource", text);
        if (auto *edit = qobject_cast<QLineEdit *>(widget)) {
            if (!edit->placeholderText().isEmpty())
                edit->setProperty(
                    "i18nPlaceholder", edit->placeholderText());
        }
    };
    rememberTranslationSource(videoSetPage);
    for (auto *widget : videoSetPage->findChildren<QWidget *>())
        rememberTranslationSource(widget);

    refreshRecentVideoSets();
    videoSetAssistantStack->setCurrentIndex(0);
    updateSource();
    updateProfileCardVisuals();
    updateVideoSetAssistant();
    setTabOrder(videoSetWelcomeCreateButton, videoSetWelcomeRecoverButton);
    setTabOrder(videoSetWelcomeRecoverButton, videoSetRecentList);
    setTabOrder(videoSetAssistantInputEdit, videoSetAssistantInputBrowseButton);
    setTabOrder(videoSetAssistantInputBrowseButton, videoSetAssistantOutputEdit);
    setTabOrder(videoSetAssistantOutputEdit, videoSetSourceContinueButton);
}

QStringList DriveManagerUI::videoSetEncodeArguments(
    const QString &command) const {
    return {command,
            videoSetAssistantInputEdit->text(),
            videoSetAssistantOutputEdit->text(),
            "--reliability-profile",
            videoSetHighCapacityRadio->isChecked()
                ? "high-capacity" : "resilient",
            "--target-duration-seconds",
            QString::number(videoSetAssistantTargetSpin->value()),
            "--max-video-size-mib",
            QString::number(videoSetAssistantMaximumSizeSpin->value()),
            "--reserve-percent",
            QString::number(videoSetAssistantReserveSpin->value())};
}

void DriveManagerUI::startVideoSetProcess(
    const QStringList &arguments,
    const bool assistantOperation) {
    if (!videoSetProcess ||
        videoSetProcess->state() != QProcess::NotRunning ||
        !videoSetDownloadProcess ||
        videoSetDownloadProcess->state() != QProcess::NotRunning)
        return;
    if (arguments.isEmpty()) return;
    videoSetAssistantOperation = assistantOperation;
    videoSetActiveCommand = arguments.front();
    videoSetProcessBuffer.clear();
    videoSetProgressLineBuffer.clear();
    videoSetCancelRequested = false;
    videoSetLog->clear();
    videoSetAssistantCancelButton->setEnabled(true);
    videoSetProgressContinueButton->setEnabled(false);
    videoSetProgressResumeButton->setEnabled(false);
    videoSetAssistantScanButton->setEnabled(false);
    videoSetAssistantRecoverButton->setEnabled(false);
    QStringList launchArguments = arguments;
    if (assistantOperation) {
        videoSetLastAssistantArguments = arguments;
        using video_set_workflow::OperationPhase;
        using video_set_workflow::OperationType;
        OperationType type = OperationType::None;
        OperationPhase phase = OperationPhase::Preparing;
        QString title = "Preparing Video Set operation";
        QString description;
        if (videoSetActiveCommand == "set-plan") {
            type = OperationType::Plan;
            phase = OperationPhase::HashingSource;
            title = "Calculating the Video Set plan";
            description = "VidStoreX is reading the source and calculating real part capacity.";
        } else if (videoSetActiveCommand == "set-encode") {
            type = OperationType::Encode;
            title = "Creating and verifying videos";
            description = "Each completed video is decoded and verified locally.";
        } else if (videoSetActiveCommand == "set-inspect") {
            type = OperationType::Scan;
            phase = OperationPhase::DiscoveringFiles;
            title = "Scanning downloaded videos";
            description = "VidStoreX is checking the videos and reading their embedded Video Set information. The original file is not being rebuilt yet.";
        } else if (videoSetActiveCommand == "set-recover") {
            type = OperationType::Recover;
            title = "Recovering the original file";
            description = "Verified parts are being decoded and written back into the original file.";
        }
        const auto operationId = videoSetOperationProgress.begin(
            type, phase, QDateTime::currentMSecsSinceEpoch(),
            title.toStdString(), description.toStdString());
        launchArguments << "--progress-format" << "jsonl"
                        << "--operation-id"
                        << QString::number(operationId);
        renderVideoSetActivity();
    }
#ifdef Q_OS_WIN
    const QString executable =
        QCoreApplication::applicationDirPath() + "/media_storage.exe";
#else
    const QString executable =
        QCoreApplication::applicationDirPath() + "/media_storage";
#endif
    videoSetProcess->start(executable, launchArguments);
}

void DriveManagerUI::handleVideoSetProgressOutput(const QString &text) {
    videoSetProgressLineBuffer += text;
    while (true) {
        const qsizetype newline = videoSetProgressLineBuffer.indexOf('\n');
        if (newline < 0) break;
        QString line = videoSetProgressLineBuffer.left(newline);
        videoSetProgressLineBuffer.remove(0, newline + 1);
        if (line.endsWith('\r')) line.chop(1);
        const auto event = video_set_workflow::parse_progress_jsonl(
            line.toUtf8().toStdString());
        if (!event.has_value()) {
            if (!line.trimmed().isEmpty())
                videoSetLog->append(line.toHtmlEscaped());
            continue;
        }
        if (!videoSetOperationProgress.apply(
                *event, QDateTime::currentMSecsSinceEpoch()))
            continue;
        if (event->operation_type ==
            video_set_workflow::OperationType::Scan)
            videoSetActivityPanel->setProperty("observedScan", true);
        if (event->operation_type ==
            video_set_workflow::OperationType::Recover)
            videoSetActivityPanel->setProperty("observedRecovery", true);
        if (event->phase ==
            video_set_workflow::OperationPhase::CheckingFullFile)
            videoSetActivityPanel->setProperty("observedFinalHash", true);
        renderVideoSetActivity();
    }
}

void DriveManagerUI::renderVideoSetActivity() {
    if (!videoSetActivityPanel) return;
    const auto &operation = videoSetOperationProgress.view();
    const bool hasOperation = operation.state !=
        video_set_workflow::OperationState::Idle;
    const bool visible = hasOperation &&
        videoSetWorkflow.view().state !=
            video_set_workflow::State::RecoveredExact;
    videoSetActivityPanel->setVisible(visible);
    if (!hasOperation) return;

    QString title;
    QString description = translatedWorkflowText(
        operation.secondary_message);
    switch (operation.operation_type) {
        case video_set_workflow::OperationType::Plan:
            title = tr("Calculating the Video Set plan");
            break;
        case video_set_workflow::OperationType::Encode:
            title = tr("Creating and verifying videos");
            break;
        case video_set_workflow::OperationType::Download:
            title = tr("Downloading processed videos");
            break;
        case video_set_workflow::OperationType::Scan:
            title = tr("Scanning downloaded videos");
            description = tr("VidStoreX is checking the videos and reading their embedded Video Set information. The original file is not being rebuilt yet.");
            break;
        case video_set_workflow::OperationType::Recover:
        case video_set_workflow::OperationType::FinalHash:
        case video_set_workflow::OperationType::Finalize:
            title = tr("Recovering the original file");
            description = operation.phase ==
                video_set_workflow::OperationPhase::CheckingFullFile
                ? tr("VidStoreX is checking the final full-file SHA-256. Recovery succeeds only if it matches.")
                : tr("Verified parts are being decoded and written back into the original file.");
            break;
        default:
            title = translatedWorkflowText(operation.primary_message);
            break;
    }
    if (operation.state == video_set_workflow::OperationState::Cancelling)
        title = tr("Cancelling...");
    videoSetActivityTitle->setText(title);
    videoSetActivityDescription->setText(description);
    videoSetActivityIcon->setText(
        operation.state == video_set_workflow::OperationState::Completed
            ? QString::fromUtf8("✓") : operation.state == video_set_workflow::OperationState::Failed
            ? QStringLiteral("!") : operation.state == video_set_workflow::OperationState::Cancelled
            ? QString::fromUtf8("■") : QString::fromUtf8("●"));

    if (operation.progress_is_determinate && operation.progress_total != 0) {
        videoSetActivityProgress->setRange(0, 1000);
        videoSetActivityProgress->setValue(static_cast<int>((std::min)(
            1000.0, static_cast<double>(operation.progress_current) *
                1000.0 / static_cast<double>(operation.progress_total))));
        videoSetActivityProgress->setFormat("%p%");
    } else if (operation.is_busy) {
        videoSetActivityProgress->setRange(0, 0);
        videoSetActivityProgress->setFormat(QString());
    } else {
        videoSetActivityProgress->setRange(0, 100);
        videoSetActivityProgress->setValue(
            operation.state == video_set_workflow::OperationState::Completed
                ? 100 : 0);
        videoSetActivityProgress->setFormat(
            operation.state == video_set_workflow::OperationState::Completed
                ? tr("Completed") : tr("Inactive"));
    }
    videoSetActivityProgress->setEnabled(operation.is_busy);

    QString counter;
    if (operation.operation_type == video_set_workflow::OperationType::Scan) {
        if (operation.phase ==
            video_set_workflow::OperationPhase::DiscoveringFiles ||
            operation.total_items == 0) {
            counter = tr("Discovering videos...");
        } else {
            counter = tr(
                "Candidates: %1   Checked: %2/%1   Verified: %3   Missing: %4   Corrupt: %5   Duplicates: %6   Conflicts: %7")
                .arg(operation.total_items)
                .arg(operation.current_index)
                .arg(operation.scan.exact_parts)
                .arg(operation.scan.missing_parts.size())
                .arg(operation.scan.corrupt_parts.size())
                .arg(operation.scan.duplicate_count)
                .arg(operation.scan.conflict_count);
        }
    } else if (operation.total_items != 0) {
        counter = tr("Part/item %1 of %2   Completed: %3")
            .arg(operation.current_index)
            .arg(operation.total_items)
            .arg(operation.completed_items);
    } else {
        counter = translatedWorkflowText(operation.primary_message);
    }
    videoSetActivityCounter->setText(counter);
    const QString fullItem = QString::fromStdString(
        operation.current_item_name);
    videoSetActivityCurrentItem->setText(
        QFontMetrics(videoSetActivityCurrentItem->font()).elidedText(
            fullItem, Qt::ElideMiddle, 360));
    videoSetActivityCurrentItem->setToolTip(fullItem);
    const auto formatDuration = [](const double seconds) {
        const auto rounded = static_cast<qint64>((std::max)(0.0, seconds));
        return QString("%1:%2").arg(rounded / 60)
            .arg(rounded % 60, 2, 10, QLatin1Char('0'));
    };
    videoSetActivityElapsed->setText(
        tr("Elapsed: %1").arg(formatDuration(operation.elapsed_seconds)));
    videoSetActivityRemaining->setText(
        operation.estimated_remaining_seconds.has_value()
            ? tr("Remaining: %1").arg(formatDuration(
                *operation.estimated_remaining_seconds))
            : QString());
    videoSetActivityWatchdog->setVisible(
        operation.taking_longer_than_usual);
    videoSetAssistantCancelButton->setEnabled(operation.can_cancel);
    videoSetActivityRetryButton->setVisible(operation.can_retry);
    videoSetActivityRetryButton->setEnabled(operation.can_retry);
    videoSetAssistantScanButton->setEnabled(!operation.is_busy);
    videoSetAssistantRecoverButton->setEnabled(
        !operation.is_busy && videoSetWorkflow.view().can_recover);
    videoSetDownloadButton->setEnabled(!operation.is_busy &&
        video_set_workflow::is_youtube_playlist_url(
            videoSetPlaylistUrlEdit->text().toStdString()));
}

void DriveManagerUI::calculateVideoSetPlan() {
    const QFileInfo source(videoSetAssistantInputEdit->text());
    if (!source.exists() || !source.isFile() || !source.isReadable() ||
        videoSetAssistantOutputEdit->text().trimmed().isEmpty()) {
        videoSetWorkflow.fail(
            "The source file or output folder is not ready.",
            "Choose a readable file and a writable output folder.");
        updateVideoSetAssistant();
        return;
    }
    videoSetWorkflow.begin_planning();
    videoSetAssistantStack->setCurrentIndex(3);
    videoSetAssistantPlanTable->setRowCount(0);
    videoSetPlanSummaryLabel->setText(tr("Calculating plan..."));
    videoSetPlanMetricsLabel->setText(
        tr("Reading source metadata and calculating packet capacity."));
    videoSetAssistantProgress->setRange(0, 0);
    updateVideoSetAssistant();
    startVideoSetProcess(videoSetEncodeArguments("set-plan"));
}

void DriveManagerUI::startVideoSetEncode(const bool resume) {
    try {
        videoSetWorkflow.begin_encoding();
    } catch (const std::exception &error) {
        videoSetWorkflow.fail(
            "The current plan is no longer valid.", error.what());
        updateVideoSetAssistant();
        return;
    }
    videoSetAssistantStack->setCurrentIndex(4);
    videoSetAssistantProgress->setRange(0, 100);
    videoSetAssistantProgress->setValue(0);
    videoSetProgressPhaseLabel->setText(tr("Reading source file"));
    videoSetProgressPartLabel->setText(tr("Preparing Video Set"));
    updateVideoSetAssistant();
    auto arguments = videoSetEncodeArguments("set-encode");
    if (resume) arguments << "--resume";
    startVideoSetProcess(arguments);
}

void DriveManagerUI::startVideoSetScan() {
    const QString input = videoSetAssistantRecoveryInputEdit->text().trimmed();
    if (input.isEmpty() || !QFileInfo::exists(input)) {
        videoSetWorkflow.fail(
            "The selected Video Set location does not exist.",
            "Choose a set folder, manifest, video, or returned-video folder.");
        updateVideoSetAssistant();
        return;
    }
    QString scanTarget = input;
    const QFileInfo selected(input);
    if (selected.isFile() && selected.fileName() == "set_manifest.json") {
        const QDir setRoot(selected.absolutePath());
        const QString returned = setRoot.filePath("returned");
        scanTarget = QDir(returned).entryList(
            {"*.mkv", "*.mp4", "*.webm", "*.avi", "*.mov"},
            QDir::Files).isEmpty()
            ? setRoot.filePath("videos") : returned;
        videoSetCurrentManifest = selected.absoluteFilePath();
        videoSetCurrentSetRoot = selected.absolutePath();
    }
    videoSetWorkflow.begin_scan();
    videoSetAssistantStack->setCurrentIndex(7);
    videoSetDetectedSetsList->clear();
    videoSetScanSummaryLabel->setText(
        tr("Checking embedded Video Set information..."));
    updateVideoSetAssistant();
    startVideoSetProcess({"set-inspect", scanTarget});
}

void DriveManagerUI::startVideoSetRecovery(const bool resume) {
    if (!videoSetWorkflow.view().can_recover) return;
    const QString input = videoSetAssistantRecoveryInputEdit->text().trimmed();
    const QString output = videoSetAssistantRecoveryOutputEdit->text().trimmed();
    if (input.isEmpty() || output.isEmpty()) {
        videoSetWorkflow.fail(
            "A recovery location is missing.",
            "Choose the returned videos and a recovered output folder.");
        updateVideoSetAssistant();
        return;
    }
    videoSetWorkflow.begin_recovery();
    videoSetAssistantStack->setCurrentIndex(8);
    videoSetRecoveryProgressBar->setRange(0, 0);
    videoSetRecoveryProgressLabel->setText(tr("Reading returned video"));
    updateVideoSetAssistant();
    QStringList arguments{"set-recover", input, output};
    if (resume) arguments << "--resume";
    startVideoSetProcess(arguments);
}

void DriveManagerUI::handleVideoSetOutput(const QString &text) {
    videoSetProcessBuffer += text;
    if (!videoSetAssistantOperation) return;

    if (videoSetActiveCommand == "set-plan" ||
        videoSetActiveCommand == "set-encode") {
        videoSetAssistantPlanTable->setRowCount(0);
        const QRegularExpression partExpression(
            R"(P(\d+): offset=(\d+) bytes=(\d+) frames=(\d+) duration=([0-9.]+)s estimated-video=(\d+) bytes)");
        auto matches = partExpression.globalMatch(videoSetProcessBuffer);
        while (matches.hasNext()) {
            const auto match = matches.next();
            const int row = videoSetAssistantPlanTable->rowCount();
            videoSetAssistantPlanTable->insertRow(row);
            const QStringList values{
                match.captured(1), match.captured(3), match.captured(4),
                match.captured(5) + " s", match.captured(6), "Planned"};
            for (int column = 0; column < values.size(); ++column)
                videoSetAssistantPlanTable->setItem(
                    row, column, new QTableWidgetItem(values.at(column)));
        }
    }

    const QRegularExpression verified(
        R"(Part\s+(\d+)/(\d+)\s+locally verified exact)");
    auto verifiedMatches = verified.globalMatch(videoSetProcessBuffer);
    int completed = 0;
    int total = 0;
    while (verifiedMatches.hasNext()) {
        const auto match = verifiedMatches.next();
        completed = (std::max)(completed, match.captured(1).toInt());
        total = match.captured(2).toInt();
    }
    if (total > 0) {
        videoSetAssistantProgress->setRange(0, total);
        videoSetAssistantProgress->setValue(completed);
        videoSetProgressPhaseLabel->setText(tr("Checking video"));
        videoSetProgressPartLabel->setText(
            tr("Completed %1 of %2 locally verified videos")
                .arg(completed).arg(total));
    } else if (videoSetProcessBuffer.contains("Video Set plan")) {
        videoSetProgressPhaseLabel->setText(tr("Preparing parts"));
    }

    const QRegularExpression recoveryPart(
        R"((?:Resume:\s+)?part\s+(\d+)\s+already verified)",
        QRegularExpression::CaseInsensitiveOption);
    const auto recoveryMatch = recoveryPart.match(text);
    if (recoveryMatch.hasMatch())
        videoSetRecoveryProgressLabel->setText(
            tr("Verified part %1; writing recovered data")
                .arg(recoveryMatch.captured(1)));

    const QRegularExpression setLine(
        R"(Set\s+([0-9a-fA-F]{32}):\s+([^\r\n]+))");
    auto setMatches = setLine.globalMatch(text);
    while (setMatches.hasNext()) {
        const auto match = setMatches.next();
        const QString display = match.captured(1).left(8) + " — " +
            match.captured(2).trimmed();
        if (videoSetDetectedSetsList->findItems(
                display, Qt::MatchExactly).isEmpty())
            videoSetDetectedSetsList->addItem(display);
    }
}

void DriveManagerUI::handleVideoSetFinished(
    const int exitCode,
    const QProcess::ExitStatus exitStatus) {
    if (!videoSetAssistantOperation) return;
    videoSetAssistantCancelButton->setEnabled(false);
    videoSetAssistantScanButton->setEnabled(true);
    videoSetDownloadButton->setEnabled(
        video_set_workflow::is_youtube_playlist_url(
            videoSetPlaylistUrlEdit->text().toStdString()));
    const bool success = exitStatus == QProcess::NormalExit && exitCode == 0;
    const auto operationId = videoSetOperationProgress.view().operation_id;
    if (videoSetCancelRequested) {
        (void) videoSetOperationProgress.cancel(
            operationId, QDateTime::currentMSecsSinceEpoch());
        if (videoSetActiveCommand == "set-encode")
            videoSetWorkflow.cancel_encoding();
        else if (videoSetActiveCommand == "set-recover")
            videoSetWorkflow.cancel_recovery();
        videoSetProgressPhaseLabel->setText(
            "Stopped safely. You can continue this Video Set later.");
        updateVideoSetAssistant();
        renderVideoSetActivity();
        return;
    }
    const bool hasFinalScanSummary =
        videoSetOperationProgress.view().has_scan_summary &&
        !videoSetOperationProgress.view().status.empty();
    if ((videoSetActiveCommand == "set-inspect" && hasFinalScanSummary) ||
        success) {
        (void) videoSetOperationProgress.complete(
            operationId, QDateTime::currentMSecsSinceEpoch());
    } else {
        (void) videoSetOperationProgress.fail(
            operationId, QDateTime::currentMSecsSinceEpoch(), exitCode,
            "The Video Set operation did not finish.",
            "Review the suggested action or technical log, then retry.");
    }

    if (videoSetActiveCommand == "set-plan") {
        if (!success) {
            videoSetWorkflow.fail(
                "The Video Set plan could not be calculated.",
                "Check the source, output folder, disk access, and technical details.");
            updateVideoSetAssistant();
            return;
        }
        video_set::SetPlan plan;
        const QRegularExpression id(R"(Set ID:\s*([0-9a-fA-F]{32}))");
        const QRegularExpression source(
            R"(Source:\s*(.+)\s+\((\d+) bytes\))");
        const QRegularExpression profile(
            R"(Profile/config:\s*([^/\r\n]+)\s*/\s*([^\r\n]+))");
        const QRegularExpression parts(R"(Parts:\s*(\d+))");
        const auto idMatch = id.match(videoSetProcessBuffer);
        const auto sourceMatch = source.match(videoSetProcessBuffer);
        const auto profileMatch = profile.match(videoSetProcessBuffer);
        const auto partsMatch = parts.match(videoSetProcessBuffer);
        try {
            if (idMatch.hasMatch())
                plan.set_id = video_set::id_from_hex(
                    idMatch.captured(1).toStdString());
        } catch (...) {
        }
        if (sourceMatch.hasMatch()) {
            plan.original_filename = sourceMatch.captured(1).trimmed().toStdString();
            plan.original_file_size = sourceMatch.captured(2).toULongLong();
        }
        if (profileMatch.hasMatch()) {
            plan.profile_name = profileMatch.captured(1).trimmed().toStdString();
            plan.config_id = profileMatch.captured(2).trimmed().toStdString();
        }
        const int partCount = partsMatch.hasMatch()
            ? partsMatch.captured(1).toInt()
            : videoSetAssistantPlanTable->rowCount();
        plan.parts.resize(static_cast<std::size_t>((std::max)(0, partCount)));
        for (int row = 0; row < videoSetAssistantPlanTable->rowCount(); ++row) {
            auto &part = plan.parts[static_cast<std::size_t>(row)];
            part.part_index = static_cast<uint32_t>(row);
            part.chunk_size = videoSetAssistantPlanTable->item(row, 1)
                ->text().toULongLong();
            part.estimated_frames = videoSetAssistantPlanTable->item(row, 2)
                ->text().toULongLong();
            part.estimated_duration_seconds =
                videoSetAssistantPlanTable->item(row, 3)
                    ->text().section(' ', 0, 0).toDouble();
            part.estimated_output_bytes =
                videoSetAssistantPlanTable->item(row, 4)
                    ->text().toULongLong();
        }
        videoSetWorkflow.apply_plan(plan);
        const auto summary = video_set_workflow::summarize_plan(plan);
        videoSetPlanSummaryLabel->setText(
            tr("Your file will be turned into %1 video(s).")
                .arg(summary.part_count));
        videoSetCreateVideosButton->setText(
            tr("Create %1 Videos").arg(summary.part_count));
        videoSetPlanMetricsLabel->setText(
            tr("About %1 per video\nEstimated total duration: %2\n"
                    "Estimated total output: %3\nTemporary disk estimate: %4\n"
                    "Recovery disk requirement: %5\nSelected mode: %6")
                .arg(QString::number(
                    summary.maximum_part_duration_seconds / 60.0,
                    'f', 1) + tr(" minutes"))
                .arg(QString::number(
                    summary.total_duration_seconds / 60.0,
                    'f', 1) + tr(" minutes"))
                .arg(QLocale().formattedDataSize(
                    static_cast<qint64>(summary.total_estimated_output_bytes)))
                .arg(QLocale().formattedDataSize(
                    static_cast<qint64>(summary.temporary_disk_bytes)))
                .arg(QLocale().formattedDataSize(
                    static_cast<qint64>(summary.recovery_disk_bytes)))
                .arg(videoSetHighCapacityRadio->isChecked()
                    ? tr("Fewer & Shorter Videos (High Capacity)")
                    : tr("Most Reliable (Resilient)")));
        videoSetAssistantProgress->setRange(0, 100);
        videoSetAssistantProgress->setValue(100);
        updateVideoSetAssistant();
        return;
    }

    if (videoSetActiveCommand == "set-encode") {
        if (!success || !videoSetProcessBuffer.contains(
                "locally verified and atomically published")) {
            videoSetWorkflow.cancel_encoding();
            videoSetProgressPhaseLabel->setText(
                tr("The videos were not completely created and verified. Use Resume to keep verified parts and continue later."));
            videoSetProgressResumeButton->setEnabled(true);
            updateVideoSetAssistant();
            return;
        }
        const QRegularExpression published(
            R"(atomically published:\s*([^\r\n]+))");
        const auto match = published.match(videoSetProcessBuffer);
        if (match.hasMatch()) {
            videoSetCurrentSetRoot = match.captured(1).trimmed();
            videoSetCurrentManifest = QDir(videoSetCurrentSetRoot)
                .filePath("set_manifest.json");
            rememberRecentVideoSet(videoSetCurrentManifest);
        }
        const uint32_t count = videoSetWorkflow.view().part_count;
        videoSetWorkflow.apply_local_verification(count);
        videoSetAssistantProgress->setRange(0, static_cast<int>(count));
        videoSetAssistantProgress->setValue(static_cast<int>(count));
        videoSetProgressPhaseLabel->setText(tr("Finalizing Video Set"));
        videoSetProgressPartLabel->setText(
            tr("All %1 videos were created and verified locally.")
                .arg(count));
        videoSetProgressContinueButton->setEnabled(true);
        updateVideoSetAssistant();
        return;
    }

    if (videoSetActiveCommand == "set-inspect") {
        video_set_workflow::ScanSummary summary =
            hasFinalScanSummary
            ? videoSetOperationProgress.view().scan
            : video_set_workflow::ScanSummary{};
        const QRegularExpression available(
            R"(available\s+(\d+)/(\d+)\s+parts,\s+duplicates\s+(\d+))");
        const auto match = available.match(videoSetProcessBuffer);
        if (!hasFinalScanSummary &&
            match.hasMatch()) {
            summary.returned_parts = match.captured(1).toUInt();
            summary.exact_parts = summary.returned_parts;
            summary.expected_parts = match.captured(2).toUInt();
            summary.duplicate_count = match.captured(3).toUInt();
            for (uint32_t index = summary.returned_parts;
                 index < summary.expected_parts; ++index)
                summary.missing_parts.push_back(index);
        }
        const QRegularExpression conflicts(R"(conflicts\s+(\d+))");
        const auto conflictMatch = conflicts.match(videoSetProcessBuffer);
        if (!hasFinalScanSummary &&
            conflictMatch.hasMatch())
            summary.conflict_count = conflictMatch.captured(1).toUInt();
        const QRegularExpression corrupt(R"(Corrupt/unreadable videos:\s*(\d+))");
        const auto corruptMatch = corrupt.match(videoSetProcessBuffer);
        if (!hasFinalScanSummary &&
            corruptMatch.hasMatch()) {
            const uint32_t count = corruptMatch.captured(1).toUInt();
            for (uint32_t index = 0; index < count; ++index)
                summary.corrupt_parts.push_back(index);
        }
        if (!hasFinalScanSummary &&
            !match.hasMatch()) {
            videoSetWorkflow.fail(
                "No valid Video Set parts were found.",
                "Choose downloaded video files or a Video Set folder and scan again.");
        } else if (videoSetDetectedSetsList->count() > 1) {
            videoSetWorkflow.fail(
                "More than one Video Set was found in this folder.",
                "Choose a set-specific folder or manifest so recovery cannot mix sets.");
        } else {
            videoSetWorkflow.apply_scan(std::move(summary));
        }
        updateVideoSetAssistant();
        renderVideoSetActivity();
        if (videoSetInstantRecoveryActive) {
            if (videoSetDetectedSetsList->count() != 1 ||
                !videoSetWorkflow.view().can_recover) {
                videoSetInstantRecoveryStatus->setText(
                    videoSetDetectedSetsList->count() > 1
                    ? tr("Multiple Video Sets were found. Select a set-specific source; automatic recovery stopped safely.")
                    : tr("The playlist does not contain one complete recoverable set. Review missing, corrupt, duplicate, and conflict details."));
                try {
                    auto state = instant_recovery::read_job_state(
                        std::filesystem::path(videoSetInstantRecoveryJobRoot.toStdWString()) /
                        "job_state.json");
                    state.phase = instant_recovery::Phase::NeedsAttention;
                    state.updated_at_epoch_seconds = QDateTime::currentSecsSinceEpoch();
                    instant_recovery::write_job_state_atomic(
                        std::filesystem::path(videoSetInstantRecoveryJobRoot.toStdWString()) /
                        "job_state.json", state);
                } catch (...) {}
                return;
            }
            const QRegularExpression source(
                R"(Set\s+[0-9a-fA-F]{32}:\s+(.+),\s+available)");
            const auto sourceMatch = source.match(videoSetProcessBuffer);
            const QString filename = sourceMatch.hasMatch()
                ? sourceMatch.captured(1).trimmed() : QString();
            if (!filename.isEmpty() && QFileInfo::exists(
                    QDir(videoSetAssistantRecoveryOutputEdit->text())
                        .filePath(filename))) {
                videoSetInstantRecoveryStatus->setText(tr(
                    "The recovered file already exists. Choose a different folder or use the manual recovery controls to confirm overwrite."));
                return;
            }
            videoSetInstantRecoveryStatus->setText(tr(
                "One complete set was found. Recovering and checking the full-file SHA-256..."));
            try {
                auto state = instant_recovery::read_job_state(
                    std::filesystem::path(videoSetInstantRecoveryJobRoot.toStdWString()) /
                    "job_state.json");
                state.phase = instant_recovery::Phase::Recovering;
                state.updated_at_epoch_seconds = QDateTime::currentSecsSinceEpoch();
                instant_recovery::write_job_state_atomic(
                    std::filesystem::path(videoSetInstantRecoveryJobRoot.toStdWString()) /
                    "job_state.json", state);
            } catch (...) {}
            QTimer::singleShot(0, this, [this]() {
                startVideoSetRecovery(true);
            });
        }
        return;
    }

    if (videoSetActiveCommand == "set-recover") {
        const auto &operation = videoSetOperationProgress.view();
        const bool structuredExact = operation.status == "recovered_exact" &&
            !operation.sha256.empty() && !operation.output_path.empty();
        if (success && (structuredExact ||
                        videoSetProcessBuffer.contains("Recovered exact"))) {
            const QRegularExpression final(R"(Final:\s*([^\r\n]+))");
            const QRegularExpression sha(R"(SHA-256:\s*([0-9a-fA-F]{64}))");
            const auto finalMatch = final.match(videoSetProcessBuffer);
            const auto shaMatch = sha.match(videoSetProcessBuffer);
            const QString output = structuredExact
                ? QString::fromStdString(operation.output_path)
                : (finalMatch.hasMatch()
                    ? finalMatch.captured(1).trimmed() : QString());
            videoSetFinalSha = structuredExact
                ? QString::fromStdString(operation.sha256).toUpper()
                : (shaMatch.hasMatch()
                    ? shaMatch.captured(1).toUpper() : QString());
            videoSetWorkflow.apply_recovery_result(
                output.toStdString(), true);
            if (videoSetInstantRecoveryActive) {
                videoSetInstantRecoveryStatus->setText(tr(
                    "Your file was recovered exactly. Full-file SHA-256 matches."));
                try {
                    auto state = instant_recovery::read_job_state(
                        std::filesystem::path(videoSetInstantRecoveryJobRoot.toStdWString()) /
                        "job_state.json");
                    state.phase = instant_recovery::Phase::RecoveredExact;
                    state.final_output_path = output.toStdString();
                    state.final_sha256 = videoSetFinalSha.toStdString();
                    state.final_sha_exact = true;
                    state.updated_at_epoch_seconds = QDateTime::currentSecsSinceEpoch();
                    instant_recovery::write_job_state_atomic(
                        std::filesystem::path(videoSetInstantRecoveryJobRoot.toStdWString()) /
                        "job_state.json", state);
                } catch (...) {}
                videoSetInstantRecoveryActive = false;
            }
            videoSetRecoveryProgressBar->setRange(0, 100);
            videoSetRecoveryProgressBar->setValue(100);
            const QFileInfo recoveredFile(output);
            videoSetSuccessDetailsLabel->setText(
                tr("The full-file SHA-256 matches the original.\n"
                        "File: %1\nSize: %2\nParts: %3\nProfile: %4\nSet: %5\nSHA-256: %6")
                    .arg(recoveredFile.fileName(),
                         QLocale().formattedDataSize(recoveredFile.size()))
                    .arg(videoSetWorkflow.view().part_count)
                    .arg(QString::fromStdString(
                        videoSetWorkflow.view().selected_profile))
                    .arg(QString::fromStdString(
                        videoSetWorkflow.view().set_id).left(8))
                    .arg(videoSetFinalSha));
        } else if (exitCode == 3) {
            video_set_workflow::ScanSummary summary;
            summary.expected_parts = videoSetWorkflow.view().part_count;
            summary.returned_parts = 0;
            summary.missing_parts = {0};
            videoSetWorkflow.apply_scan(std::move(summary));
        } else if (exitCode == 4) {
            video_set_workflow::ScanSummary summary;
            summary.expected_parts = videoSetWorkflow.view().part_count;
            summary.corrupt_parts = {0};
            videoSetWorkflow.apply_scan(std::move(summary));
        } else {
            videoSetWorkflow.apply_recovery_result({}, false);
        }
        updateVideoSetAssistant();
        renderVideoSetActivity();
    }
}

void DriveManagerUI::updateVideoSetAssistant() {
    const auto &view = videoSetWorkflow.view();
    QString primaryMessage = translatedWorkflowText(view.primary_message);
    if (view.state == video_set_workflow::State::Planned)
        primaryMessage = tr("Your file will be divided into %1 videos.")
            .arg(view.part_count);
    else if (view.state == video_set_workflow::State::LocallyVerified)
        primaryMessage = tr("All %1 videos were created and verified locally.")
            .arg(view.part_count);
    else if (view.state ==
             video_set_workflow::State::IncompleteMissingParts)
        primaryMessage = tr("%1 of %2 parts are missing.")
            .arg(view.scan.missing_parts.size())
            .arg(view.scan.expected_parts);
    else if (view.state == video_set_workflow::State::ReadyToRecover)
        primaryMessage = tr("All %1 of %2 parts were found and verified.")
            .arg(view.scan.returned_parts)
            .arg(view.scan.expected_parts);
    videoSetPrimaryMessage->setText(primaryMessage);
    const char *messageState = "info";
    if (view.state == video_set_workflow::State::RecoveredExact ||
        view.state == video_set_workflow::State::ReadyToRecover ||
        view.state == video_set_workflow::State::LocallyVerified)
        messageState = "success";
    else if (view.state == video_set_workflow::State::IncompleteMissingParts)
        messageState = "warning";
    else if (view.state == video_set_workflow::State::ConflictDetected ||
             view.state == video_set_workflow::State::CorruptPartsDetected ||
             view.state == video_set_workflow::State::Failed)
        messageState = "error";
    videoSetPrimaryMessage->setProperty("vsxState", messageState);
    videoSetPrimaryMessage->style()->unpolish(videoSetPrimaryMessage);
    videoSetPrimaryMessage->style()->polish(videoSetPrimaryMessage);
    videoSetSuggestedAction->setText(
        translatedWorkflowText(view.suggested_action));

    QStringList steps;
    int active = 0;
    if (view.path == video_set_workflow::Path::Recover) {
        steps = {tr("Choose"), tr("Check"), tr("Recover"),
                 tr("Verify"), tr("Done")};
        if (view.state == video_set_workflow::State::Recovering) active = 2;
        else if (view.state == video_set_workflow::State::RecoveredExact) active = 4;
        else active = view.state == video_set_workflow::State::Welcome ? 0 : 1;
    } else {
        steps = {tr("File"), tr("Mode"), tr("Create Videos"),
                 tr("YouTube"), tr("Done")};
        switch (view.state) {
            case video_set_workflow::State::SourceRequired:
                active = 0; break;
            case video_set_workflow::State::ReadyToPlan:
                active = 1; break;
            case video_set_workflow::State::Planning:
            case video_set_workflow::State::Planned:
            case video_set_workflow::State::Encoding:
            case video_set_workflow::State::EncodingPaused:
            case video_set_workflow::State::LocallyVerified:
                active = 2; break;
            case video_set_workflow::State::RecoveredExact:
                active = 4; break;
            default:
                active = 3; break;
        }
    }
    const bool failed = view.state == video_set_workflow::State::Failed ||
        view.state == video_set_workflow::State::IncompleteMissingParts ||
        view.state == video_set_workflow::State::ConflictDetected ||
        view.state == video_set_workflow::State::CorruptPartsDetected;
    videoSetStepIndicator->setSteps(steps, active, failed ? active : -1);
    videoSetStepIndicator->setVisible(view.state !=
        video_set_workflow::State::Welcome);
    videoSetPrimaryMessage->setVisible(view.state !=
        video_set_workflow::State::Welcome && !primaryMessage.isEmpty());
    videoSetSuggestedAction->setVisible(view.state !=
        video_set_workflow::State::Welcome &&
        !videoSetSuggestedAction->text().isEmpty());

    switch (view.state) {
        case video_set_workflow::State::Welcome:
            videoSetAssistantStack->setCurrentIndex(0); break;
        case video_set_workflow::State::SourceRequired:
            if (view.path == video_set_workflow::Path::Create)
                videoSetAssistantStack->setCurrentIndex(1);
            break;
        case video_set_workflow::State::Planning:
        case video_set_workflow::State::Planned:
            videoSetAssistantStack->setCurrentIndex(3); break;
        case video_set_workflow::State::Encoding:
        case video_set_workflow::State::EncodingPaused:
        case video_set_workflow::State::LocallyVerified:
            videoSetAssistantStack->setCurrentIndex(4); break;
        case video_set_workflow::State::AwaitingUpload:
            videoSetAssistantStack->setCurrentIndex(5); break;
        case video_set_workflow::State::AwaitingReturnedVideos:
        case video_set_workflow::State::DownloadingReturnedVideos:
            if (view.path == video_set_workflow::Path::Create)
                videoSetAssistantStack->setCurrentIndex(6);
            else
                videoSetAssistantStack->setCurrentIndex(7);
            break;
        case video_set_workflow::State::ScanningReturnedVideos:
        case video_set_workflow::State::IncompleteMissingParts:
        case video_set_workflow::State::ConflictDetected:
        case video_set_workflow::State::CorruptPartsDetected:
        case video_set_workflow::State::ReadyToRecover:
            videoSetAssistantStack->setCurrentIndex(7); break;
        case video_set_workflow::State::Recovering:
            videoSetAssistantStack->setCurrentIndex(8); break;
        case video_set_workflow::State::RecoveredExact:
            videoSetAssistantStack->setCurrentIndex(9); break;
        case video_set_workflow::State::ReadyToPlan:
        case video_set_workflow::State::Failed:
            break;
    }
    videoSetCreateVideosButton->setEnabled(
        view.state == video_set_workflow::State::Planned);
    videoSetProgressContinueButton->setVisible(
        view.state == video_set_workflow::State::LocallyVerified);
    videoSetProgressOpenFolderButton->setVisible(
        view.state == video_set_workflow::State::LocallyVerified);
    videoSetProgressResumeButton->setVisible(
        view.state == video_set_workflow::State::EncodingPaused);
    videoSetAssistantRecoverButton->setEnabled(
        view.can_recover && !videoSetOperationProgress.view().is_busy);
    videoSetScanSummaryLabel->setText(
        primaryMessage);
    if (view.state == video_set_workflow::State::ScanningReturnedVideos) {
        videoSetScanCountsLabel->setText(tr("Discovering and checking videos..."));
    } else if (view.scan.expected_parts != 0 ||
               view.scan.returned_parts != 0 ||
               !view.scan.corrupt_parts.empty()) {
        videoSetScanCountsLabel->setText(
            tr("Verified: %1/%2   Missing: %3   Corrupt: %4   Duplicates: %5   Conflicts: %6")
                .arg(view.scan.exact_parts)
                .arg(view.scan.expected_parts)
                .arg(view.scan.missing_parts.size())
                .arg(view.scan.corrupt_parts.size())
                .arg(view.scan.duplicate_count)
                .arg(view.scan.conflict_count));
    } else {
        videoSetScanCountsLabel->setText(tr("Scan has not found a Video Set summary yet."));
    }
    if (videoSetScanMetricValues.size() == 5) {
        videoSetScanMetricValues[0]->setText(QString::number(view.scan.exact_parts));
        videoSetScanMetricValues[1]->setText(QString::number(
            view.scan.missing_parts.size()));
        videoSetScanMetricValues[2]->setText(QString::number(
            view.scan.corrupt_parts.size()));
        videoSetScanMetricValues[3]->setText(QString::number(
            view.scan.duplicate_count));
        videoSetScanMetricValues[4]->setText(QString::number(
            view.scan.conflict_count));
        for (int index = 0; index < videoSetScanMetricValues.size(); ++index) {
            auto *metric = qobject_cast<QFrame *>(
                videoSetScanMetricValues[index]->parentWidget());
            if (!metric) continue;
            const int value = videoSetScanMetricValues[index]->text().toInt();
            const char *state = index == 0 ? "success" :
                (value > 0 ? (index == 1 ? "warning" : "error") : "muted");
            metric->setProperty("vsxState", state);
            metric->style()->unpolish(metric);
            metric->style()->polish(metric);
        }
    }
    if (view.can_recover)
        videoSetScanSummaryLabel->setText(tr("Everything is ready."));
    videoSetRecoveryAvailabilityLabel->setText(view.can_recover
        ? tr("Scan complete. Select Recover Original File to rebuild the file.")
        : tr("Recovery will become available after scanning finishes and every required part is verified."));
    renderVideoSetActivity();
}

QString DriveManagerUI::findYtDlpExecutable() const {
    const QString fromPath = QStandardPaths::findExecutable("yt-dlp");
#ifdef Q_OS_WIN
    const QString executableName = "yt-dlp.exe";
#else
    const QString executableName = "yt-dlp";
#endif
    const QString applicationDirectory =
        QCoreApplication::applicationDirPath();
    const QStringList candidates{
        QDir(applicationDirectory).filePath(executableName),
        QDir(applicationDirectory).filePath("tools/" + executableName),
        QDir(applicationDirectory).filePath("../tools/" + executableName)};
    const QString selected = QSettings().value(
        "videoSet/ytdlpPath").toString();
    std::vector<std::string> toolCandidates;
    for (const auto &candidate : candidates)
        toolCandidates.push_back(candidate.toStdString());
    const auto found = video_set_workflow::select_ytdlp_executable(
        fromPath.toStdString(), std::move(toolCandidates),
        selected.toStdString(), [](const std::string_view value) {
            return QFileInfo(QString::fromStdString(std::string(value)))
                .isExecutable();
        });
    return QString::fromStdString(found);
}

void DriveManagerUI::startYouTubeSync() {
    if (videoSetCurrentManifest.isEmpty() ||
        !QFileInfo::exists(videoSetCurrentManifest)) {
        videoSetYouTubeSyncStatus->setText(tr(
            "A locally verified Video Set is required before sync."));
        return;
    }
    const QSettings settings;
    if (!settings.value("youtube/connected", false).toBool()) {
        videoSetYouTubeSyncStatus->setText(tr(
            "YouTube is not connected. Connect it in Settings or upload manually."));
        return;
    }
    try {
        auto store = youtube_sync::make_platform_credential_store();
        const auto protectedToken = store->load("youtube-oauth");
        if (!protectedToken)
            throw std::runtime_error("secure OAuth credentials are unavailable");
        const auto token = youtube_sync::parse_token_record(*protectedToken);
        if (token.expires_at_epoch_seconds > 0 &&
            token.expires_at_epoch_seconds <=
                QDateTime::currentSecsSinceEpoch() + 30) {
            if (token.refresh_token.empty())
                throw std::runtime_error("OAuth refresh token is unavailable");
            const auto config = youtube_sync::read_oauth_client_config(
                std::filesystem::path(QSettings().value(
                    "youtube/oauthClientConfigPath").toString().toStdWString()));
            if (!config.configured())
                throw std::runtime_error("OAuth client configuration is unavailable");
            youtubePendingSyncAfterRefresh = true;
            videoSetYouTubeSyncStatus->setText(tr(
                "Refreshing YouTube authorization..."));
            youtubeNetworkService->refreshAccessToken(
                config, QString::fromStdString(token.refresh_token));
            return;
        }
        if (token.access_token.empty())
            throw std::runtime_error("OAuth access token needs refresh");
        youtubeSyncAccessToken = QString::fromStdString(token.access_token);
        const auto plan = video_set::read_manifest(
            std::filesystem::path(videoSetCurrentManifest.toStdWString()));
        const std::string setId = video_set::id_hex(plan.set_id);
        youtubeSyncStatePath = QDir(videoSetCurrentSetRoot).filePath(
            "youtube_sync_state.json");
        if (QFileInfo::exists(youtubeSyncStatePath)) {
            youtubeRuntimeSyncState = youtube_sync::read_sync_state(
                std::filesystem::path(youtubeSyncStatePath.toStdWString()),
                setId);
        } else {
            youtubeRuntimeSyncState = {};
            youtubeRuntimeSyncState.set_id = setId;
            youtubeRuntimeSyncState.requested_privacy =
                youtube_sync::parse_privacy(settings.value(
                    "youtube/defaultPrivacy", "unlisted").toString().toStdString());
            youtubeRuntimeSyncState.actual_privacy =
                youtubeRuntimeSyncState.requested_privacy;
            for (const auto &part : plan.parts) {
                youtube_sync::PartState remote;
                remote.part_index = part.part_index;
                remote.part_id = video_set::id_hex(part.part_id);
                remote.video_path = (std::filesystem::path(
                    videoSetCurrentSetRoot.toStdWString()) / "videos" /
                    std::filesystem::u8path(part.expected_video_filename)).string();
                remote.requested_privacy =
                    youtubeRuntimeSyncState.requested_privacy;
                remote.actual_privacy = remote.requested_privacy;
                youtubeRuntimeSyncState.parts.push_back(std::move(remote));
            }
            youtube_sync::write_sync_state_atomic(
                std::filesystem::path(youtubeSyncStatePath.toStdWString()),
                youtubeRuntimeSyncState);
        }
        videoSetYouTubeSyncButton->setEnabled(false);
        videoSetYouTubeSyncPauseButton->setEnabled(true);
        videoSetYouTubeSyncProgress->setRange(0, 100);
        if (!youtubeRuntimeSyncState.playlist_created) {
            youtubeSyncOperation = "playlist_create";
            videoSetYouTubeSyncStatus->setText(tr("Creating playlist..."));
            youtubeNetworkService->execute(
                youtube_sync::create_playlist_request(
                    youtube_endpoint_set(), youtubeSyncAccessToken.toStdString(),
                    "VidStoreX - Set " + setId.substr(0, 8),
                    youtubeRuntimeSyncState.requested_privacy));
        } else {
            continueYouTubeUpload();
        }
    } catch (const std::exception &error) {
        videoSetYouTubeSyncStatus->setText(tr(
            "YouTube Sync needs attention: %1. Manual upload remains available.")
            .arg(QString::fromUtf8(error.what())));
        videoSetYouTubeSyncButton->setEnabled(true);
        videoSetYouTubeSyncPauseButton->setEnabled(false);
    }
}

void DriveManagerUI::continueYouTubeUpload() {
    const auto persist = [this]() {
        youtubeRuntimeSyncState.last_updated =
            QDateTime::currentDateTimeUtc().toString(Qt::ISODate).toStdString();
        youtube_sync::write_sync_state_atomic(
            std::filesystem::path(youtubeSyncStatePath.toStdWString()),
            youtubeRuntimeSyncState);
    };
    auto next = std::find_if(youtubeRuntimeSyncState.parts.begin(),
        youtubeRuntimeSyncState.parts.end(), [](const youtube_sync::PartState &part) {
            return part.upload_state != youtube_sync::UploadState::Uploaded;
        });
    if (next == youtubeRuntimeSyncState.parts.end()) {
        std::vector<std::string> ids;
        for (const auto &part : youtubeRuntimeSyncState.parts)
            ids.push_back(part.youtube_video_id);
        youtubeSyncOperation = "processing";
        if (youtubeProcessingStartedMs == 0)
            youtubeProcessingStartedMs = QDateTime::currentMSecsSinceEpoch();
        videoSetYouTubeSyncStatus->setText(tr(
            "All parts were uploaded. Checking YouTube processing..."));
        youtubeNetworkService->execute(
            youtube_sync::processing_status_request(
                youtube_endpoint_set(), youtubeSyncAccessToken.toStdString(), ids));
        return;
    }
    youtubeSyncPartIndex = static_cast<uint32_t>(
        std::distance(youtubeRuntimeSyncState.parts.begin(), next));
    auto &part = *next;
    if (part.upload_state == youtube_sync::UploadState::Paused)
        part.upload_state = part.upload_session_uri.empty()
            ? youtube_sync::UploadState::Pending
            : youtube_sync::UploadState::SessionCreated;
    const QFileInfo file(QString::fromStdString(part.video_path));
    if (!file.exists() || !file.isFile()) {
        videoSetYouTubeSyncStatus->setText(tr(
            "Video Set part %1 is missing locally. Manual upload remains available.")
            .arg(part.part_index + 1));
        youtubeSyncOperation.clear();
        return;
    }
    if (part.upload_session_uri.empty()) {
        youtubeSyncOperation = "upload_session";
        videoSetYouTubeSyncStatus->setText(tr("Uploading Part %1 of %2")
            .arg(part.part_index + 1).arg(youtubeRuntimeSyncState.parts.size()));
        youtube_sync::VideoMetadata metadata;
        metadata.set_id = youtubeRuntimeSyncState.set_id;
        metadata.part_index = part.part_index;
        metadata.part_count = static_cast<uint32_t>(
            youtubeRuntimeSyncState.parts.size());
        metadata.privacy = part.requested_privacy;
        metadata.privacy_friendly_titles = QSettings().value(
            "youtube/privacyFriendlyTitles", true).toBool();
        part.upload_state = youtube_sync::UploadState::SessionCreated;
        persist();
        youtubeNetworkService->execute(
            youtube_sync::resumable_session_request(
                youtube_endpoint_set(), youtubeSyncAccessToken.toStdString(), metadata,
                static_cast<uint64_t>(file.size())));
        return;
    }
    QFile input(file.absoluteFilePath());
    if (!input.open(QIODevice::ReadOnly) ||
        !input.seek(static_cast<qint64>(part.uploaded_bytes))) {
        videoSetYouTubeSyncStatus->setText(tr(
            "The current upload part could not be read."));
        youtubeSyncOperation.clear();
        return;
    }
    const auto chunk = youtube_sync::next_upload_chunk(
        part.uploaded_bytes, static_cast<uint64_t>(file.size()));
    const QByteArray bytes = input.read(static_cast<qint64>(chunk.size()));
    youtube_sync::HttpRequest request;
    request.method = "PUT";
    request.url = part.upload_session_uri;
    request.headers = {
        {"Authorization", "Bearer " + youtubeSyncAccessToken.toStdString()},
        {"Content-Type", "video/*"},
        {"Content-Length", std::to_string(bytes.size())},
        {"Content-Range", "bytes " + std::to_string(chunk.first) + "-" +
            std::to_string(chunk.last) + "/" + std::to_string(chunk.total)}};
    request.body.assign(bytes.constData(), static_cast<std::size_t>(bytes.size()));
    part.upload_state = youtube_sync::UploadState::Uploading;
    persist();
    youtubeSyncOperation = "upload_chunk";
    const uint64_t totalBytes = std::accumulate(
        youtubeRuntimeSyncState.parts.begin(), youtubeRuntimeSyncState.parts.end(),
        uint64_t{0}, [](const uint64_t sum, const youtube_sync::PartState &p) {
            return sum + static_cast<uint64_t>(QFileInfo(
                QString::fromStdString(p.video_path)).size());
        });
    const uint64_t doneBytes = std::accumulate(
        youtubeRuntimeSyncState.parts.begin(), youtubeRuntimeSyncState.parts.end(),
        uint64_t{0}, [](const uint64_t sum, const youtube_sync::PartState &p) {
            return sum + p.uploaded_bytes;
        });
    videoSetYouTubeSyncProgress->setValue(totalBytes == 0 ? 0 :
        static_cast<int>(doneBytes * 100 / totalBytes));
    youtubeNetworkService->execute(request);
}

void DriveManagerUI::handleYouTubeSyncResponse(
    const int status, const QByteArray &body,
    const QList<QPair<QByteArray, QByteArray>> &headers) {
    const auto header = [&headers](const QByteArray &name) {
        for (const auto &item : headers)
            if (item.first.compare(name, Qt::CaseInsensitive) == 0)
                return item.second;
        return QByteArray{};
    };
    const auto persist = [this]() {
        youtubeRuntimeSyncState.last_updated =
            QDateTime::currentDateTimeUtc().toString(Qt::ISODate).toStdString();
        youtube_sync::write_sync_state_atomic(
            std::filesystem::path(youtubeSyncStatePath.toStdWString()),
            youtubeRuntimeSyncState);
    };
    try {
        if (youtubeSyncOperation == "playlist_create") {
            if (status < 200 || status >= 300) throw std::runtime_error(
                youtube_sync::classify_api_error(status, body.toStdString(),
                    "playlist_create").user_message);
            const auto object = QJsonDocument::fromJson(body).object();
            youtubeRuntimeSyncState.playlist_id = object.value("id").toString().toStdString();
            if (youtubeRuntimeSyncState.playlist_id.empty())
                throw std::runtime_error("playlist response did not contain an ID");
            youtubeRuntimeSyncState.playlist_url =
                "https://www.youtube.com/playlist?list=" +
                youtubeRuntimeSyncState.playlist_id;
            youtubeRuntimeSyncState.playlist_created = true;
            const auto privacy = object.value("status").toObject()
                .value("privacyStatus").toString();
            if (!privacy.isEmpty())
                youtubeRuntimeSyncState.actual_privacy =
                    youtube_sync::parse_privacy(privacy.toStdString());
            persist();
            youtubeSyncOperation.clear();
            continueYouTubeUpload();
            return;
        }
        auto &part = youtubeRuntimeSyncState.parts.at(youtubeSyncPartIndex);
        if (youtubeSyncOperation == "upload_session") {
            if (status < 200 || status >= 300 || header("Location").isEmpty())
                throw std::runtime_error("YouTube did not create a resumable upload session");
            part.upload_session_uri = header("Location").toStdString();
            part.upload_state = youtube_sync::UploadState::SessionCreated;
            persist();
            youtubeSyncOperation.clear();
            continueYouTubeUpload();
            return;
        }
        if (youtubeSyncOperation == "upload_chunk" ||
            youtubeSyncOperation == "upload_status") {
            const auto decision = youtube_sync::decide_upload_response(
                status, header("Range").toStdString(), body.toStdString());
            if (decision.action == youtube_sync::UploadResponseDecision::Action::NextChunk) {
                youtubeSyncRetryAttempt = 0;
                part.uploaded_bytes = decision.next_offset;
                persist();
                youtubeSyncOperation.clear();
                continueYouTubeUpload();
                return;
            }
            if (decision.action == youtube_sync::UploadResponseDecision::Action::QueryStatus) {
                youtubeSyncOperation = "upload_status";
                uint32_t delay = youtube_sync::exponential_backoff_seconds(
                    youtubeSyncRetryAttempt++);
                bool ok = false;
                const uint32_t retryAfter = header("Retry-After").toUInt(&ok);
                if (ok) delay = retryAfter;
                const auto request = youtube_sync::upload_status_request(
                    part.upload_session_uri, youtubeSyncAccessToken.toStdString(),
                    static_cast<uint64_t>(QFileInfo(
                        QString::fromStdString(part.video_path)).size()));
                QTimer::singleShot(static_cast<int>(delay * 1000), this,
                    [this, request]() {
                    if (youtubeSyncOperation == "upload_status")
                        youtubeNetworkService->execute(request);
                });
                return;
            }
            if (decision.action == youtube_sync::UploadResponseDecision::Action::SessionExpired) {
                part.upload_state = youtube_sync::UploadState::SessionExpired;
                persist();
                throw std::runtime_error(
                    "The upload session expired. Review before retrying to avoid a duplicate.");
            }
            if (decision.action != youtube_sync::UploadResponseDecision::Action::Completed ||
                decision.video_id.empty())
                throw std::runtime_error("YouTube upload failed permanently");
            part.youtube_video_id = decision.video_id;
            youtubeSyncRetryAttempt = 0;
            part.upload_state = youtube_sync::UploadState::Uploaded;
            part.uploaded_bytes = static_cast<uint64_t>(QFileInfo(
                QString::fromStdString(part.video_path)).size());
            const auto object = QJsonDocument::fromJson(body).object();
            const auto actual = object.value("status").toObject()
                .value("privacyStatus").toString();
            if (!actual.isEmpty()) part.actual_privacy =
                youtube_sync::parse_privacy(actual.toStdString());
            persist();
            youtubeSyncOperation = "playlist_insert";
            youtubeNetworkService->execute(
                youtube_sync::add_playlist_item_request(
                    youtube_endpoint_set(), youtubeSyncAccessToken.toStdString(),
                    youtubeRuntimeSyncState.playlist_id,
                    part.youtube_video_id, part.part_index));
            return;
        }
        if (youtubeSyncOperation == "playlist_insert") {
            if (status < 200 || status >= 300)
                throw std::runtime_error(
                    youtube_sync::classify_api_error(status, body.toStdString(),
                        "playlist_insert").user_message);
            part.playlist_item_id = QJsonDocument::fromJson(body).object()
                .value("id").toString().toStdString();
            persist();
            youtubeSyncOperation.clear();
            continueYouTubeUpload();
            return;
        }
        if (youtubeSyncOperation == "processing") {
            if (status < 200 || status >= 300)
                throw std::runtime_error("YouTube processing status could not be read");
            const auto items = QJsonDocument::fromJson(body).object()
                .value("items").toArray();
            for (const auto &value : items) {
                const auto object = value.toObject();
                const std::string id = object.value("id").toString().toStdString();
                auto found = std::find_if(youtubeRuntimeSyncState.parts.begin(),
                    youtubeRuntimeSyncState.parts.end(), [&](const youtube_sync::PartState &p) {
                        return p.youtube_video_id == id;
                    });
                if (found == youtubeRuntimeSyncState.parts.end()) continue;
                const auto processing = object.value("processingDetails").toObject();
                const QString processingStatus = processing.value(
                    "processingStatus").toString();
                found->processing_state = processingStatus == "succeeded"
                    ? youtube_sync::ProcessingState::Succeeded
                    : processingStatus == "failed"
                    ? youtube_sync::ProcessingState::Failed
                    : youtube_sync::ProcessingState::Processing;
                const auto progress = processing.value("processingProgress").toObject();
                found->processing_parts_done = static_cast<uint64_t>(
                    progress.value("partsProcessed").toDouble());
                found->processing_parts_total = static_cast<uint64_t>(
                    progress.value("partsTotal").toDouble());
                const auto privacy = object.value("status").toObject()
                    .value("privacyStatus").toString();
                if (!privacy.isEmpty()) found->actual_privacy =
                    youtube_sync::parse_privacy(privacy.toStdString());
            }
            youtubeRuntimeSyncState.actual_privacy = std::any_of(
                youtubeRuntimeSyncState.parts.begin(), youtubeRuntimeSyncState.parts.end(),
                [](const youtube_sync::PartState &p) {
                    return p.actual_privacy == youtube_sync::Privacy::Private;
                }) ? youtube_sync::Privacy::Private
                   : youtubeRuntimeSyncState.requested_privacy;
            persist();
            youtubeSyncOperation.clear();
            const auto ready = std::count_if(youtubeRuntimeSyncState.parts.begin(),
                youtubeRuntimeSyncState.parts.end(), [](const youtube_sync::PartState &p) {
                    return p.processing_state == youtube_sync::ProcessingState::Succeeded;
                });
            videoSetYouTubeSyncProgress->setValue(
                youtubeRuntimeSyncState.parts.empty() ? 0 :
                static_cast<int>(ready * 100 / youtubeRuntimeSyncState.parts.size()));
            if (youtubeRuntimeSyncState.actual_privacy == youtube_sync::Privacy::Private) {
                youtubeProcessingStartedMs = 0;
                youtubeProcessingPollAttempt = 0;
                videoSetYouTubeSyncStatus->setText(tr(
                    "Videos were uploaded successfully, but they are Private. Automatic download of YouTube's processed copies is unavailable in this configuration."));
            } else if (ready == static_cast<int>(youtubeRuntimeSyncState.parts.size())) {
                videoSetYouTubeSyncStatus->setText(tr(
                    "All uploaded videos were processed. The playlist is ready for returned-copy verification."));
                videoSetPlaylistUrlEdit->setText(QString::fromStdString(
                    youtubeRuntimeSyncState.playlist_url));
                youtubeProcessingStartedMs = 0;
                youtubeProcessingPollAttempt = 0;
                if (QSettings().value("youtube/autoDownload", true).toBool())
                    QTimer::singleShot(0, this,
                        &DriveManagerUI::startYouTubeReadinessProbe);
            } else {
                if (youtube_sync::processing_poll_timed_out(
                        youtubeProcessingStartedMs,
                        QDateTime::currentMSecsSinceEpoch())) {
                    videoSetYouTubeSyncStatus->setText(tr(
                        "Processing is taking longer than expected. %1 of %2 videos are ready. Resume later to check again.")
                        .arg(ready).arg(youtubeRuntimeSyncState.parts.size()));
                    videoSetYouTubeSyncButton->setText(tr("Resume Upload"));
                    videoSetYouTubeSyncButton->setEnabled(true);
                    videoSetYouTubeSyncPauseButton->setEnabled(false);
                    return;
                }
                const uint32_t delay =
                    youtube_sync::processing_poll_delay_seconds(
                        youtubeProcessingPollAttempt++);
                videoSetYouTubeSyncStatus->setText(tr(
                    "%1 of %2 videos are processed. Checking again in %3 seconds.")
                    .arg(ready).arg(youtubeRuntimeSyncState.parts.size())
                    .arg(delay));
                std::vector<std::string> ids;
                for (const auto &item : youtubeRuntimeSyncState.parts)
                    ids.push_back(item.youtube_video_id);
                const auto request = youtube_sync::processing_status_request(
                    youtube_endpoint_set(), youtubeSyncAccessToken.toStdString(), ids);
                youtubeSyncOperation = "processing";
                QTimer::singleShot(static_cast<int>(delay * 1000), this,
                    [this, request]() {
                    if (youtubeSyncOperation == "processing")
                        youtubeNetworkService->execute(request);
                });
                return;
            }
            videoSetYouTubeSyncButton->setText(tr("Resume Upload"));
            videoSetYouTubeSyncButton->setEnabled(true);
            videoSetYouTubeSyncPauseButton->setEnabled(false);
        }
    } catch (const std::exception &error) {
        youtubeSyncOperation.clear();
        videoSetYouTubeSyncStatus->setText(tr(
            "YouTube Sync needs attention: %1 Manual upload remains available.")
            .arg(QString::fromUtf8(error.what())));
        videoSetYouTubeSyncButton->setText(tr("Resume Upload"));
        videoSetYouTubeSyncButton->setEnabled(true);
        videoSetYouTubeSyncPauseButton->setEnabled(false);
    }
}

void DriveManagerUI::startYouTubeReadinessProbe() {
    if (youtubeRuntimeSyncState.actual_privacy ==
            youtube_sync::Privacy::Private)
        return;
    const QString executable = findYtDlpExecutable();
    if (executable.isEmpty()) {
        videoSetYouTubeSyncStatus->setText(tr(
            "Videos are processed, but yt-dlp is unavailable. Select it or use the manual download controls."));
        videoSetYouTubeSyncButton->setEnabled(true);
        videoSetYouTubeSyncPauseButton->setEnabled(false);
        return;
    }
    if (youtubeProcessingStartedMs == 0)
        youtubeProcessingStartedMs = QDateTime::currentMSecsSinceEpoch();
    if (youtube_sync::processing_poll_timed_out(
            youtubeProcessingStartedMs, QDateTime::currentMSecsSinceEpoch())) {
        youtubeSyncOperation.clear();
        videoSetYouTubeSyncStatus->setText(tr(
            "Processing is taking longer than expected. The 1080p copies are not available yet; Resume later or download manually."));
        videoSetYouTubeSyncButton->setEnabled(true);
        videoSetYouTubeSyncPauseButton->setEnabled(false);
        return;
    }
    if (youtubeReadinessProcess &&
        youtubeReadinessProcess->state() != QProcess::NotRunning)
        return;
    youtubeSyncOperation = "readiness";
    videoSetYouTubeSyncStatus->setText(tr(
        "Waiting for processed 1080p copy..."));
    youtubeReadinessProcess = new QProcess(this);
    youtubeReadinessProcess->setProcessChannelMode(QProcess::MergedChannels);
    connect(youtubeReadinessProcess,
        QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
        [this](const int code, const QProcess::ExitStatus exitStatus) {
        auto *probe = youtubeReadinessProcess;
        youtubeReadinessProcess = nullptr;
        if (probe) probe->deleteLater();
        if (youtubeSyncOperation != "readiness") return;
        if (exitStatus == QProcess::NormalExit && code == 0) {
            for (auto &part : youtubeRuntimeSyncState.parts)
                part.returned_download_state = "ready";
            try {
                youtube_sync::write_sync_state_atomic(
                    std::filesystem::path(youtubeSyncStatePath.toStdWString()),
                    youtubeRuntimeSyncState);
            } catch (...) {}
            youtubeSyncOperation.clear();
            startVideoSetDownload();
            return;
        }
        const uint32_t delay = youtube_sync::processing_poll_delay_seconds(
            youtubeProcessingPollAttempt++);
        videoSetYouTubeSyncStatus->setText(tr(
            "Waiting for processed 1080p copy. Checking again in %1 seconds.")
            .arg(delay));
        QTimer::singleShot(static_cast<int>(delay * 1000), this, [this]() {
            if (youtubeSyncOperation == "readiness")
                startYouTubeReadinessProbe();
        });
    });
    connect(youtubeReadinessProcess, &QProcess::errorOccurred, this,
        [this](const QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart) return;
        auto *probe = youtubeReadinessProcess;
        youtubeReadinessProcess = nullptr;
        if (probe) probe->deleteLater();
        youtubeSyncOperation.clear();
        videoSetYouTubeSyncStatus->setText(tr(
            "yt-dlp could not check the processed 1080p copies. Manual download remains available."));
        videoSetYouTubeSyncButton->setEnabled(true);
        videoSetYouTubeSyncPauseButton->setEnabled(false);
    });
    youtubeReadinessProcess->start(executable, {
        "--simulate", "--quiet", "--no-warnings", "--format",
        QString::fromStdString(std::string(
            video_set_workflow::kYtDlpFormatSelector)),
        videoSetPlaylistUrlEdit->text()});
}

void DriveManagerUI::startVideoSetDownload() {
    if (videoSetCurrentSetRoot.isEmpty()) {
        videoSetDownloadStatusLabel->setText(
            "Open or create a Video Set before downloading its playlist.");
        return;
    }
    const QString executable = findYtDlpExecutable();
    if (executable.isEmpty()) {
        videoSetDownloadStatusLabel->setText(
            "yt-dlp was not found. Select the executable or download videos manually. VidStoreX does not install it automatically.");
        videoSetSelectYtDlpButton->setFocus();
        return;
    }
    const QString returned = QDir(videoSetCurrentSetRoot).filePath("returned");
    if (!QDir().mkpath(returned)) {
        videoSetDownloadStatusLabel->setText(
            "The returned-videos folder could not be created. Choose a writable set location.");
        return;
    }
    std::vector<std::string> rawArguments;
    try {
        rawArguments = video_set_workflow::ytdlp_arguments(
            videoSetPlaylistUrlEdit->text().toStdString(),
#ifdef Q_OS_WIN
            std::filesystem::path(returned.toStdWString())
#else
            std::filesystem::path(returned.toStdString())
#endif
        );
    } catch (const std::exception &error) {
        videoSetDownloadStatusLabel->setText(error.what());
        return;
    }
    QStringList arguments;
    for (const auto &argument : rawArguments)
        arguments << QString::fromStdString(argument);
    videoSetWorkflow.begin_download();
    videoSetCancelRequested = false;
    videoSetLastAssistantArguments = {"yt-dlp"};
    (void) videoSetOperationProgress.begin(
        video_set_workflow::OperationType::Download,
        video_set_workflow::OperationPhase::Preparing,
        QDateTime::currentMSecsSinceEpoch(),
        "Downloading processed videos",
        "VidStoreX is downloading YouTube's processed copies into this set's returned folder.");
    videoSetDownloadProgress->setRange(0, 0);
    videoSetDownloadButton->setEnabled(false);
    videoSetAssistantCancelButton->setEnabled(true);
    videoSetDownloadStatusLabel->setText(
        "Starting yt-dlp directly; PowerShell ExecutionPolicy is not involved.");
    QSettings().setValue(
        "videoSet/lastPlaylistUrl", videoSetPlaylistUrlEdit->text());
    updateVideoSetAssistant();
    videoSetDownloadProcess->start(executable, arguments);
}

void DriveManagerUI::refreshRecentVideoSets() {
    videoSetRecentList->clear();
    QSettings settings;
    if (!settings.value("ui/rememberRecentSets", true).toBool()) {
        videoSetRecentContinueButton->setEnabled(false);
        videoSetRecentOpenFolderButton->setEnabled(false);
        videoSetRecentRemoveButton->setEnabled(false);
        videoSetRecentContinueButton->setVisible(false);
        videoSetRecentOpenFolderButton->setVisible(false);
        videoSetRecentRemoveButton->setVisible(false);
        videoSetRecentList->setVisible(false);
        videoSetRecentList->setFixedHeight(0);
        videoSetRecentEmptyState->setVisible(true);
        videoSetRecentEmptyLabel->setText(tr(
            "Recent Video Sets are disabled in Settings."));
        return;
    }
    const auto paths = settings.value(
        "videoSet/recentManifests").toStringList();
    const auto friendlyStatus = [this](const std::string &status) {
        if (status == "Locally verified") return tr("Ready for YouTube");
        if (status == "Returned verified") return tr("Ready to recover");
        if (status == "Recovered exact") return tr("Recovered exactly");
        if (status == "Encoding") return tr("Creating videos");
        if (status == "Planned") return tr("Ready to create");
        if (status == "Incomplete") return tr("Needs attention");
        return translatedWorkflowText(status);
    };
    for (const auto &path : paths.mid(0, 5)) {
        const QFileInfo manifest(path);
        QString display;
        QString statusText;
        QString titleText;
        QString metadataText;
        const char *statusState = "info";
        if (!manifest.exists()) {
            statusText = tr("Location no longer exists");
            titleText = tr("Unavailable Video Set");
            metadataText = statusText;
            statusState = "warning";
            display = tr("Unavailable Video Set — %1").arg(statusText);
        } else {
            try {
                const auto plan = video_set::read_manifest(
#ifdef Q_OS_WIN
                    std::filesystem::path(path.toStdWString())
#else
                    std::filesystem::path(path.toStdString())
#endif
                );
                statusText = friendlyStatus(plan.aggregate_state);
                titleText = QString::fromStdString(plan.original_filename);
                metadataText = tr("%1 parts").arg(plan.parts.size());
                statusState = plan.aggregate_state == "Recovered exact"
                    ? "success" : plan.aggregate_state == "Incomplete"
                    ? "warning" : "info";
                display = tr("%1\n%2  ·  %3 parts")
                    .arg(QString::fromStdString(plan.original_filename),
                         statusText)
                    .arg(plan.parts.size());
            } catch (const std::exception &) {
                statusText = tr("Manifest could not be read");
                titleText = tr("Unreadable Video Set");
                metadataText = statusText;
                statusState = "error";
                display = tr("Unreadable Video Set — %1").arg(statusText);
            }
        }
        const qint64 openedSeconds = settings.value(
            recent_opened_setting_key(path)).toLongLong();
        const QString openedText = openedSeconds > 0
            ? tr("Last opened: ") + QLocale().toString(
                QDateTime::fromSecsSinceEpoch(openedSeconds),
                QLocale::ShortFormat)
            : tr("Last opened: Not recorded");
        display += "\n" + openedText;
        auto *item = new QListWidgetItem(videoSetRecentList);
        item->setData(Qt::UserRole, path);
        item->setData(Qt::UserRole + 1, statusText);
        item->setData(Qt::UserRole + 2, openedText);
        item->setData(Qt::AccessibleTextRole, display);
        item->setToolTip(path);
        auto *entry = new VidStoreXRecentEntry(
            titleText, metadataText + "  ·  " + openedText,
            statusText, statusState);
        entry->setObjectName(QString("videoSetRecentEntry%1")
            .arg(videoSetRecentList->row(item)));
        auto *title = entry->titleLabel();
        title->setObjectName(QString("videoSetRecentEntryTitle%1")
            .arg(videoSetRecentList->row(item)));
        auto *metadata = entry->metadataLabel();
        metadata->setObjectName(QString("videoSetRecentEntryMetadata%1")
            .arg(videoSetRecentList->row(item)));
        auto *status = entry->statusLabel();
        status->setObjectName(QString("videoSetRecentEntryStatus%1")
            .arg(videoSetRecentList->row(item)));
        status->setAccessibleName(tr("Status: %1").arg(statusText));
        entry->ensurePolished();
        entry->layout()->activate();
        item->setSizeHint(entry->sizeHint());
        videoSetRecentList->setItemWidget(item, entry);
    }
    const bool hasItems = videoSetRecentList->count() != 0;
    videoSetRecentContinueButton->setEnabled(hasItems);
    videoSetRecentOpenFolderButton->setEnabled(hasItems);
    videoSetRecentRemoveButton->setEnabled(hasItems);
    videoSetRecentContinueButton->setVisible(hasItems);
    videoSetRecentOpenFolderButton->setVisible(hasItems);
    videoSetRecentRemoveButton->setVisible(hasItems);
    videoSetRecentList->setVisible(hasItems);
    videoSetRecentEmptyState->setVisible(!hasItems);
    if (hasItems) {
        const int visibleRows = qMin(videoSetRecentList->count(),
            vidstorex_ui::Layout::RecentVisibleRows);
        int listHeight = 2 * videoSetRecentList->frameWidth();
        for (int row = 0; row < visibleRows; ++row)
            listHeight += videoSetRecentList->sizeHintForRow(row);
        videoSetRecentList->setFixedHeight(listHeight);
    } else {
        videoSetRecentList->setFixedHeight(0);
    }
    if (!hasItems)
        videoSetRecentEmptyLabel->setText(tr("No recent Video Sets yet."));
    if (hasItems) videoSetRecentList->setCurrentRow(0);
}

void DriveManagerUI::rememberRecentVideoSet(const QString &manifestPath) {
    if (manifestPath.isEmpty()) return;
    QSettings settings;
    if (!settings.value("ui/rememberRecentSets", true).toBool()) return;
    auto paths = settings.value("videoSet/recentManifests").toStringList();
    paths.removeAll(manifestPath);
    paths.prepend(manifestPath);
    while (paths.size() > 5) paths.removeLast();
    settings.setValue("videoSet/recentManifests", paths);
    settings.setValue(
        recent_opened_setting_key(manifestPath),
        QDateTime::currentSecsSinceEpoch());
    refreshRecentVideoSets();
}

void DriveManagerUI::openRecentVideoSet(const QString &manifestPath) {
    if (!QFileInfo::exists(manifestPath)) {
        videoSetWorkflow.fail(
            "This Video Set location no longer exists.",
            "Remove it from Recent Video Sets or locate the set manually.");
        updateVideoSetAssistant();
        return;
    }
    try {
        const auto plan = video_set::read_manifest(
#ifdef Q_OS_WIN
            std::filesystem::path(manifestPath.toStdWString())
#else
            std::filesystem::path(manifestPath.toStdString())
#endif
        );
        videoSetCurrentManifest = manifestPath;
        videoSetCurrentSetRoot = QFileInfo(manifestPath).absolutePath();
        videoSetWorkflow.resume_from_manifest(plan);
        {
            const QSignalBlocker blocker(videoSetAssistantInputEdit);
            videoSetAssistantInputEdit->setText(
                QString::fromStdString(plan.original_filename));
        }
        videoSetAssistantRecoveryInputEdit->setText(manifestPath);
        if (videoSetWorkflow.view().state ==
                video_set_workflow::State::AwaitingUpload)
            videoSetAssistantStack->setCurrentIndex(5);
        else if (videoSetWorkflow.view().state ==
                     video_set_workflow::State::RecoveredExact)
            videoSetAssistantStack->setCurrentIndex(9);
        else if (videoSetWorkflow.view().state ==
                     video_set_workflow::State::EncodingPaused)
            videoSetAssistantStack->setCurrentIndex(4);
        else
            videoSetAssistantStack->setCurrentIndex(7);
        rememberRecentVideoSet(manifestPath);
        updateVideoSetAssistant();
    } catch (const std::exception &error) {
        videoSetWorkflow.fail(
            "This Video Set manifest could not be opened.", error.what());
        updateVideoSetAssistant();
    }
}

bool DriveManagerUI::eventFilter(QObject *object, QEvent *event) {
    if (object == videoSetWelcomePage && event->type() == QEvent::Resize) {
        auto *resize = static_cast<QResizeEvent *>(event);
        auto *layout = qobject_cast<QVBoxLayout *>(
            videoSetWelcomePage->layout());
        if (layout) {
            const int side = qMax(24,
                (resize->size().width() -
                 vidstorex_ui::Layout::ContentMaxWidth) / 2);
            layout->setContentsMargins(side, 16, side, 18);
        }
    }
    if (object == videoSetSourceDropLabel) {
        if (event->type() == QEvent::DragEnter) {
            auto *drag = static_cast<QDragEnterEvent *>(event);
            if (drag->mimeData()->hasUrls() &&
                drag->mimeData()->urls().size() == 1 &&
                QFileInfo(drag->mimeData()->urls().front().toLocalFile()).isFile()) {
                drag->acceptProposedAction();
                return true;
            }
        } else if (event->type() == QEvent::Drop) {
            auto *drop = static_cast<QDropEvent *>(event);
            const auto urls = drop->mimeData()->urls();
            if (urls.size() == 1) {
                videoSetAssistantInputEdit->setText(
                    urls.front().toLocalFile());
                drop->acceptProposedAction();
                return true;
            }
        }
    }
    return QMainWindow::eventFilter(object, event);
}

void DriveManagerUI::closeEvent(QCloseEvent *event) {
    const bool videoSetRunning = videoSetProcess &&
        videoSetProcess->state() != QProcess::NotRunning;
    const bool downloadRunning = videoSetDownloadProcess &&
        videoSetDownloadProcess->state() != QProcess::NotRunning;
    if (videoSetRunning || downloadRunning) {
        const auto answer = QMessageBox::question(
            this, "Video Set operation is running",
            "Stop the child operation safely and close? Completed verified "
            "parts and recovery state will be kept for Resume.",
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes) {
            event->ignore();
            return;
        }
        QProcess *process = videoSetRunning
            ? videoSetProcess : videoSetDownloadProcess;
        process->terminate();
        if (!process->waitForFinished(3000)) process->kill();
    }
    QMainWindow::closeEvent(event);
}

void DriveManagerUI::changeEvent(QEvent *event) {
    QMainWindow::changeEvent(event);
    if (event->type() == QEvent::LanguageChange && centralWidget)
        retranslateUserInterface();
    if ((event->type() == QEvent::PaletteChange ||
         event->type() == QEvent::ApplicationPaletteChange) && centralWidget)
        vidstorex_ui::applyTheme(centralWidget);
}

void DriveManagerUI::setupMenuBar() {
    // Menu setup - using QMainWindow's built-in menuBar
    QMenu *fileMenu = menuBar()->addMenu("&File");
    fileMenu->setObjectName("fileMenu");
    auto *exitAction = fileMenu->addAction("E&xit", this, &QWidget::close);
    exitAction->setObjectName("exitAction");

    QMenu *toolsMenu = menuBar()->addMenu("&Tools");
    toolsMenu->setObjectName("toolsMenu");
    auto *clearAction = toolsMenu->addAction(
        "&Clear Logs", this, &DriveManagerUI::clearLogs);
    clearAction->setObjectName("clearLogsAction");

    QMenu *helpMenu = menuBar()->addMenu("&Help");
    helpMenu->setObjectName("helpMenu");
    auto *aboutAction = helpMenu->addAction("&About", [this]() {
        QMessageBox::about(this, tr("About VidStoreX"),
                           tr("VidStoreX\n\nTurn files into resilient videos and recover them later.\nVersion 1.4"));
    });
    aboutAction->setObjectName("aboutAction");
}

void DriveManagerUI::setupStatusBar() {
    // Status bar setup - using QMainWindow's built-in statusBar
    permanentStatus = new QLabel("Ready");
    statusBar()->addPermanentWidget(permanentStatus);
    statusBar()->setVisible(false);
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
    if (videoSetCheckBox && videoSetCheckBox->isChecked()) {
        videoSetWorkflow.choose_create();
        videoSetAssistantInputEdit->setText(inputFileEdit->text());
        const QFileInfo outputInfo(outputFileEdit->text());
        videoSetAssistantOutputEdit->setText(outputInfo.absolutePath());
        mainTabs->setCurrentWidget(videoSetPage);
        videoSetAssistantStack->setCurrentIndex(1);
        updateVideoSetAssistant();
        return;
    }
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
    const int profile_id =
        reliabilityProfileCombo->itemData(index).toInt();
    const bool fastLocal =
        encodingModeCombo->currentData().toInt() ==
        MS_ENCODING_MODE_FAST_LOCAL;
    const bool custom = profile_id < 0 && !fastLocal;
    repairPercentSpinBox->setEnabled(custom);
    if (!custom) {
        repairPercentSpinBox->setValue(
            reliability_profile_definition(
                reliability_profile_from_id(profile_id))
                .repair_percentage);
    }
    if (!fastLocal && profile_id ==
            static_cast<int>(ReliabilityProfile::HighCapacity)) {
        reliabilityHelpLabel->setText(
            "4x4 one-bit geometry with 5% repair. Provides approximately "
            "4x the useful capacity of the Resilient geometry and passed "
            "a six-case real YouTube stress validation. Geometry: 4x4; "
            "modulation: 1-bit; signal: 1.0; validation: 6/6 exact; "
            "intended use: shorter videos / higher capacity. Resilient "
            "remains the safest default.");
    } else if (!fastLocal) {
        reliabilityHelpLabel->setText(
            "Higher repair improves damage tolerance, but increases "
            "frames, time, and output size. Resilient remains the safest "
            "default profile.");
    }
    onPreflightInputChanged();
}

void DriveManagerUI::onEncodingModeChanged(const int) {
    const bool fastLocal =
        encodingModeCombo->currentData().toInt() ==
        MS_ENCODING_MODE_FAST_LOCAL;
    reliabilityProfileCombo->setEnabled(!fastLocal);
    repairPercentSpinBox->setEnabled(
        !fastLocal && isCustomReliabilityProfile());
    if (fastLocal) {
        reliabilityHelpLabel->setText(
            "Not applicable in Fast Local Mode. No repair/FEC packets "
            "are generated.");
    }
    encodingModeHelpLabel->setText(
        fastLocal
            ? "Fast Local is optimized for lossless local storage and "
              "requires an .mkv output. Re-encoding or uploading the "
              "video to lossy platforms may destroy the data."
            : "Resilient / Platform produces larger output, but is more "
              "tolerant of re-encoding and supports reliability/FEC "
              "profiles.");
    if (fastLocal) {
        onPreflightInputChanged();
    } else {
        onReliabilityProfileChanged(
            reliabilityProfileCombo->currentIndex());
    }
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
        !isCustomReliabilityProfile()) {
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
            : reliabilityProfileCombo->currentData().toInt();
    fingerprint.repair_ratio =
        encodingModeCombo->currentData().toInt() ==
                MS_ENCODING_MODE_FAST_LOCAL
            ? 0.0
            : selectedReliabilityOptions().repair_ratio;
    fingerprint.encrypted = encryptCheckBox->isChecked();
    fingerprint.encoding_mode = selectedEncodingMode();
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
                          : e.encoding_mode ==
                                MS_ENCODING_MODE_HIGH_CAPACITY
                          ? "High Capacity"
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
        if (e.encoding_mode == MS_ENCODING_MODE_HIGH_CAPACITY) {
            lines << "  Geometry: 4x4"
                  << "  Modulation: 1-bit"
                  << "  Signal: 1.0"
                  << QString("  Config ID: %1")
                         .arg(QString::fromStdString(
                             reliability_profile_config_id(
                                 ReliabilityProfile::HighCapacity)))
                  << "  Validation: Real YouTube tested, 6/6 exact"
                  << "  Useful capacity: approximately 4x the "
                     "same-resolution Resilient geometry"
                  << "  Actual encoded file size may vary.";
        }
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

    if (selectedReliabilityProfile() ==
            ReliabilityProfile::HighCapacity) {
        QMessageBox::warning(
            this, "High Capacity file profile",
            "High Capacity is currently available for file encode. "
            "Streaming keeps its existing geometry; select Resilient, "
            "Balanced, Durable, or Custom for stream encode.");
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
                if (capacityPresetCombo &&
                    capacityPresetCombo->currentIndex() == 2 &&
                    line.trimmed().startsWith(
                        "Required with safety margin:"))
                    capacityEstimateLabel->setText(
                        "Boundary: exactly 7 videos; " +
                        line.trimmed());
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
            item.value("boundary_case_id").toString().isEmpty()
                ? "-"
                : item.value("boundary_case_id").toString(),
            item.value("config_id").toString(),
            item.value("session_group").toString().isEmpty()
                ? "-" : item.value("session_group").toString(),
            item.value("payload_instance_id").toString().isEmpty()
                ? "-" : item.value("payload_instance_id").toString(),
            item.value("source_sha256").toString().left(8).isEmpty()
                ? "-" : item.value("source_sha256").toString().left(8),
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
                item.value("boundary_density_gain").toDouble() > 0.0
                    ? item.value("boundary_density_gain").toDouble()
                    : item.value("useful_payload_gain").toDouble(),
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
            item.value("local_evidence_status").toString().isEmpty()
                ? item.value("local_gate_status").toString()
                : item.value("local_evidence_status").toString(),
            item.value("real_youtube_status").toString().isEmpty()
                ? "Not uploaded/tested"
                : item.value("real_youtube_status").toString(),
            item.value("overall_evidence_status").toString().isEmpty()
                ? status
                : item.value("overall_evidence_status").toString()
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
    if (isCustomReliabilityProfile()) {
        return {repair_percentage_to_ratio(
            repairPercentSpinBox->value())};
    }
    return reliability_options_for_profile(
        selectedReliabilityProfile());
}

ReliabilityProfile DriveManagerUI::selectedReliabilityProfile() const {
    return reliability_profile_from_id(
        reliabilityProfileCombo->currentData().toInt());
}

ms_encoding_mode_t DriveManagerUI::selectedEncodingMode() const {
    if (encodingModeCombo->currentData().toInt() ==
            MS_ENCODING_MODE_FAST_LOCAL)
        return MS_ENCODING_MODE_FAST_LOCAL;
    return selectedReliabilityProfile() ==
            ReliabilityProfile::HighCapacity
        ? MS_ENCODING_MODE_HIGH_CAPACITY
        : MS_ENCODING_MODE_RESILIENT;
}

bool DriveManagerUI::isCustomReliabilityProfile() const {
    return reliabilityProfileCombo->currentData().toInt() < 0;
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
    uiLanguage = vidstorex_ui::resolve_language(
        settings.value("ui/language").toString(),
        QLocale::system().name());
    restoreGeometry(settings.value("geometry").toByteArray());
    restoreState(settings.value("windowState").toByteArray());
}

void DriveManagerUI::saveSettings() const {
    QSettings settings;
    settings.setValue("geometry", saveGeometry());
    settings.setValue("windowState", saveState());
    settings.setValue("ui/language", uiLanguage);
    const int profile_id =
        reliabilityProfileCombo->currentData().toInt();
    settings.setValue(
        "encoding/reliabilityProfileId",
        profile_id >= 0
            ? profile_id
            : static_cast<int>(ReliabilityProfile::Local));
    if (videoSetAssistantOutputEdit)
        settings.setValue(
            "videoSet/lastOutputRoot",
            videoSetAssistantOutputEdit->text());
    if (videoSetAssistantRecoveryOutputEdit)
        settings.setValue(
            "videoSet/lastRecoveryOutput",
            videoSetAssistantRecoveryOutputEdit->text());
    if (videoSetPlaylistUrlEdit)
        settings.setValue(
            "videoSet/lastPlaylistUrl",
            videoSetPlaylistUrlEdit->text());
    if (videoSetAdvancedSettingsButton)
        settings.setValue(
            "videoSet/advancedVisible",
            videoSetAdvancedSettingsButton->isChecked());
    if (videoSetClassicToolsGroup)
        settings.setValue(
            "videoSet/classicVisible",
            videoSetClassicToolsGroup->isChecked());
    if (settingsOutputEdit)
        settings.setValue(
            "ui/defaultVideoSetOutputFolder",
            settingsOutputEdit->text());
    if (rememberRecentCheckBox)
        settings.setValue(
            "ui/rememberRecentSets",
            rememberRecentCheckBox->isChecked());
    if (showAdvancedToolsCheckBox)
        settings.setValue(
            "ui/showAdvancedTools",
            showAdvancedToolsCheckBox->isChecked());
}
