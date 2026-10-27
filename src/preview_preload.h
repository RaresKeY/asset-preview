#pragma once
#include "preview_cache.h"
#include <QObject>
#include <QThreadPool>
#include <atomic>

// One speculative return target, with no hidden view, player or generator.
class PreviewPreloader final : public QObject {
public:
    explicit PreviewPreloader(preview::PreviewCache*);
    ~PreviewPreloader() override;
    void target(const QString& id={},const QString& kind={},const QString& path={},const QStringList& dependencies={});
    QString targetId() const { return id; }
    QStringList targetPaths() const { return paths; }
    QJsonObject metrics() const;
private:
    void start();
    preview::PreviewCache* cache;
    QThreadPool worker;
    QString id,kind,path,identity,state="idle";
    QString dependencyIdentity;
    QStringList paths;
    qint64 lastBytes=0,lastUs=0;
    bool busy=false;
    std::shared_ptr<std::atomic_bool> cancelled;
};
