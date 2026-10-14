#pragma once
#include <QJsonObject>
#include <QImage>
#include <QWidget>
#include <functional>
#include <memory>
using InspectionCallback = std::function<void(const QJsonObject&, bool)>;

class PreviewView {
public:
    virtual ~PreviewView() = default;
    virtual bool reload(const QJsonObject& config, QString& error) = 0;
    virtual void fit() = 0;
    virtual void resetView() { fit(); }
    virtual QJsonObject cameraState() const { return {}; }
    virtual QJsonObject cameraHome() const { return {}; }
    virtual void setCameraState(const QJsonObject&) {}
    virtual void cameraView(const QString&) {}
    virtual QJsonObject inspectionState() const { return cameraState(); }
    std::function<void()> activated;
    InspectionCallback inspectionChanged;
    virtual void settings(const QJsonObject& config) = 0;
    virtual QJsonObject metrics() const = 0;
    virtual QImage snapshot() { return {}; }
    virtual bool loading() const { return false; }
    virtual bool seek(double, QString& error) { error="Seeking is supported for videos only"; return false; }
};

class ImageView final : public QWidget, public PreviewView {
public:
    explicit ImageView(QWidget* parent = nullptr);
    bool reload(const QJsonObject&, QString&) override;
    void fit() override;
    void settings(const QJsonObject&) override;
    QJsonObject metrics() const override;
    QJsonObject inspectionState() const override;
    QImage snapshot() override { return grab().toImage(); }
protected:
    void paintEvent(QPaintEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
private:
    QImage image;
    QSize originalSize;
    QString background = "checker";
    bool nearest = false;
    double zoom = 1;
    QPointF pan, last;
    int loads = 0, renders = 0;
    bool restored=false;
    void remember(bool persist=true);
};

struct ModelHandle { QWidget* surface; PreviewView* view; };
using LoadedCallback = std::function<void(bool, QString)>;
using SettingsCallback = std::function<void(QJsonObject)>;
ModelHandle createModel(QJsonObject config, QWidget* parent, LoadedCallback loaded, InspectionCallback changed);
ModelHandle createVideo(QJsonObject config, QWidget* parent, LoadedCallback loaded, SettingsCallback changed);
