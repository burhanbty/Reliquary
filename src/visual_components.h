#pragma once

#include <QWidget>
#include <QStringList>

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
