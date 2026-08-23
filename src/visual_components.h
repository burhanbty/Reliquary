#pragma once

#include <QFrame>
#include <QVector>
#include <QWidget>
#include <QStringList>

class QBoxLayout;
class QResizeEvent;

enum class VidStoreXPartState {
    Pending,
    Active,
    Complete,
    Verified,
    Missing,
    Corrupt,
    Conflict,
    Failed,
    Cancelled
};

class VidStoreXBlockProgress final : public QWidget {
public:
    enum class State { Determinate, Indeterminate, Success, Error, Paused };

    explicit VidStoreXBlockProgress(QWidget *parent = nullptr);
    void setProgress(quint64 value, quint64 maximum);
    void setState(State state);
    [[nodiscard]] quint64 value() const noexcept { return value_; }
    [[nodiscard]] quint64 maximum() const noexcept { return maximum_; }
    [[nodiscard]] State state() const noexcept { return state_; }
    [[nodiscard]] QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    quint64 value_ = 0;
    quint64 maximum_ = 0;
    State state_ = State::Indeterminate;
};

class VidStoreXPartGrid final : public QWidget {
public:
    enum class DisplayMode { Individual, Compact, Aggregated };

    explicit VidStoreXPartGrid(QWidget *parent = nullptr);
    void setParts(const QVector<VidStoreXPartState> &parts);
    [[nodiscard]] const QVector<VidStoreXPartState> &parts() const noexcept {
        return parts_;
    }
    [[nodiscard]] DisplayMode displayMode() const noexcept;
    [[nodiscard]] static DisplayMode displayModeForCount(int count) noexcept;
    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QVector<VidStoreXPartState> parts_;
};

class VidStoreXProcessingFlow final : public QWidget {
public:
    enum class Mode { Create, Download, Scan, Recover, Verify };
    enum class PresentationMode { Normal, Compact };

    explicit VidStoreXProcessingFlow(QWidget *parent = nullptr);
    void setMode(Mode mode);
    void setPresentationMode(PresentationMode mode);
    void setParts(const QVector<VidStoreXPartState> &parts);
    void setFileProgress(quint64 value, quint64 maximum, bool determinate);
    [[nodiscard]] Mode mode() const noexcept { return mode_; }
    [[nodiscard]] PresentationMode presentationMode() const noexcept {
        return presentationMode_;
    }
    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    Mode mode_ = Mode::Create;
    PresentationMode presentationMode_ = PresentationMode::Normal;
    QVector<VidStoreXPartState> parts_;
    quint64 fileValue_ = 0;
    quint64 fileMaximum_ = 0;
    bool fileDeterminate_ = false;
};

class VidStoreXSignalRail final : public QWidget {
public:
    explicit VidStoreXSignalRail(QWidget *parent = nullptr);
    [[nodiscard]] QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;
};

class VidStoreXDataGlyph final : public QWidget {
public:
    enum class Mode { FileToBlocks, BlocksToFile, Verified, Missing, Empty };

    explicit VidStoreXDataGlyph(Mode mode, QWidget *parent = nullptr);
    void setMode(Mode mode);
    [[nodiscard]] Mode mode() const noexcept { return mode_; }
    [[nodiscard]] QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    Mode mode_;
};

class VidStoreXFlowIllustration final : public QWidget {
public:
    enum class Mode { Create, Store, Recover };

    explicit VidStoreXFlowIllustration(Mode mode,
                                       QWidget *parent = nullptr);
    void setMode(Mode mode);
    [[nodiscard]] Mode mode() const noexcept { return mode_; }
    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    Mode mode_;
};

class VidStoreXOnboardingProgress final : public QWidget {
public:
    explicit VidStoreXOnboardingProgress(QWidget *parent = nullptr);
    void setCurrentPage(int page);
    [[nodiscard]] int currentPage() const noexcept { return currentPage_; }
    [[nodiscard]] QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    int currentPage_ = 0;
};

class QLabel;

class VidStoreXRecentEntry final : public QFrame {
public:
    explicit VidStoreXRecentEntry(const QString &title,
                                  const QString &metadata,
                                  const QString &status,
                                  const char *statusState,
                                  QWidget *parent = nullptr);
    [[nodiscard]] QLabel *titleLabel() const noexcept { return title_; }
    [[nodiscard]] QLabel *metadataLabel() const noexcept { return metadata_; }
    [[nodiscard]] QLabel *statusLabel() const noexcept { return status_; }
    [[nodiscard]] QSize sizeHint() const override;

private:
    QLabel *title_ = nullptr;
    QLabel *metadata_ = nullptr;
    QLabel *status_ = nullptr;
};

class VidStoreXStepper final : public QWidget {
public:
    explicit VidStoreXStepper(QWidget *parent = nullptr);
    void setSteps(const QStringList &steps, int active, int error = -1);
    [[nodiscard]] const QStringList &steps() const noexcept { return steps_; }
    [[nodiscard]] int activeStep() const noexcept { return active_; }
    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QStringList steps_;
    int active_ = 0;
    int error_ = -1;
};

class VidStoreXResponsiveColumns final : public QWidget {
public:
    explicit VidStoreXResponsiveColumns(QWidget *parent = nullptr);
    void addWidget(QWidget *widget, int stretch = 1);
    void setBreakpoint(int logicalPixels);
    [[nodiscard]] bool isStacked() const noexcept { return stacked_; }

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    void updateDirection(int availableWidth);
    QBoxLayout *layout_ = nullptr;
    int breakpoint_ = 1040;
    bool stacked_ = true;
};
