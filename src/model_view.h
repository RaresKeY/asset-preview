#pragma once
#include "viewer.h"
#include <QOpenGLWindow>
#include <f3d/engine.h>
#include <optional>

class ModelView final : public QOpenGLWindow, public PreviewView {
public:
    explicit ModelView(QJsonObject config);
    ~ModelView() override;
    bool reload(const QJsonObject&, QString&) override;
    void fit() override;
    void resetView() override;
    QJsonObject cameraState() const override;
    QJsonObject cameraHome() const override;
    void setCameraState(const QJsonObject&) override;
    void cameraView(const QString&) override;
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
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
private:
    bool load(QString& error);
    void applyOptions(f3d::engine&);
    void cleanup();
    f3d::vector3_t worldUp() const;
    void orbit(double yaw, double pitch);
    double parallelSpan() const;
    void restoreCamera(const QJsonObject&);
    void rememberCamera(bool persist=true);
    f3d::camera_state_t defaultCamera;
    f3d::camera_state_t referenceCamera;
    QJsonObject config;
    std::unique_ptr<f3d::engine> engine;
    QString renderer;
    QPointF last;
    bool initialized = false;
    int loads = 0, renders = 0;
    std::optional<qint64> triangles;
};
