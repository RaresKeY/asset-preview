#include "animation_controls.h"
#include "animation_time.h"
#include <QSpinBox>
#include "animation_timeline.h"
#include <QStyle>
#include <QComboBox>
#include <QAbstractItemView>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QResizeEvent>
#include <QSignalBlocker>
#include <QToolButton>
#include <cmath>

namespace {
QString seconds(double value) { return QString::number(value,'f',2)+" s"; }
QIcon icon(QWidget* widget,QStyle::StandardPixmap glyph) {
    QIcon result;
    for (const auto mode:{QIcon::Normal,QIcon::Disabled}) {
        auto image=widget->style()->standardIcon(glyph).pixmap(QSize(16,16),widget->devicePixelRatioF());
        QPainter painter(&image); painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
        painter.fillRect(image.rect(),mode==QIcon::Disabled?QColor("#63707d"):QColor("#eff0f1")); painter.end();
        result.addPixmap(image,mode);
    }
    return result;
}
}
AnimationControls::AnimationControls(QWidget* parent) : QFrame(parent) {
    setObjectName("viewOverlay");
    setAccessibleName("3D animation playback controls");
    row=new QGridLayout(this); row->setContentsMargins(5,3,5,3); row->setSpacing(3);
    auto button=[this](QStyle::StandardPixmap glyph,const QString& name) {
        auto* result=new QToolButton(this); result->setFixedSize(28,28);
        result->setIcon(icon(this,glyph)); result->setAccessibleName(name); result->setToolTip(name);
        result->setFocusPolicy(Qt::StrongFocus); return result;
    };
    play=button(QStyle::SP_MediaPlay,"Play animation (Space)");
    setFocusPolicy(Qt::StrongFocus); setFocusProxy(play);
    restart=button(QStyle::SP_MediaSkipBackward,"Restart animation (Home)");
    loop=button(QStyle::SP_BrowserReload,"Loop animation"); loop->setCheckable(true);
    previous=button(QStyle::SP_MediaSeekBackward,"Previous frame (,)");
    next=button(QStyle::SP_MediaSeekForward,"Next frame (.)");
    stepped=new QToolButton(this); stepped->setText("Stepped"); stepped->setCheckable(true);
    stepped->setFocusPolicy(Qt::StrongFocus); stepped->setAccessibleName("Stepped playback");
    stepped->setToolTip("Hold each pose at the chosen frame rate");
    fps=new QSpinBox(this); fps->setRange(1,120); fps->setValue(12); fps->setSuffix(" fps");
    fps->setKeyboardTracking(false); fps->setAccessibleName("Animation frame rate");
    fps->setToolTip("Pose sampling and frame step rate");
    clips=new QComboBox(this); clips->setMinimumWidth(24); clips->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Fixed);
    clips->setMinimumHeight(28);
    clips->setAccessibleName("Animation clip"); clips->setToolTip("Animation clip");
    speed=new QDoubleSpinBox(this); speed->setRange(.1,4); speed->setSingleStep(.25); speed->setDecimals(2); speed->setSuffix("×");
    speed->setMinimumWidth(78); speed->setSizePolicy(QSizePolicy::Fixed,QSizePolicy::Fixed);
    speed->setKeyboardTracking(false); speed->setAccessibleName("Animation speed");
    speed->setToolTip("Animation speed");
    timeline=new preview::AnimationTimeline(this); timeline->setFixedHeight(34);
    timeline->setAccessibleName("Animation timeline"); timeline->setToolTip("Click or drag to a frame; arrow keys step frames");
    time=new QLabel(this); time->setMinimumWidth(0); time->setAccessibleName("Animation time");
    row->addWidget(timeline,0,0,1,11);
    row->addWidget(play,1,0); row->addWidget(restart,1,1);
    row->addWidget(clips,1,2); row->setColumnStretch(2,1);
    row->addWidget(speed,1,3); row->addWidget(loop,1,4);
    row->addWidget(previous,1,5); row->addWidget(next,1,6);
    row->addWidget(stepped,1,7); row->addWidget(fps,1,8);
    row->addWidget(time,1,10);
    connect(previous,&QToolButton::clicked,this,[this] { step(-1); });
    connect(next,&QToolButton::clicked,this,[this] { step(1); });
    connect(stepped,&QToolButton::clicked,this,[this](bool value) {
        if (settingsRequested) settingsRequested({{"animation_stepped",value}});
    });
    connect(fps,&QSpinBox::valueChanged,this,[this](int value) {
        configuredFps=value;
        if (settingsRequested) settingsRequested({{"animation_fps",value}});
    });
    connect(fps,&QSpinBox::editingFinished,this,[this] {
        const QSignalBlocker blocker(fps); fps->setValue(configuredFps);
    });
    connect(play,&QToolButton::clicked,this,[this] { if (settingsRequested) settingsRequested({{"paused",!paused}}); });
    connect(restart,&QToolButton::clicked,this,[this] { if (seekRequested) seekRequested(0); });
    connect(loop,&QToolButton::clicked,this,[this](bool value) { if (settingsRequested) settingsRequested({{"loop",value}}); });
    connect(clips,&QComboBox::activated,this,[this](int index) { if (settingsRequested) settingsRequested({{"animation",index}}); });
    connect(speed,&QDoubleSpinBox::valueChanged,this,[this](double value) {
        configuredSpeed=value;
        if (settingsRequested) settingsRequested({{"animation_speed",value}});
    });
    connect(speed,&QDoubleSpinBox::editingFinished,this,[this] {
        const QSignalBlocker blocker(speed); speed->setValue(configuredSpeed);
    });
    connect(timeline,&QSlider::sliderMoved,this,[this] { updateTime(); });
    timeline->seekRequested=[this](double seconds) {
        position=seconds; updateTime(); if (seekRequested) seekRequested(position);
    };
    QWidget::setTabOrder(play,restart); QWidget::setTabOrder(restart,clips); QWidget::setTabOrder(clips,speed);
    QWidget::setTabOrder(speed,loop); QWidget::setTabOrder(loop,previous);
    QWidget::setTabOrder(previous,next); QWidget::setTabOrder(next,stepped);
    QWidget::setTabOrder(stepped,fps); QWidget::setTabOrder(fps,timeline);
    // Match the existing dark preview overlays even under a light desktop theme.
    const QString fields="QComboBox, QAbstractSpinBox {color:#eff0f1;background:#31363b;border:1px solid #454b50;border-radius:3px;padding:2px;}"
        "QComboBox:focus, QAbstractSpinBox:focus {border-color:#3daee9;}"
        "QComboBox:disabled, QAbstractSpinBox:disabled {color:#63707d;}"
        "QComboBox QAbstractItemView {color:#eff0f1;background:#232629;selection-background-color:#28536b;}";
    setStyleSheet("QFrame#viewOverlay {background:#232629;border:1px solid #454b50;border-radius:3px;}"
        "QLabel {color:#bdc3c7;background:transparent;border:0;}"
        "QToolButton {color:#eff0f1;background:transparent;border:1px solid transparent;border-radius:3px;padding:3px;}"
        "QToolButton:hover {background:#31363b;border-color:#5d6870;}"
        "QToolButton:checked {background:#28536b;border-color:#3daee9;}"
        "QToolButton:pressed {background:#1b465e;}"
        "QToolButton:focus {border-color:#3daee9;}");
    clips->setStyleSheet(fields); speed->setStyleSheet(fields); fps->setStyleSheet(fields);
    setPlayback({},{});
}
void AnimationControls::setPlayback(const QJsonObject& settings,const QJsonObject& metrics) {
    const QSignalBlocker clipBlock(clips),speedBlock(speed),loopBlock(loop),sliderBlock(timeline),steppedBlock(stepped),fpsBlock(fps);
    const auto names=metrics["animations"].toArray();
    QStringList text; for (const auto& name:names) text.append(name.toString());
    QStringList existing; for (int i=0;i<clips->count();++i) existing.append(clips->itemText(i));
    const int index=metrics["animation"].toInt();
    const int generation=metrics["loads"].toInt();
    if (text!=existing || clipIndex!=index || loadedGeneration!=generation) timeline->cancelDrag();
    clipIndex=index; loadedGeneration=generation;
    if (text!=existing) { clips->clear(); clips->addItems(text); }
    if (!clips->view()->isVisible()) clips->setCurrentIndex(metrics["animation"].toInt());
    if (clips->currentIndex()>=0) clips->setToolTip(clips->currentText());
    paused=metrics["paused"].toBool(true);
    const auto name=paused?QString("Play animation"):QString("Pause animation");
    if (play->accessibleName()!=name) {
        play->setIcon(icon(this,paused?QStyle::SP_MediaPlay:QStyle::SP_MediaPause));
        play->setAccessibleName(name); play->setToolTip(name+" (Space)");
    }
    configuredSpeed=settings["animation_speed"].toDouble(1);
    // Playback notifications must preserve an unfinished edit. Merely holding
    // focus does not prevent external settings from reaching a paused control.
    if (!speed->hasFocus() || !speed->findChild<QLineEdit*>()->isModified()) speed->setValue(configuredSpeed);
    loop->setChecked(settings["loop"].toBool(true));
    stepped->setChecked(settings["animation_stepped"].toBool());
    configuredFps=std::clamp(settings["animation_fps"].toInt(12),1,120);
    if (!fps->hasFocus() || !fps->findChild<QLineEdit*>()->isModified()) fps->setValue(configuredFps);
    duration=metrics["duration"].toDouble(); position=metrics["position"].toDouble();
    if (!std::isfinite(duration) || duration<0) duration=0;
    if (!std::isfinite(position) || position<0) position=0;
    const bool available=!names.isEmpty(),seekable=available && duration>0;
    play->setEnabled(seekable); restart->setEnabled(seekable); timeline->setEnabled(seekable);
    clips->setEnabled(available); speed->setEnabled(seekable); loop->setEnabled(seekable);
    previous->setEnabled(seekable); next->setEnabled(seekable); stepped->setEnabled(seekable); fps->setEnabled(seekable);
    if (!seekable) timeline->setSliderDown(false);
    timeline->setClip(duration,configuredFps);
    if (!timeline->isSliderDown()) timeline->setPosition(position);
    updateTime();
}
void AnimationControls::step(int direction) {
    if(stepRequested) { stepRequested(direction); return; }
    // Pausing synchronously publishes the current elapsed position, which can
    // be newer than the last low-rate pose notification.
    if (settingsRequested) settingsRequested({{"paused",true}});
    const auto target=preview::animationStep(position,duration,configuredFps,direction);
    if (seekRequested) seekRequested(target);
}
void AnimationControls::updateTime() {
    const double shown=timeline->shownPosition();
    const auto text=QString("Frame %1 / %2").arg(shown>=duration?std::ceil(duration*configuredFps):std::floor(preview::animationFrame(shown,configuredFps)),0,'f',0).arg(std::ceil(duration*configuredFps),0,'f',0);
    time->setText(text); time->setToolTip(seconds(shown)+" / "+seconds(duration));
    timeline->setAccessibleDescription(text+"; "+time->toolTip());
    place();
}
void AnimationControls::place() {
    timeline->setFixedHeight(condensed?10:std::max(34,fontMetrics().height()+16));
    // Reserve a readable clip name, then reveal secondary controls only when
    // their actual font-dependent size fits. Hidden actions remain in Options.
    int remaining=width()-row->contentsMargins().left()-row->contentsMargins().right()
        -play->width()-std::min(120,std::max(24,width()/4))-row->spacing();
    auto reveal=[&](QWidget* widget,bool allowed=true) {
        const int needed=widget->sizeHint().width()+row->spacing();
        const bool visible=!condensed && allowed && remaining>=needed;
        widget->setVisible(visible);
        if (visible) remaining-=needed;
    };
    reveal(restart); reveal(speed); reveal(loop);
    const int steps=previous->sizeHint().width()+next->sizeHint().width()+2*row->spacing();
    const bool showSteps=!condensed && remaining>=steps;
    previous->setVisible(showSteps); next->setVisible(showSteps);
    if (showSteps) remaining-=steps;
    const int sampling=stepped->sizeHint().width()+fps->sizeHint().width()+2*row->spacing();
    const bool showSampling=!condensed && remaining>=sampling;
    stepped->setVisible(showSampling); fps->setVisible(showSampling);
    if (showSampling) remaining-=sampling;
    reveal(time);
    row->activate();
}
void AnimationControls::setCondensed(bool value) {
    if (condensed==value) return;
    condensed=value; timeline->setFixedHeight(value?10:std::max(34,fontMetrics().height()+16));
    row->setContentsMargins(value?3:5,value?1:3,value?3:5,value?1:3); row->setSpacing(value?2:3);
    place(); updateGeometry();
}
void AnimationControls::focusPlayback(bool backwards) {
    QList<QWidget*> widgets{play,restart,clips,speed,loop,previous,next,stepped,fps,timeline};
    if (backwards) std::reverse(widgets.begin(),widgets.end());
    for (auto* widget:widgets) if (widget->isVisible() && widget->isEnabled()) {
        widget->setFocus(backwards?Qt::BacktabFocusReason:Qt::TabFocusReason); return;
    }
}
void AnimationControls::resizeEvent(QResizeEvent* event) { QFrame::resizeEvent(event); place(); }
QJsonObject AnimationControls::describe(QWidget* window) const {
    QJsonObject result;
    for (const auto& [name,widget]:{std::pair{"play",static_cast<QWidget*>(play)},{"restart",static_cast<QWidget*>(restart)},
            {"loop",static_cast<QWidget*>(loop)},{"clip",static_cast<QWidget*>(clips)},{"speed",static_cast<QWidget*>(speed)},
            {"previous",static_cast<QWidget*>(previous)},{"next",static_cast<QWidget*>(next)},
            {"stepped",static_cast<QWidget*>(stepped)},{"fps",static_cast<QWidget*>(fps)},
            {"timeline",static_cast<QWidget*>(timeline)},{"time",static_cast<QWidget*>(time)}}) {
        const auto p=widget->mapTo(window,QPoint());
        QJsonObject item{{"x",p.x()},{"y",p.y()},{"width",widget->width()},{"height",widget->height()},
            {"visible",widget->isVisible()},{"enabled",widget->isEnabled()},{"focused",widget->hasFocus()}};
        if (widget==clips) { item["index"]=clips->currentIndex(); item["count"]=clips->count(); item["text"]=clips->currentText(); }
        if (widget==timeline) { item["dragging"]=timeline->isSliderDown(); item["position"]=timeline->shownPosition(); }
        if (widget==speed) item["value"]=speed->value();
        if (widget==fps) item["value"]=fps->value();
        if (widget==time) item["text"]=time->text();
        if (auto* button=qobject_cast<QToolButton*>(widget)) { item["checked"]=button->isChecked(); item["name"]=button->accessibleName(); }
        result[name]=item;
    }
    return result;
}
