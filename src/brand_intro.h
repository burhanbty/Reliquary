#pragma once

#include <QWidget>

#include <functional>

class QSequentialAnimationGroup;

class BrandIntroOverlay final : public QWidget {
public:
    explicit BrandIntroOverlay(const QString &definition,
                               QWidget *parent = nullptr);

    static constexpr int totalDurationMs() noexcept { return 3200; }
    void start();
    void setFinishedCallback(std::function<void()> callback);

protected:
    void keyPressEvent(QKeyEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;

private:
    void finish();

    QSequentialAnimationGroup *animation_ = nullptr;
    std::function<void()> finishedCallback_;
    bool finished_ = false;
};
