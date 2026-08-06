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
#include <QTimer>
#include <QStyleFactory>

#include "drive_manager_ui.h"

int main(int argc, char *argv[]) {
    bool smokeTest = false;
    bool closeDuringEstimate = false;
    QString preflightSmokeInput;
    QString preflightSmokeOutput;
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
        }
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
        if (!profiles || !repair || !help) {
            qCritical() << "profile controls were not found";
            return 2;
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
