#include "model_worker.h"
#include "backend_abi_config.h"
#include <QElapsedTimer>
#include <QFile>
#include <QThread>
#include <stdexcept>

namespace {
class Worker final : public ModelWorker {
    QString path;
    int loads=0;
    static void mark(const QString& name) { QFile file(name); if(!file.open(QIODevice::WriteOnly))qFatal("Cannot create capture barrier"); }
public:
    ~Worker() override { if(!path.isEmpty())mark(path+".destroyed"); }
    bool reload(const QJsonObject& config,QString& error) override {
        path=config["path"].toString();
        QFile source(path); if(!source.open(QIODevice::ReadOnly))return false;
        if(source.readAll().startsWith("broken")) { error="Incomplete fixture"; return false; }
        if(QFile::exists(path+".reload-hold")) {
            mark(path+".reload-entered"); QElapsedTimer clock; clock.start();
            while(!QFile::exists(path+".reload-release")) {
                if(clock.elapsed()>10000)throw std::runtime_error("Reload barrier timed out");
                QThread::msleep(1);
            }
        }
        ++loads; return true;
    }
    void fit() override {}
    void settings(const QJsonObject&) override {}
    QJsonObject metrics() const override { return {{"loads",loads},{"engine",true}}; }
    void present() override {}
    QImage thumbnailSnapshot(QSize bound) override {
        mark(path+".capture");
        QElapsedTimer clock; clock.start();
        while(!QFile::exists(path+".release")) {
            if(clock.elapsed()>10000)throw std::runtime_error("Capture barrier timed out");
            QThread::msleep(1);
        }
        QImage image(bound,QImage::Format_RGB32); image.fill(Qt::red); return image;
    }
    void viewport(QSize,double) override {}
    void exposed(bool) override {}
    void scroll(double) override {}
    void drag(QPointF,bool) override {}
};
}
extern "C" Q_DECL_EXPORT const char* asset_preview_backend_abi() { return ASSET_PREVIEW_BACKEND_ABI; }
extern "C" Q_DECL_EXPORT ModelWorker* asset_preview_create_model_worker(QJsonObject) { return new Worker; }
