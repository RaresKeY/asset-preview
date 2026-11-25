// Compile with src/crash_log.cpp and Qt6Core; run outside UI timing workloads.
#include "crash_log.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <cstdio>

int main(int argc, char** argv) {
    QCoreApplication application(argc,argv);
    QTemporaryDir directory;
    if (!directory.isValid()) return 1;
    preview::CrashLog log(directory.path());
    QString error;
    if (!log.beginSession(error)) return log.finish(1);
    QJsonArray results;
    for (const int count:{1,16}) {
        QJsonArray previews;
        for (int i=0;i<count;++i)
            previews.append(QJsonObject{{"id",QString::number(i)},
                {"path",QString("/private/fixtures/project-%1/rendered-image.png").arg(i)},
                {"kind","image"},{"active",true},{"status","Live · image ready"},
                {"settings",QJsonObject{{"background","checker"},{"nearest",false}}}});
        const QJsonObject stable{{"selected","0"},{"comparison",QJsonArray{}},
            {"layout","projects"},{"previews",previews},
            {"last_request",QJsonObject{{"method","layout"},{"id","0"}}}};
        const auto encoded=QJsonDocument(stable).toJson(QJsonDocument::Compact);
        const auto independent=QJsonDocument::fromJson(encoded).object();
        for (const auto* mode:{"shared","independent","changed"}) {
            const bool changed=QByteArray(mode)=="changed";
            const bool independentStorage=QByteArray(mode)=="independent";
            constexpr int iterations=20000;
            preview::CrashLog::context(stable);
            QElapsedTimer timer; timer.start();
            for (int i=0;i<iterations;++i) {
                auto state=independentStorage && i%2?independent:stable;
                if (changed) state["selected"]=QString::number(i%2);
                preview::CrashLog::context(state);
            }
            results.append(QJsonObject{{"previews",count},{"mode",mode},
                {"iterations",iterations},{"elapsed_ms",timer.nsecsElapsed()/1e6},
                {"context_bytes",encoded.size()}});
        }
    }
    const auto bytes=QJsonDocument(results).toJson(QJsonDocument::Compact);
    std::puts(bytes.constData());
    return log.finish(0);
}
