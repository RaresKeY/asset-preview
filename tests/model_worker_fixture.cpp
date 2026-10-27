#include "model_worker.h"
#include "backend_abi_config.h"
#include <QThread>
#include <atomic>

namespace {
std::atomic_int entered=0,allowed=0;
class Worker final : public ModelWorker {
    int stage=0,loads=0;
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
    void fit() override {}
    void settings(const QJsonObject&) override {}
    QJsonObject metrics() const override { return {{"engine",true},{"loads",loads}}; }
    QImage snapshot() override { QImage image(16,16,QImage::Format_RGBA8888); image.fill(QColor(stage,0,0)); return image; }
    void viewport(QSize,double) override {}
    void exposed(bool) override {}
    void scroll(double) override {}
    void drag(QPointF,bool) override {}
};
}
extern "C" Q_DECL_EXPORT const char* asset_preview_backend_abi() { return ASSET_PREVIEW_BACKEND_ABI; }
extern "C" Q_DECL_EXPORT ModelWorker* asset_preview_create_model_worker(QJsonObject) { return new Worker; }
extern "C" Q_DECL_EXPORT int fixtureEntered() { return entered; }
extern "C" Q_DECL_EXPORT void fixtureAllow(int stage) { allowed=stage; }
