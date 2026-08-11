#pragma once

#include <QDateTime>
#include <QDialog>
#include <QImage>
#include <QString>

#include <cstdint>
#include <optional>

class QCheckBox;
class QLabel;
class QPushButton;
class QResizeEvent;

namespace result_card {

enum class Type {
    VideoSetCreated,
    FileRecoveredExact
};

enum class SourceKind {
    Local,
    YouTubePlaylist,
    Unknown
};

enum class RecoveryStatus {
    RecoveredExact,
    ShaMismatch,
    RecoveryFailed,
    SetIncomplete,
    Conflict,
    Cancelled
};

struct PrivacyOptions {
    bool showFileName = true;
    bool showFileSize = true;
    bool showProfile = true;
    bool showShortSha = true;
    bool showFullSha = false;
    bool showTechnicalDetails = false;
};

struct CreateEvidence {
    QString filePath;
    quint64 fileSizeBytes = 0;
    QString profileName;
    uint32_t partCount = 0;
    uint32_t verifiedPartCount = 0;
    bool allPartsCreated = false;
    bool localVerificationPassed = false;
    QDateTime completedAt;
    QString appVersion;
};

struct RecoveryEvidence {
    RecoveryStatus status = RecoveryStatus::RecoveryFailed;
    QString recoveredFilePath;
    quint64 fileSizeBytes = 0;
    QString profileName;
    uint32_t partCount = 0;
    uint32_t verifiedPartCount = 0;
    SourceKind sourceKind = SourceKind::Unknown;
    QString sha256;
    bool returnedPartsExact = false;
    bool finalShaExact = false;
    QDateTime completedAt;
    QString appVersion;
};

struct Model {
    Type type = Type::VideoSetCreated;
    QString fileName;
    quint64 fileSizeBytes = 0;
    QString profileName;
    uint32_t partCount = 0;
    uint32_t verifiedPartCount = 0;
    SourceKind sourceKind = SourceKind::Unknown;
    QString sha256;
    bool shaVerified = false;
    QDateTime timestamp;
    QString appVersion;
    bool localVerificationPassed = false;
    bool youtubeRoundTripVerified = false;
    QString localeName = QStringLiteral("en_US");
};

[[nodiscard]] std::optional<Model> makeCreateModel(
    const CreateEvidence &evidence, const QString &localeName);
[[nodiscard]] std::optional<Model> makeRecoveryModel(
    const RecoveryEvidence &evidence, const QString &localeName);

class Renderer final {
public:
    static constexpr int Width = 1600;
    static constexpr int Height = 900;

    [[nodiscard]] static QImage render(
        const Model &model,
        const PrivacyOptions &privacy = {});
    [[nodiscard]] static QString visibleText(
        const Model &model,
        const PrivacyOptions &privacy = {});
    [[nodiscard]] static QString accessibleSummary(const Model &model);
};

struct ExportResult {
    bool ok = false;
    QString path;
    QString error;
};

[[nodiscard]] QString suggestedFileName(const Model &model);
[[nodiscard]] ExportResult savePng(
    const QImage &image, QString path, bool allowOverwrite = false);

class PreviewDialog final : public QDialog {
public:
    explicit PreviewDialog(Model model, QWidget *parent = nullptr);

    [[nodiscard]] const Model &model() const noexcept { return model_; }
    [[nodiscard]] PrivacyOptions privacyOptions() const;
    [[nodiscard]] QImage renderedImage() const { return rendered_; }

protected:
    void changeEvent(QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    void retranslate();
    void rerender();
    void updatePreviewPixmap();
    void saveImage();
    void copyImage();

    Model model_;
    QImage rendered_;
    QLabel *title_ = nullptr;
    QLabel *preview_ = nullptr;
    QLabel *status_ = nullptr;
    QCheckBox *showFileName_ = nullptr;
    QCheckBox *showFileSize_ = nullptr;
    QCheckBox *showProfile_ = nullptr;
    QCheckBox *showShortSha_ = nullptr;
    QCheckBox *showFullSha_ = nullptr;
    QCheckBox *showTechnicalDetails_ = nullptr;
    QPushButton *copyButton_ = nullptr;
    QPushButton *saveButton_ = nullptr;
    QPushButton *closeButton_ = nullptr;
};

} // namespace result_card
