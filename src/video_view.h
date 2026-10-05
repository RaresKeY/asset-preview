#pragma once
#include "viewer.h"
#include <QOpenGLWindow>
#include <atomic>

class VideoView final : public QOpenGLWindow, public PreviewView {
public:
    VideoView(QJsonObject, LoadedCallback, SettingsCallback);
    ~VideoView() override;
    bool reload(const QJsonObject&, QString&) override;
    void fit() override;
    void settings(const QJsonObject&) override;
    QJsonObject metrics() const override;
    QImage snapshot() override;
    bool loading() const override;
    bool seek(double, QString&) override;
    std::function<void()> focusSurface;
protected:
    void initializeGL() override;
    void paintGL() override;
    bool eventFilter(QObject*, QEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
private:
    struct Player;
    static void wakeup(void*);
    static void redraw(void*);
    void drain();
    void apply(Player&);
    void cleanup();
    void failed(const QString&);
    void change(QJsonObject);
    QJsonObject config;
    LoadedCallback loaded;
    SettingsCallback changed;
    std::unique_ptr<Player> current, candidate;
    std::atomic_bool queued=false, frameQueued=false, closing=false;
    QString renderer;
    bool initialized=false;
    int loads=0, renders=0, generation=0;
};
