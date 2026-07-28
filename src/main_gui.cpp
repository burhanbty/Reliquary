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
