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

#include <vector>

WorkerThread::WorkerThread(const Operation op, const QString &input, const QString &output,
                           const bool encrypt, const QString &password,
                           const QString &streamUrl, const int bitrate,
                           const int streamWidth, const int streamHeight,
                           const int streamFps, const double repairRatio,
                           QObject *parent)
    : QThread(parent), operation(op), inputPath(input), outputPath(output),
      encrypt(encrypt), password(password), streamUrl(streamUrl), bitrate(bitrate),
      streamWidth(streamWidth), streamHeight(streamHeight), streamFps(streamFps),
      repairRatio(repairRatio) {
}

static int gui_encode_progress(const uint64_t current, const uint64_t total, void *user) {
    auto *thread = static_cast<WorkerThread *>(user);
    if (total > 0) {
        const int pct = 5 + static_cast<int>(90 * (current + 1) / total);
        emit thread->progressUpdated(pct);
    }
    return 0;
}

static int gui_decode_progress(const uint64_t current, const uint64_t total, void *user) {
    auto *thread = static_cast<WorkerThread *>(user);
    if (total > 0) {
        const int pct = 10 + static_cast<int>(70 * current / total);
        emit thread->progressUpdated(pct);
    }
    return 0;
}

static int gui_stream_encode_progress(const uint64_t current, const uint64_t total, void *user) {
    auto *thread = static_cast<WorkerThread *>(user);
    if (total > 0) {
        const int pct = 5 + static_cast<int>(90 * (current + 1) / total);
        emit thread->progressUpdated(pct);
    }
    return 0;
}

static int gui_stream_decode_progress(const uint64_t current, const uint64_t total, void *user) {
    auto *thread = static_cast<WorkerThread *>(user);
    if (total > 0) {
        const int pct = 10 + static_cast<int>(70 * current / total);
        emit thread->progressUpdated(pct);
    } else if (current > 0) {
        emit thread->progressUpdated(std::min(static_cast<int>(current % 90) + 10, 95));
    }
    return 0;
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

        ms_result_t result{};

        if (const ms_status_t status = ms_encode(&opts, &result); status == MS_OK) {
            emit logMessage(QString("Input size: %1 bytes").arg(result.input_size));
            emit logMessage(QString("Chunks: %1").arg(result.total_chunks));
            emit logMessage(QString("Generated %1 packets in %2 frames")
                .arg(result.total_packets).arg(result.total_frames));
            emit_performance_report(this, result);
            emit progressUpdated(100);
            emit operationCompleted(true, "Encoding completed successfully");
        } else {
            emit operationCompleted(false, QString("Error: %1").arg(ms_status_string(status)));
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
            emit operationCompleted(true, "Decoding completed successfully");
        } else {
            emit operationCompleted(false, QString("Error: %1").arg(ms_status_string(status)));
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
            emit operationCompleted(true, "Stream encode completed successfully");
        } else {
            emit operationCompleted(false, QString("Stream encode error: %1").arg(ms_status_string(status)));
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
            emit operationCompleted(true, "Stream decode completed successfully");
        } else {
            emit operationCompleted(false, QString("Stream decode error: %1").arg(ms_status_string(status)));
        }
    }
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

    resetProgress();
    logMessage("Drive Manager initialized");
}

DriveManagerUI::~DriveManagerUI() {
    if (workerThread && workerThread->isRunning()) {
        workerThread->quit();
        workerThread->wait();
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
    inputFileEdit->setReadOnly(true);
    fileOpsLayout->addWidget(inputFileEdit, 0, 1);

    selectInputButton = new QPushButton("Browse...");
    fileOpsLayout->addWidget(selectInputButton, 0, 2);

    fileOpsLayout->addWidget(new QLabel("Output File:"), 1, 0);
    outputFileEdit = new QLineEdit();
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

    fileOpsLayout->addWidget(new QLabel("Reliability:"), 4, 0);
    reliabilityProfileCombo = new QComboBox();
    reliabilityProfileCombo->addItem("Local / Fast (5%)", 5.0);
    reliabilityProfileCombo->addItem("Balanced (20%)", 20.0);
    reliabilityProfileCombo->addItem("Durable (50%)", 50.0);
    reliabilityProfileCombo->addItem("Custom", -1.0);
    fileOpsLayout->addWidget(reliabilityProfileCombo, 4, 1);

    repairPercentSpinBox = new QDoubleSpinBox();
    repairPercentSpinBox->setRange(0.0, MAX_REPAIR_PERCENTAGE);
    repairPercentSpinBox->setDecimals(2);
    repairPercentSpinBox->setSingleStep(0.5);
    repairPercentSpinBox->setValue(DEFAULT_REPAIR_PERCENTAGE);
    repairPercentSpinBox->setSuffix("%");
    repairPercentSpinBox->setEnabled(false);
    fileOpsLayout->addWidget(repairPercentSpinBox, 4, 2);

    reliabilityHelpLabel = new QLabel(
        "Higher repair improves damage tolerance, but increases frames, time, and output size.");
    reliabilityHelpLabel->setWordWrap(true);
    reliabilityHelpLabel->setStyleSheet("color: palette(mid); font-size: 9pt;");
    fileOpsLayout->addWidget(reliabilityHelpLabel, 5, 0, 1, 3);

    encodeButton = new QPushButton("Encode to Video");
    encodeButton->setIcon(QIcon::fromTheme("media-record"));
    fileOpsLayout->addWidget(encodeButton, 6, 0, 1, 3);

    decodeButton = new QPushButton("Decode from Video");
    decodeButton->setIcon(QIcon::fromTheme("media-playback-start"));
    fileOpsLayout->addWidget(decodeButton, 7, 0, 1, 3);

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

    // Main layout
    auto *mainLayout = new QHBoxLayout(centralWidget);
    mainLayout->addWidget(mainSplitter);
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

    EncodingReliabilityOptions reliability;
    try {
        reliability = selectedReliabilityOptions();
        const auto estimate = estimate_encoding_reliability(
            static_cast<uint64_t>(QFileInfo(inputFileEdit->text()).size()),
            encrypt, reliability,
            static_cast<uint64_t>(VideoEncoder::packets_per_frame()),
            FRAME_FPS);
        logReliabilityEstimate(estimate, reliability);
    } catch (const std::exception &error) {
        QMessageBox::warning(
            this, "Invalid repair percentage",
            QString::fromUtf8(error.what()));
        return;
    }

    isOperationRunning = true;
    currentOperation = "Encoding";
    encodeButton->setEnabled(false);
    decodeButton->setEnabled(false);

    workerThread = std::make_unique<WorkerThread>(WorkerThread::Encode,
                                                  inputFileEdit->text(), outputFileEdit->text(), encrypt,
                                                  passwordEdit->text(), QString(), 35000,
                                                  0, 0, 0,
                                                  reliability.repair_ratio, this);

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
                                                  DEFAULT_REPAIR_RATIO, this);

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

void DriveManagerUI::onReliabilityProfileChanged(const int index) const {
    const double percentage =
        reliabilityProfileCombo->itemData(index).toDouble();
    const bool custom = percentage < 0.0;
    repairPercentSpinBox->setEnabled(custom);
    if (!custom) {
        repairPercentSpinBox->setValue(percentage);
    }
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
                                                  reliability.repair_ratio, this);

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
                                                  DEFAULT_REPAIR_RATIO, this);

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

void DriveManagerUI::onOperationCompleted(const bool success, const QString &message) {
    isOperationRunning = false;
    encodeButton->setEnabled(true);
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
    workerThread.reset();
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
