#include "fullscreen_notice.h"
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QResizeEvent>
#include <QStyle>
#include <QToolButton>
#include <algorithm>

FullscreenNotice::FullscreenNotice(QWidget* parent) : QFrame(parent), slide(this,"pos") {
    setObjectName("fullscreenNotice"); setAttribute(Qt::WA_NativeWindow);
    setAccessibleName("Full screen. Press Esc or F11 to exit.");
    setStyleSheet(
        "QFrame#fullscreenNotice {background:#293943;border:1px solid #e8edf3;border-radius:10px;}"
        "QLabel {color:#eff0f1;background:transparent;border:none;}"
        "QToolButton {color:#eff0f1;background:#20262e;border:1px solid #63707d;border-radius:4px;padding:8px 12px;}"
        "QToolButton:hover {background:#394453;} QToolButton:pressed {background:#485566;}"
        "QToolButton:focus {border-color:#82d9c8;}");
    auto* row=new QHBoxLayout(this); row->setContentsMargins(18,12,18,12); row->setSpacing(16);
    auto* label=new QLabel("Full screen",this);
    auto font=label->font(); font.setBold(true); label->setFont(font);
    auto* exit=new QToolButton(this); exit->setText("Exit full screen (Esc)");
    exit->setToolTip("Exit full screen (Esc or F11)"); exit->setFocusPolicy(Qt::StrongFocus);
    row->addWidget(label); row->addWidget(exit);
    connect(exit,&QToolButton::clicked,this,[this] { if (exitRequested) exitRequested(); });
    delay.setSingleShot(true); delay.setInterval(3000);
    slide.setEasingCurve(QEasingCurve::InCubic);
    connect(&delay,&QTimer::timeout,this,[this] {
        if (!isVisible()) return;
        // Qt styles can disable widget animation for reduced motion.
        if (style()->styleHint(QStyle::SH_Widget_Animation_Duration,nullptr,this)<=0) { dismiss(); return; }
        slide.setDuration(220); slide.setStartValue(pos()); slide.setEndValue(QPoint(x(),-height()-1)); slide.start();
    });
    connect(&slide,&QPropertyAnimation::finished,this,&QWidget::hide);
    parent->installEventFilter(this); hide();
}

void FullscreenNotice::place() {
    const auto preferred=sizeHint();
    resize(std::min(preferred.width(),std::max(1,parentWidget()->width()-24)),preferred.height());
    move((parentWidget()->width()-width())/2,12);
    layout()->activate();
}
void FullscreenNotice::announce() {
    dismiss(); place(); show(); raise(); delay.start();
}
void FullscreenNotice::dismiss() { delay.stop(); slide.stop(); hide(); }
void FullscreenNotice::resizeEvent(QResizeEvent* event) {
    QFrame::resizeEvent(event);
    // Shape the native child itself, keeping rounded corners clear above GL
    // surfaces without relying on translucent child-window composition.
    QPainterPath shape; shape.addRoundedRect(QRectF(rect()),10,10);
    setMask(QRegion(shape.toFillPolygon().toPolygon()));
}
bool FullscreenNotice::eventFilter(QObject* object,QEvent* event) {
    if (object==parentWidget()) {
        if (event->type()==QEvent::Hide) dismiss();
        else if (event->type()==QEvent::Resize && isVisible()) {
            if (slide.state()==QAbstractAnimation::Running) dismiss();
            else place();
        }
    }
    return QFrame::eventFilter(object,event);
}
void FullscreenNotice::compose(QPainter& painter,QWidget* window) {
    if (!isVisible()) return;
    const auto origin=mapTo(window,QPoint());
    painter.save();
    painter.setClipRegion(mask().translated(origin),Qt::IntersectClip);
    painter.drawPixmap(origin,grab()); painter.restore();
}
QJsonObject FullscreenNotice::describe(QWidget* window) const {
    const auto p=mapTo(window,QPoint());
    return {{"visible",isVisible()},{"sliding",slide.state()==QAbstractAnimation::Running},
        {"x",p.x()},{"y",p.y()},{"width",width()},{"height",height()}};
}
