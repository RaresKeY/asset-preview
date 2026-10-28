#pragma once
#include "../viewer.h"
#include <QOpenGLWindow>
#include <memory>

class InspectionVtkView final : public QOpenGLWindow, public PreviewView {
public:
    explicit InspectionVtkView(QJsonObject config);
    ~InspectionVtkView() override;
    bool reload(const QJsonObject&,QString&) override;
    void settings(const QJsonObject&) override;
    void fit() override;
    void resetView() override;
    QJsonObject cameraState() const override;
    QJsonObject cameraHome() const override;
    void setCameraState(const QJsonObject&) override;
    void cameraView(const QString&) override;
    QJsonObject metrics() const override;
    QJsonObject inspect(const QJsonObject&,QString&) override;
    QImage snapshot() override;
    LoadedCallback loaded;
    InspectionEventCallback inspectionEvent;
protected:
    void initializeGL() override;
    void resizeGL(int,int) override;
    void paintGL() override;
    void wheelEvent(QWheelEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
private:
    struct Scene;
    bool load(QString&);
    void cleanup();
    void paintFrame(GLuint framebuffer);
    void options();
    void restoreCamera(const QJsonObject&);
    void rememberCamera(bool persist=true);
    void orbit(double,double);
    QJsonObject state() const;
    void changed();
    void applyVisibility();
    void applyCuts();
    void applySelection();
    QJsonObject pick(double,double,QString&);
    bool applyView(const QJsonObject&,QString&);
    QJsonObject config, retainedView, selectedTarget;
    std::unique_ptr<Scene> scene;
    QString rendererName, selected, staleSelection;
    QPointF last, pressed;
    bool initialized=false, dragged=false, doubleClick=false;
    int loads=0,renders=0,picks=0;
};
