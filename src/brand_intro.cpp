#include "brand_intro.h"

#include "app_branding.h"

#include <QAbstractAnimation>
#include <QBoxLayout>
#include <QEasingCurve>
#include <QGraphicsOpacityEffect>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QParallelAnimationGroup>
#include <QPropertyAnimation>
#include <QSequentialAnimationGroup>

#include <utility>

namespace {

class BrandDataRail final : public QWidget {
public:
    explicit BrandDataRail(QWidget *parent = nullptr) : QWidget(parent) {
        setFixedSize(128, 8);
        setFocusPolicy(Qt::NoFocus);
        setAttribute(Qt::WA_TransparentForMouseEvents);
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, false);
        constexpr int cells = 9;
        constexpr int gap = 4;
        const int width = (this->width() - gap * (cells - 1)) / cells;
        const int active[] = {0, 2, 3, 5, 8};
        painter.setPen(Qt::NoPen);
        for (int index = 0; index < cells; ++index) {
            bool highlighted = false;
            for (const int cell : active)
                if (cell == index) highlighted = true;
            painter.setBrush(highlighted ? QColor(213, 138, 32, 170)
                                         : QColor(93, 86, 75, 90));
            painter.drawRect(index * (width + gap), 1, width, height() - 2);
        }
    }
};

QPropertyAnimation *opacityAnimation(QGraphicsOpacityEffect *effect,
                                     const qreal from, const qreal to,
                                     const int duration,
                                     const QEasingCurve::Type easing) {
    auto *animation = new QPropertyAnimation(effect, "opacity");
    animation->setStartValue(from);
    animation->setEndValue(to);
    animation->setDuration(duration);
    animation->setEasingCurve(easing);
    return animation;
}

} // namespace

BrandIntroOverlay::BrandIntroOverlay(const QString &definition, QWidget *parent)
    : QWidget(parent) {
    setObjectName(QStringLiteral("brandIntroOverlay"));
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_DeleteOnClose, false);
    setStyleSheet(QStringLiteral(
        "QWidget#brandIntroOverlay { background: #1A1A1A; }"
        "QLabel#brandIntroName { color: #F4EFE6; font-size: 42px; "
        "font-weight: 600; letter-spacing: 5px; }"
        "QLabel#brandIntroDefinition { color: #B8B1A7; font-size: 16px; }"));

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(48, 48, 48, 48);
    outer->addStretch(1);
    auto *name = new QLabel(QStringLiteral("RELIQUARY"), this);
    name->setObjectName(QStringLiteral("brandIntroName"));
    name->setAlignment(Qt::AlignCenter);
    name->setAccessibleName(QStringLiteral("Reliquary"));
    auto *rail = new BrandDataRail(this);
    rail->setObjectName(QStringLiteral("brandIntroSignalRail"));
    auto *definitionLabel = new QLabel(definition, this);
    definitionLabel->setObjectName(QStringLiteral("brandIntroDefinition"));
    definitionLabel->setAlignment(Qt::AlignCenter);
    definitionLabel->setWordWrap(false);
    definitionLabel->setAccessibleName(definition);

    outer->addWidget(name);
    outer->addSpacing(18);
    outer->addWidget(rail, 0, Qt::AlignHCenter);
    outer->addSpacing(20);
    outer->addWidget(definitionLabel);
    outer->addStretch(1);

    setAccessibleName(QStringLiteral("Reliquary brand introduction"));
    setAccessibleDescription(QStringLiteral("Reliquary. ") + definition);

    auto *nameOpacity = new QGraphicsOpacityEffect(name);
    auto *railOpacity = new QGraphicsOpacityEffect(rail);
    auto *definitionOpacity = new QGraphicsOpacityEffect(definitionLabel);
    nameOpacity->setOpacity(0.0);
    railOpacity->setOpacity(0.0);
    definitionOpacity->setOpacity(0.0);
    name->setGraphicsEffect(nameOpacity);
    rail->setGraphicsEffect(railOpacity);
    definitionLabel->setGraphicsEffect(definitionOpacity);

    animation_ = new QSequentialAnimationGroup(this);
    animation_->addPause(350);
    auto *nameIn = new QParallelAnimationGroup(animation_);
    nameIn->addAnimation(opacityAnimation(
        nameOpacity, 0.0, 1.0, 500, QEasingCurve::OutCubic));
    nameIn->addAnimation(opacityAnimation(
        railOpacity, 0.0, 1.0, 500, QEasingCurve::OutCubic));
    animation_->addAnimation(nameIn);
    animation_->addPause(250);
    animation_->addAnimation(opacityAnimation(
        definitionOpacity, 0.0, 1.0, 450, QEasingCurve::OutCubic));
    animation_->addPause(1000);
    auto *allOut = new QParallelAnimationGroup(animation_);
    allOut->addAnimation(opacityAnimation(
        nameOpacity, 1.0, 0.0, 650, QEasingCurve::InOutCubic));
    allOut->addAnimation(opacityAnimation(
        railOpacity, 1.0, 0.0, 650, QEasingCurve::InOutCubic));
    allOut->addAnimation(opacityAnimation(
        definitionOpacity, 1.0, 0.0, 650, QEasingCurve::InOutCubic));
    animation_->addAnimation(allOut);
    connect(animation_, &QSequentialAnimationGroup::finished,
            this, [this]() { finish(); });
}

void BrandIntroOverlay::start() {
    if (finished_ || animation_->state() == QAbstractAnimation::Running) return;
    show();
    raise();
    setFocus(Qt::OtherFocusReason);
    animation_->start();
}

void BrandIntroOverlay::setFinishedCallback(std::function<void()> callback) {
    finishedCallback_ = std::move(callback);
}

void BrandIntroOverlay::keyPressEvent(QKeyEvent *event) {
    if (event->key() == Qt::Key_Escape || event->key() == Qt::Key_Enter ||
        event->key() == Qt::Key_Return || event->key() == Qt::Key_Space) {
        event->accept();
        finish();
        return;
    }
    QWidget::keyPressEvent(event);
}

void BrandIntroOverlay::mousePressEvent(QMouseEvent *event) {
    event->accept();
    finish();
}

void BrandIntroOverlay::finish() {
    if (finished_) return;
    finished_ = true;
    if (animation_->state() != QAbstractAnimation::Stopped) animation_->stop();
    hide();
    const auto callback = std::move(finishedCallback_);
    if (callback) callback();
    deleteLater();
}
