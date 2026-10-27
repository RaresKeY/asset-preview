#include "../src/preview_cache.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QLockFile>
#include <QTemporaryDir>
#include <QDateTime>
#include <iostream>
#include <stdexcept>
using namespace preview;
void expect(bool ok,const char* message) { if (!ok) throw std::runtime_error(message); }
QString key(int i) { return QString::fromLatin1(QCryptographicHash::hash(QByteArray::number(i),QCryptographicHash::Sha256).toHex()); }
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv); QTemporaryDir temp;
    qputenv("ASSET_PREVIEW_SMALL_CACHE",(temp.path()+"/cache").toUtf8());
    qputenv("ASSET_PREVIEW_TEXTURE_CACHE",(temp.path()+"/textures").toUtf8());
    PreviewCache cache;
    for (int i=0;i<5;++i) {
        QImage image(4096,2048,QImage::Format_RGBA8888); image.fill(Qt::red);
        cache.identify(key(i),"image",QString("/assets/image-%1.png").arg(i));
        cache.put(key(i),{std::move(image),{4096,2048},{}});
    }
    expect(cache.metrics()["bytes"].toInteger()==PreviewCache::RamLimit,"RAM quota");
    expect(cache.take(key(0)).image.isNull(),"least recently used eviction");
    const auto diagnostic=cache.metrics()["diagnostics"].toObject();
    expect(diagnostic["counters"].toObject()["ram.unused_evict"].toInt()>0 &&
        diagnostic["counters"].toObject()["ram.recent_refaults"].toInt()==1,"real cache eviction churn observed");
    auto taken=cache.take(key(1)); expect(!taken.image.isNull(),"eligible payload reused");
    expect(cache.metrics()["bytes"].toInteger()==96*PreviewCache::MiB,"taking transfers accounting");
    QImage large(4096,4096,QImage::Format_RGBA8888); large.fill(Qt::blue);
    cache.put(key(10),{std::move(large),{4096,4096},{}});
    expect(cache.take(key(10)).image.isNull(),"per-item refusal");
    cache.clear();
    auto wave=std::make_shared<AudioWaveform>(); wave->duration=2; wave->channels=2; wave->peak=.5;
    wave->bins[0]={-.25f,.25f,-.5f,.5f};
    cache.put(key(11),{{},{},wave}); cache.clear();
    const auto restored=cache.take(key(11),true).waveform;
    expect(restored && restored->bins==wave->bins && restored->duration==2,"waveform disk round trip");
    QImage thumb(160,100,QImage::Format_RGBA8888); thumb.fill(QColor(20,130,80,150));
    cache.putThumbnail(key(12),thumb); cache.clear();
    expect(cache.thumbnail(key(12))==thumb,"thumbnail disk round trip");
    QFile corrupt(temp.path()+"/cache/v1-"+key(12)+".bin"); expect(corrupt.open(QIODevice::WriteOnly),"corruption fixture"); corrupt.write("broken"); corrupt.close();
    cache.clear(); expect(cache.thumbnail(key(12)).isNull(),"corrupt disk entry rejected");
    cache.putThumbnail(key(12),thumb); cache.clear(); expect(!cache.thumbnail(key(12)).isNull(),"corrupt entry repaired");
    { QFile old(temp.path()+"/cache/v1-"+key(12)+".bin"); expect(old.open(QIODevice::ReadOnly),"expiry fixture");
      expect(old.setFileTime(QDateTime::currentDateTimeUtc().addDays(-31),QFileDevice::FileModificationTime),"expiry timestamp"); }
    cache.clear(); expect(cache.thumbnail(key(12)).isNull(),"unused entries expire on use");
    const auto asset=temp.path()+"/asset.png";
    { QFile f(asset); expect(f.open(QIODevice::WriteOnly),"source fixture"); f.write("first"); }
    const auto before=sourceCacheKey("image-v1",asset);
    { QFile f(asset+".next"); expect(f.open(QIODevice::WriteOnly),"replacement fixture"); f.write("other"); f.close(); QFile::remove(asset); QFile::rename(asset+".next",asset); }
    expect(before!=sourceCacheKey("image-v1",asset),"atomic same-size source replacement invalidates");
    const auto foreign=temp.path()+"/cache/keep.txt";
    { QFile f(foreign); expect(f.open(QIODevice::WriteOnly),"unrelated fixture"); f.write("preserve"); }
    cache.clear();
    for (int i=100;i<356;++i) cache.putThumbnail(key(i),thumb);
    for (int i=100;i<356;++i) expect(!cache.peekThumbnail(key(i)).isNull(),"all 256 tile thumbnails fit without eviction");
    expect(cache.metrics()["thumbnail_bytes"].toInteger()<=PreviewCache::ThumbnailRamLimit,"thumbnail RAM subquota");
    qint64 bytes=0; for (const auto& f:QDir(temp.path()+"/cache").entryInfoList({"v1-*.bin"},QDir::Files)) bytes+=f.size();
    expect(bytes<=PreviewCache::DiskLimit,"disk quota");
    cache.clear(true); expect(QFile::exists(foreign),"cleanup preserves unrelated files");
    expect(QDir(temp.path()+"/cache").entryList({"v1-*.bin"},QDir::Files).isEmpty(),"clear removes only owned data");
    const auto textures=temp.path()+"/textures"; expect(QDir().mkdir(textures),"old texture cache fixture");
    expect(QFile::setPermissions(textures,QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner),"private texture directory");
    { QFile marker(textures+"/.asset-preview-textures"); expect(marker.open(QIODevice::WriteOnly),"texture marker"); marker.write("Asset Preview texture derivatives v1\n"); }
    for (int i=0;i<3;++i) { QFile f(textures+"/v1-"+key(500+i)+".glb"); expect(f.open(QIODevice::WriteOnly) && f.resize(80*PreviewCache::MiB),"legacy sparse derivative"); }
    PreviewCache migrated; migrated.putThumbnail(key(520),thumb);
    qint64 textureBytes=0; for (const auto& f:QDir(textures).entryInfoList({"v1-*.glb"},QDir::Files)) textureBytes+=f.size();
    expect(textureBytes<=224*PreviewCache::MiB && migrated.metrics()["disk_entries"].toInt()>0,"legacy quota migrated before small-cache admission");
    const auto link=temp.path()+"/link"; expect(QFile::link(temp.path()+"/cache",link),"symlink fixture");
    qputenv("ASSET_PREVIEW_SMALL_CACHE",link.toUtf8()); PreviewCache unsafe;
    unsafe.putThumbnail(key(1),thumb); unsafe.clear(); expect(unsafe.thumbnail(key(1)).isNull(),"symlink root refused");
    std::cout<<"Preview cache bounds, transfer, persistence, integrity, freshness and cleanup passed\n";
}
