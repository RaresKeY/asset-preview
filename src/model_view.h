#pragma once
#include "viewer.h"
#include <QOpenGLWindow>
#include <f3d/engine.h>

class ModelView final : public QOpenGLWindow, public PreviewView {
public:
    explicit ModelView(QJsonObject config);
    ~ModelView() override;
    bool reload(const QJsonObject&, QString&) override;
    void fit() override;
    void settings(const QJsonObject&) override;
    QJsonObject metrics() const override;
    std::function<void(bool, QString)> loaded;
    QImage snapshot() override;
protected:
    void initializeGL() override;
    void resizeGL(int, int) override;
    void paintGL() override;
    void wheelEvent(QWheelEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
private:
    bool load(QString& error);
    void applyOptions(f3d::engine&);
    void cleanup();
    QJsonObject config;
    std::unique_ptr<f3d::engine> engine;
    QString renderer;
    QPointF last;
    bool initialized = false;
    int loads = 0, renders = 0;
};
