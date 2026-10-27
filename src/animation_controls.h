#pragma once
#include <QFrame>
#include <QJsonObject>
#include <functional>

class QComboBox;
class QDoubleSpinBox;
class QGridLayout;
class QLabel;
namespace preview { class AnimationTimeline; }
class QSpinBox;
class QToolButton;

class AnimationControls final : public QFrame {
public:
    explicit AnimationControls(QWidget* parent);
    void setPlayback(const QJsonObject& settings,const QJsonObject& metrics);
    void setCondensed(bool value);
    void focusPlayback(bool backwards=false);
    QJsonObject describe(QWidget* window) const;
    std::function<void(QJsonObject)> settingsRequested;
    std::function<void(double)> seekRequested;
    std::function<void(int)> stepRequested;
protected:
    void resizeEvent(QResizeEvent*) override;
private:
    void place();
    void updateTime();
    void step(int direction);
    QToolButton *play,*restart,*loop,*previous,*next,*stepped;
    QComboBox* clips;
    QDoubleSpinBox* speed;
    QSpinBox* fps;
    preview::AnimationTimeline* timeline;
    QLabel* time;
    QGridLayout* row;
    double duration=0,position=0,configuredSpeed=1;
    int configuredFps=12,clipIndex=-1,loadedGeneration=0;
    bool paused=true;
    bool condensed=false;
};
