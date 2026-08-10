#include "visual_components.h"

#include "ui_theme.h"

#include <QPainter>
#include <QPainterPath>
#include <QHBoxLayout>
#include <QLabel>
#include <QVBoxLayout>

VidStoreXSignalRail::VidStoreXSignalRail(QWidget *parent) : QWidget(parent) {
    setObjectName(QStringLiteral("vidStoreXSignalRail"));
    setFocusPolicy(Qt::NoFocus);
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAccessibleName({});
    setAccessibleDescription({});
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setMinimumHeight(8);
}

QSize VidStoreXSignalRail::sizeHint() const { return {240, 8}; }

void VidStoreXSignalRail::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, false);
    const auto t = vidstorex_ui::themeTokens(palette());
    if (width() <= 0 || height() <= 0) return;
    constexpr int pattern[] = {3, 1, 2, 4, 1, 3, 2, 1, 4, 2, 3, 1};
    const int gap = qMax(2, qRound(devicePixelRatioF() * 2.0));
    const int unit = qMax(3, (width() - gap * 11) / 27);
    int x = 0;
    for (int i = 0; i < 12 && x < width(); ++i) {
        const int blockWidth = qMin(width() - x, pattern[i] * unit);
        QColor color = (i % 4 == 0 || i == 7) ? t.accent : t.borderStrong;
        if (i == 3 || i == 10) color = t.success;
        painter.fillRect(QRect(x, 1, blockWidth, qMax(2, height() - 2)), color);
        x += blockWidth + gap;
    }
}

VidStoreXDataGlyph::VidStoreXDataGlyph(const Mode mode, QWidget *parent)
    : QWidget(parent), mode_(mode) {
    setFocusPolicy(Qt::NoFocus);
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAccessibleName({});
    setAccessibleDescription({});
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
}

void VidStoreXDataGlyph::setMode(const Mode mode) {
    if (mode_ == mode) return;
    mode_ = mode;
    update();
}

QSize VidStoreXDataGlyph::sizeHint() const { return {68, 48}; }

void VidStoreXDataGlyph::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    const auto t = vidstorex_ui::themeTokens(palette());
    const qreal scale = qMin(width() / 68.0, height() / 48.0);
    p.translate((width() - 68 * scale) / 2.0, (height() - 48 * scale) / 2.0);
    p.scale(scale, scale);
    QPen line(t.borderStrong, 2);
    line.setJoinStyle(Qt::MiterJoin);
    p.setPen(line);
    p.setBrush(Qt::NoBrush);

    const auto drawFile = [&]() {
        QPainterPath file;
        file.moveTo(4, 5); file.lineTo(23, 5); file.lineTo(32, 14);
        file.lineTo(32, 43); file.lineTo(4, 43); file.closeSubpath();
        file.moveTo(23, 5); file.lineTo(23, 14); file.lineTo(32, 14);
        p.drawPath(file);
        p.drawLine(10, 23, 25, 23);
        p.drawLine(10, 29, 25, 29);
    };
    const auto drawBlocks = [&](const QColor &highlight) {
        for (int row = 0; row < 3; ++row) {
            for (int column = 0; column < 3; ++column) {
                QRect block(43 + column * 8, 12 + row * 8, 6, 6);
                p.fillRect(block, (row + column) % 3 == 0
                                      ? highlight : t.borderStrong);
            }
        }
    };
    if (mode_ == Mode::FileToBlocks || mode_ == Mode::BlocksToFile) {
        drawFile();
        p.setPen(QPen(t.accent, 2));
        p.drawLine(35, 24, 40, 24);
        if (mode_ == Mode::FileToBlocks) {
            p.drawLine(38, 21, 41, 24); p.drawLine(38, 27, 41, 24);
        } else {
            p.drawLine(38, 21, 35, 24); p.drawLine(38, 27, 35, 24);
        }
        drawBlocks(t.accent);
        return;
    }
    if (mode_ == Mode::Empty) {
        drawBlocks(t.accent);
        p.setPen(QPen(t.borderStrong, 2));
        p.drawLine(7, 16, 29, 16);
        p.drawLine(7, 24, 24, 24);
        p.drawLine(7, 32, 19, 32);
        return;
    }
    drawBlocks(mode_ == Mode::Verified ? t.success : t.error);
    p.setPen(QPen(mode_ == Mode::Verified ? t.success : t.error, 3,
                  Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    if (mode_ == Mode::Verified) {
        p.drawLine(7, 26, 15, 34); p.drawLine(15, 34, 31, 13);
    } else {
        p.drawLine(10, 14, 29, 34); p.drawLine(29, 14, 10, 34);
    }
}

VidStoreXFlowIllustration::VidStoreXFlowIllustration(
    const Mode mode, QWidget *parent)
    : QWidget(parent), mode_(mode) {
    setObjectName(QStringLiteral("onboardingFlowIllustration"));
    setFocusPolicy(Qt::NoFocus);
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAccessibleName({});
    setAccessibleDescription({});
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

void VidStoreXFlowIllustration::setMode(const Mode mode) {
    if (mode_ == mode) return;
    mode_ = mode;
    update();
}

QSize VidStoreXFlowIllustration::sizeHint() const { return {680, 190}; }
QSize VidStoreXFlowIllustration::minimumSizeHint() const { return {420, 140}; }

void VidStoreXFlowIllustration::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    const auto t = vidstorex_ui::themeTokens(palette());
    constexpr qreal logicalWidth = 680.0;
    constexpr qreal logicalHeight = 190.0;
    const qreal scale = qMin(width() / logicalWidth,
                             height() / logicalHeight);
    if (scale <= 0.0) return;
    p.translate((width() - logicalWidth * scale) / 2.0,
                (height() - logicalHeight * scale) / 2.0);
    p.scale(scale, scale);

    const QPen border(t.borderStrong, 2.0);
    const auto drawArrow = [&](const QPointF from, const QPointF to) {
        p.setPen(QPen(t.accent, 3.0, Qt::SolidLine,
                      Qt::SquareCap, Qt::MiterJoin));
        p.drawLine(from, to);
        const bool vertical = qAbs(to.y() - from.y()) >
                              qAbs(to.x() - from.x());
        if (vertical) {
            p.drawLine(to, to + QPointF(-6, -7));
            p.drawLine(to, to + QPointF(6, -7));
        } else {
            p.drawLine(to, to + QPointF(-7, -6));
            p.drawLine(to, to + QPointF(-7, 6));
        }
    };
    const auto drawFile = [&](const QRectF &rect, const bool verified) {
        QPainterPath file;
        const qreal fold = 15.0;
        file.moveTo(rect.left(), rect.top());
        file.lineTo(rect.right() - fold, rect.top());
        file.lineTo(rect.right(), rect.top() + fold);
        file.lineTo(rect.right(), rect.bottom());
        file.lineTo(rect.left(), rect.bottom());
        file.closeSubpath();
        file.moveTo(rect.right() - fold, rect.top());
        file.lineTo(rect.right() - fold, rect.top() + fold);
        file.lineTo(rect.right(), rect.top() + fold);
        p.setPen(border);
        p.setBrush(t.surfaceRaised);
        p.drawPath(file);
        p.drawLine(QPointF(rect.left() + 13, rect.top() + 31),
                   QPointF(rect.right() - 13, rect.top() + 31));
        p.drawLine(QPointF(rect.left() + 13, rect.top() + 43),
                   QPointF(rect.right() - 22, rect.top() + 43));
        if (verified) {
            p.setPen(QPen(t.success, 4.0, Qt::SolidLine,
                          Qt::RoundCap, Qt::RoundJoin));
            p.drawLine(QPointF(rect.left() + 17, rect.bottom() - 18),
                       QPointF(rect.left() + 25, rect.bottom() - 10));
            p.drawLine(QPointF(rect.left() + 25, rect.bottom() - 10),
                       QPointF(rect.left() + 42, rect.bottom() - 29));
        }
    };
    const auto drawVideo = [&](const QRectF &rect, const int seed) {
        p.setPen(border);
        p.setBrush(t.surfaceRaised);
        p.drawRect(rect);
        const qreal cell = 10.0;
        for (int row = 0; row < 3; ++row)
            for (int column = 0; column < 4; ++column) {
                const QRectF block(rect.left() + 9 + column * cell,
                                   rect.top() + 9 + row * cell, 7, 7);
                p.fillRect(block, (row + column + seed) % 4 == 0
                                      ? t.accent : t.borderStrong);
            }
        p.fillRect(QRectF(rect.left() + 9, rect.bottom() - 14,
                          rect.width() - 18, 4), t.success);
    };
    const auto drawPlaylist = [&](const QRectF &rect) {
        p.setPen(border);
        p.setBrush(t.surfaceRaised);
        p.drawRect(rect);
        for (int row = 0; row < 3; ++row) {
            p.fillRect(QRectF(rect.left() + 13, rect.top() + 14 + row * 19,
                              12, 10), row == 1 ? t.accent : t.borderStrong);
            p.drawLine(QPointF(rect.left() + 34,
                               rect.top() + 19 + row * 19),
                       QPointF(rect.right() - 13,
                               rect.top() + 19 + row * 19));
        }
    };

    if (mode_ == Mode::Create) {
        drawFile({78, 55, 76, 82}, false);
        drawArrow({182, 96}, {250, 96});
        for (int i = 0; i < 3; ++i)
            drawVideo({292 + i * 104.0, 58, 82, 76}, i);
    } else if (mode_ == Mode::Store) {
        for (int i = 0; i < 3; ++i)
            drawVideo({84 + i * 100.0, 58, 78, 74}, i);
        drawArrow({408, 96}, {474, 96});
        drawPlaylist({518, 53, 92, 84});
    } else {
        drawPlaylist({54, 53, 92, 84});
        drawArrow({174, 96}, {230, 96});
        for (int i = 0; i < 3; ++i)
            drawVideo({262 + i * 74.0, 67, 60, 58}, i);
        drawArrow({500, 96}, {548, 96});
        drawFile({574, 55, 66, 82}, true);
    }
}

VidStoreXOnboardingProgress::VidStoreXOnboardingProgress(QWidget *parent)
    : QWidget(parent) {
    setObjectName(QStringLiteral("onboardingProgress"));
    setFocusPolicy(Qt::NoFocus);
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAccessibleName(QStringLiteral("Getting started progress"));
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    setCurrentPage(0);
}

void VidStoreXOnboardingProgress::setCurrentPage(const int page) {
    currentPage_ = qBound(0, page, 2);
    setAccessibleDescription(QStringLiteral("Page %1 of 3")
                                 .arg(currentPage_ + 1));
    update();
}

QSize VidStoreXOnboardingProgress::sizeHint() const { return {104, 12}; }

void VidStoreXOnboardingProgress::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, false);
    const auto t = vidstorex_ui::themeTokens(palette());
    constexpr int blockWidth = 28;
    constexpr int gap = 8;
    const int top = qMax(0, (height() - 8) / 2);
    for (int index = 0; index < 3; ++index) {
        const QColor color = index < currentPage_
            ? t.success : index == currentPage_
            ? t.accent : t.borderStrong;
        p.fillRect(QRect(index * (blockWidth + gap), top, blockWidth, 8),
                   color);
    }
}

VidStoreXRecentEntry::VidStoreXRecentEntry(
    const QString &title, const QString &metadata, const QString &status,
    const char *statusState, QWidget *parent)
    : QFrame(parent) {
    setAttribute(Qt::WA_TransparentForMouseEvents);
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(12, 8, 10, 8);
    layout->setSpacing(12);
    auto *textLayout = new QVBoxLayout();
    textLayout->setSpacing(3);
    title_ = new QLabel(title);
    title_->setObjectName(QStringLiteral("videoSetRecentEntryTitle"));
    title_->setProperty("recentTitle", true);
    title_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    title_->setToolTip(title);
    metadata_ = new QLabel(metadata);
    metadata_->setObjectName(QStringLiteral("videoSetRecentEntryMetadata"));
    metadata_->setProperty("recentMeta", true);
    metadata_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    metadata_->setToolTip(metadata);
    textLayout->addWidget(title_);
    textLayout->addWidget(metadata_);
    status_ = new QLabel(status);
    status_->setObjectName(QStringLiteral("videoSetRecentEntryStatus"));
    status_->setProperty("vsxState", statusState);
    status_->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    status_->setWordWrap(false);
    layout->addLayout(textLayout, 1);
    layout->addWidget(status_, 0, Qt::AlignVCenter);
}

QSize VidStoreXRecentEntry::sizeHint() const {
    const QSize natural = QFrame::sizeHint();
    return {natural.width(), qMax(58, natural.height())};
}

VidStoreXStepper::VidStoreXStepper(QWidget *parent) : QWidget(parent) {
    setObjectName(QStringLiteral("videoSetAssistantStepper"));
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setAccessibleName(QStringLiteral("Video Set workflow steps"));
}

void VidStoreXStepper::setSteps(const QStringList &steps, const int active,
                                const int error) {
    steps_ = steps;
    active_ = qBound(0, active, qMax(0, steps.size() - 1));
    error_ = error;
    setAccessibleDescription(steps.isEmpty() ? QString() :
        QStringLiteral("%1: %2").arg(active_ + 1).arg(steps.value(active_)));
    update();
}

QSize VidStoreXStepper::sizeHint() const { return {720, 58}; }
QSize VidStoreXStepper::minimumSizeHint() const { return {460, 54}; }

void VidStoreXStepper::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    const auto t = vidstorex_ui::themeTokens(palette());
    if (steps_.isEmpty() || width() <= 0 || height() <= 0) return;
    const int count = steps_.size();
    const int segment = width() / count;
    const int box = qMin(26, height() - 24);
    const int boxY = 3;
    QFont labelFont = font();
    labelFont.setPointSizeF(qMax(8.0, labelFont.pointSizeF() - 1.0));
    for (int i = 0; i < count; ++i) {
        const int center = segment * i + segment / 2;
        const QRect numberRect(center - box / 2, boxY, box, box);
        const bool complete = i < active_;
        const bool current = i == active_;
        const bool failed = i == error_;
        QColor fill = t.surfaceRaised;
        QColor stroke = t.borderStrong;
        QColor text = t.textMuted;
        if (failed) { fill = t.error; stroke = t.error; text = t.onAccent; }
        else if (complete) { fill = t.success; stroke = t.success; text = t.onAccent; }
        else if (current) { fill = t.accent; stroke = t.accent; text = t.onAccent; }
        p.setPen(QPen(stroke, current ? 2 : 1));
        p.setBrush(fill);
        p.drawRoundedRect(numberRect, 5, 5);
        p.setPen(text);
        QFont numberFont = font();
        numberFont.setBold(true);
        p.setFont(numberFont);
        if (complete) {
            QPen check(text, 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
            p.setPen(check);
            p.drawLine(numberRect.left() + 6, numberRect.center().y(),
                       numberRect.left() + 11, numberRect.bottom() - 6);
            p.drawLine(numberRect.left() + 11, numberRect.bottom() - 6,
                       numberRect.right() - 5, numberRect.top() + 6);
        } else {
            p.drawText(numberRect, Qt::AlignCenter, QString::number(i + 1));
        }
        if (i + 1 < count) {
            p.setPen(QPen(i < active_ ? t.success : t.border, 2));
            p.drawLine(numberRect.right() + 5, numberRect.center().y(),
                       center + segment - box / 2 - 5, numberRect.center().y());
        }
        p.setFont(labelFont);
        p.setPen(current ? t.textPrimary : t.textSecondary);
        p.drawText(QRect(segment * i + 2, boxY + box + 5,
                         segment - 4, height() - boxY - box - 5),
                   Qt::AlignHCenter | Qt::AlignTop | Qt::TextSingleLine,
                   p.fontMetrics().elidedText(steps_[i], Qt::ElideRight,
                                              segment - 8));
    }
}
