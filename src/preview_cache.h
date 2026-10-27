#pragma once
#include "audio_waveform.h"
#include "dependency_metadata.h"
#include <QCryptographicHash>
#include <QImage>
#include <QJsonObject>
#include <memory>

namespace preview {
// A payload has one retained owner. Taking it transfers it back to a visible
// view; the cache never counts a second reference to an active allocation.
struct CachedPayload {
    QImage image;
    QSize originalSize;
    std::shared_ptr<const AudioWaveform> waveform;
    qint64 bytes() const { return image.sizeInBytes()+(waveform?qint64(sizeof(AudioWaveform)):0); }
};
inline QString sourceCacheKey(const QString& kind,const QString& path) {
    const auto s=sourceFingerprint(path);
    if (!s.exists) return {};
    const auto identity=kind.toUtf8()+'\0'+path.toUtf8()+'\0'+
        QByteArray::number(s.device)+':'+QByteArray::number(s.inode)+':'+QByteArray::number(s.mode)+':'+
        QByteArray::number(s.size)+':'+QByteArray::number(s.modifiedSeconds)+':'+QByteArray::number(s.modifiedNanoseconds)+':'+
        QByteArray::number(s.changedSeconds)+':'+QByteArray::number(s.changedNanoseconds);
    return QString::fromLatin1(QCryptographicHash::hash(identity,QCryptographicHash::Sha256).toHex());
}
// The server owns this interface; optional playback modules use its vtable and
// do not acquire a second cache singleton or add a startup media dependency.
class CacheStore {
public:
    virtual ~CacheStore()=default;
    virtual CachedPayload take(const QString& key,bool waveform=false)=0;
    virtual void put(const QString& key,CachedPayload payload)=0;
    virtual void identify(const QString&,const QString&,const QString&) {}
    virtual void observe(const QString&,const QString&,const QString&,qint64,const QString& = {},qint64 = 0,const QJsonObject& = {}) {}
};
class PreviewCache final : public CacheStore {
public:
    static constexpr qint64 MiB=1024*1024, ItemLimit=32*MiB, RamLimit=128*MiB;
    static constexpr qint64 ThumbnailLimit=256*1024, ThumbnailRamLimit=16*MiB;
    static constexpr qint64 DiskItemLimit=MiB, DiskLimit=32*MiB;
    static constexpr int EntryLimit=256, DiskEntries=1024, ThumbnailWidth=160, ThumbnailHeight=100;
    PreviewCache();
    ~PreviewCache() override;
    CachedPayload take(const QString&,bool waveform=false) override;
    void put(const QString&,CachedPayload) override;
    bool hasImage(const QString&) const;
    bool allowsPreload() const;
    QImage thumbnail(const QString& key);
    // Painting never performs disk I/O, fingerprinting or diagnostic writes.
    QImage peekThumbnail(const QString& key) const;
    void putThumbnail(const QString& key,QImage image);
    void clear(bool disk=false,const QString& reason="manual");
    void trimForPressure();
    void identify(const QString&,const QString&,const QString&) override;
    void observe(const QString&,const QString&,const QString&,qint64,const QString& reason={},qint64 elapsedUs=0,const QJsonObject& details={}) override;
    void setDiagnosticsDirectory(const QString&);
    QJsonObject events(int limit=64,quint64 after=0) const;
    QJsonObject metrics() const;
private:
    struct Impl;
    std::unique_ptr<Impl> d;
};
}
