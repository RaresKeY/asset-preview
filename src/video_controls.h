#pragma once
#include <QFrame>
#include <QJsonObject>
#include <functional>
#include "audio_waveform.h"

class QGridLayout;
class QLabel;
class QSlider;
class QToolButton;

class VideoControls final : public QFrame {
public:
    explicit VideoControls(QWidget* parent, bool audio=false);
    void setPlayback(const QJsonObject& settings, const QJsonObject& metrics);
    void setWaveform(std::shared_ptr<const preview::AudioWaveform>);
    void setCondensed(bool value);
    void setFullscreen(bool value);
    void focusPlayback(bool backwards=false);
    QJsonObject describe(QWidget* window) const;
    std::function<void(QJsonObject)> settingsRequested;
    std::function<void(double)> seekRequested;
    std::function<void()> fullscreenRequested;
protected:
    bool event(QEvent*) override;
    void resizeEvent(QResizeEvent*) override;
private:
    void place();
    void shape();
    void updateTime();
    QToolButton *play=nullptr, *restart=nullptr, *mute=nullptr, *loop=nullptr;
    QToolButton* fullscreen=nullptr;
    QSlider* timeline=nullptr;
    QLabel* clock=nullptr;
    QGridLayout* controls=nullptr;
    QJsonObject settings;
    double duration=0, position=0;
    bool condensed=false, audio=false;
    QString media;
};
