#include "model_worker.h"
#include "backend_abi_config.h"
#include <QThread>
#include <QTimer>
#include <atomic>

namespace {
std::atomic_int entered=0,allowed=0,metricReads=0;
class Worker final : public ModelWorker {
    int stage=0,loads=0,renders=0,moves=0;
    bool pending=false;
    QPointF motion;
    QString trace;
    void update() {
        if(stage!=5 || pending)return;
        pending=true;
        QTimer::singleShot(0,this,[this] {
            pending=false;
            QThread::msleep(12); ++renders;
            if(frameReady)frameReady(snapshot());
        });
    }
public:
    bool reload(const QJsonObject& cfg,QString&) override {
        stage=cfg["test_stage"].toInt(); ++loads;
        // Leave the first completion in the GUI mailbox, then publish a new
        // revision's frame before its import has completed.
        if(stage==2 && frameReady)frameReady(snapshot());
        if(stage==3) {
            if(inspectionChanged)inspectionChanged({{"obsolete",true}},true);
            if(playbackSettingsRequested)playbackSettingsRequested({{"paused",true}});
        }
        entered=stage;
        while(stage<4 && allowed<stage)QThread::msleep(1);
        return true;
    }
    void fit() override { trace+='f'; update(); }
    void settings(const QJsonObject&) override {}
    bool renderPending() const override { return pending; }
    QJsonObject cameraState() const override { return {{"moves",moves},{"x",motion.x()},{"y",motion.y()},{"trace",trace}}; }
    QJsonObject metrics() const override { ++metricReads; return {{"engine",true},{"loads",loads},{"renders",renders},{"render_pending",pending},{"camera",cameraState()}}; }
    QImage snapshot() override { QImage image(16,16,QImage::Format_RGBA8888); image.fill(QColor(stage==5?moves:stage,0,0)); return image; }
    void viewport(QSize,double) override {}
    void exposed(bool) override {}
    void scroll(double) override { trace+='w'; update(); }
    void drag(QPointF delta,bool pan) override { motion+=delta; ++moves; trace+=pan?'p':'o'; update(); }
};
}
extern "C" Q_DECL_EXPORT const char* asset_preview_backend_abi() { return ASSET_PREVIEW_BACKEND_ABI; }
extern "C" Q_DECL_EXPORT ModelWorker* asset_preview_create_model_worker(QJsonObject) { return new Worker; }
extern "C" Q_DECL_EXPORT int fixtureEntered() { return entered; }
extern "C" Q_DECL_EXPORT void fixtureAllow(int stage) { allowed=stage; }
extern "C" Q_DECL_EXPORT int fixtureMetricReads() { return metricReads; }
