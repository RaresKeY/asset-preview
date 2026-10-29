#pragma once
#include "model_worker.h"
#include "triangle_counts.h"

#include <QElapsedTimer>
#include <QTimer>
#include <f3d/engine.h>
#include <optional>
#include <vector>

class ModelScene final : public ModelWorker {
public:
    explicit ModelScene(QJsonObject config);
    ~ModelScene() override;
    bool reload(const QJsonObject&, QString&) override;
    void fit() override;
    void resetView() override;
    QJsonObject cameraState() const override;
    QJsonObject cameraHome() const override;
    void setCameraState(const QJsonObject&) override;
    void cameraView(const QString&) override;
    void settings(const QJsonObject&) override;
    QJsonObject metrics() const override;
    QImage snapshot() override;
    bool seek(double, QString&) override;
    static QImage thumbnail(QJsonObject,QString& error,QString& renderer);
    void viewport(QSize,double) override;
    void exposed(bool) override;
    void scroll(double) override;
    void drag(QPointF,bool) override;
    void gpuContext(std::shared_ptr<ModelGpuContext>) override;
    void prepare() override;
    bool renderPending() const override { return framePending; }
    void present() override;
private:
    int width() const { return extent.width(); }
    int height() const { return extent.height(); }
    double devicePixelRatioF() const { return ratio; }
    bool isExposed() const { return visible; }
    void update(int delay=0);
    void paintGL();
    QSize extent{100,100};
    double ratio=1;
    bool visible=true,framePending=false;
    bool pendingLoadNotice=false;
    std::shared_ptr<ModelGpuContext> gpu;
    std::vector<std::shared_ptr<ModelGpuFrame>> gpuFrames;
    quint64 frameSerial=0;
    bool cameraDirty=false;
    std::shared_ptr<ModelGpuFrame> renderGpuFrame();
    bool load(QString& error, bool retainAnimation=false);
    struct Animation {
        QStringList clips;
        int index=0;
        double start=0, duration=0, position=0, sample=0;
        bool paused=true, eof=false;
    } animation;
    Animation prepareAnimation(f3d::engine&, double position=0);
    void advanceAnimation();
    void restartAnimationClock();
    void scheduleAnimation();
    void notifyPlayback();
    QTimer animationTick;
    QElapsedTimer animationClock, notificationClock;
    double animationAnchor=0, animationClockAtPose=0;
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
    bool thumbnailOnly = false;
    bool resetCameraFrame = false;
    int loads = 0, renders = 0;
    std::optional<qint64> triangles;
    preview::TriangleMetadata triangleMetadata;
    QJsonObject texturePreviewMetrics;
    QJsonObject loadTiming;
    QJsonObject gltfImport;
    QElapsedTimer loadClock;
    bool firstFramePending=false;
    QString publishedMaterialModel;
};
