#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <memory>

namespace preview {
class CacheDiagnostics {
public:
    static constexpr int EventLimit=128, EventBytesLimit=64*1024, IdentityLimit=128;
    static constexpr int LogLimit=256*1024, PendingLimit=32*1024;
    CacheDiagnostics();
    ~CacheDiagnostics();
    void setDirectory(const QString&);
    void identify(const QString& key,const QString& kind,const QString& path);
    void record(const QString& tier,const QString& action,const QString& key,qint64 bytes,
                const QString& reason={},qint64 elapsedUs=0,const QJsonObject& details={});
    QJsonObject summary() const;
    QJsonObject events(int limit=64,quint64 after=0) const;
private:
    struct Impl;
    std::unique_ptr<Impl> d;
};
}
