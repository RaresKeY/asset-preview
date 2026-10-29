#pragma once
#include <QJsonObject>
#include <QImage>
#include <QWidget>
#include <functional>
#include <memory>
#include <atomic>
using InspectionCallback = std::function<void(const QJsonObject&, bool)>;
namespace preview { struct AudioWaveform; class CacheStore; }

class PreviewView {
public:
    virtual ~PreviewView() = default;
    preview::CacheStore* cache=nullptr;
    bool reuseCachedPayload=true;
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
    std::function<void()> playbackChanged;
    std::function<void()> loadingChanged;
    std::function<void(QJsonObject)> playbackSettingsRequested;
    virtual void settings(const QJsonObject& config) = 0;
    virtual QJsonObject metrics() const = 0;
    virtual std::shared_ptr<const preview::AudioWaveform> audioWaveform() const { return {}; }
    virtual QImage snapshot() { return {}; }
    virtual QImage thumbnailSnapshot(QSize bound) { return snapshot().scaled(bound,Qt::KeepAspectRatio,Qt::SmoothTransformation); }
    virtual bool loading() const { return false; }
    virtual bool seek(double, QString& error) { error="This preview has no seekable playback"; return false; }
    virtual bool stepAnimation(int,int,QString& error) { error="This preview has no frame stepping"; return false; }
    virtual QJsonObject inspect(const QJsonObject&, QString& error) { error="This view does not support component inspection"; return {}; }
};

class ImageView final : public QWidget, public PreviewView {
public:
    explicit ImageView(QWidget* parent = nullptr);
    ~ImageView() override;
    bool reload(const QJsonObject&, QString&) override;
    void fit() override;
    void settings(const QJsonObject&) override;
    QJsonObject metrics() const override;
    QJsonObject inspectionState() const override;
    QImage snapshot() override { return grab().toImage(); }
    QImage pixels() const { return image; }
    QImage thumbnail() const { return previewImage; }
    bool loading() const override { return pending; }
    std::function<void(bool,QString)> loaded;
protected:
    void paintEvent(QPaintEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
private:
    QImage image;
    QImage previewImage;
    QSize originalSize;
    QString background = "checker";
    bool nearest = false;
    double zoom = 1;
    QPointF pan, last;
    int loads = 0, renders = 0;
    bool restored=false;
    bool cacheHit=false;
    QString cachedPath, cachedKey;
    bool pending=false;
    std::shared_ptr<std::atomic_bool> cancelled;
    QJsonObject loadTiming;
    void publish(QImage,QSize,const QString&,const QString&);
    void remember(bool persist=true);
};

struct ModelHandle { QWidget* surface; PreviewView* view; };
using LoadedCallback = std::function<void(bool, QString)>;
using SettingsCallback = std::function<void(QJsonObject)>;
ModelHandle createModel(QJsonObject config, QWidget* parent, LoadedCallback loaded, InspectionCallback changed);
ModelHandle createMedia(QJsonObject config, QWidget* parent, LoadedCallback loaded, SettingsCallback changed);
using InspectionEventCallback = std::function<void(const QJsonObject&)>;
ModelHandle createInspection(QJsonObject config, QWidget* parent, LoadedCallback loaded, InspectionCallback changed, InspectionEventCallback event);
