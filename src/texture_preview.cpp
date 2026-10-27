#include "texture_preview.h"
#include "preview_cache.h"
#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

namespace preview {
TexturePreview::TexturePreview()=default;
TexturePreview::~TexturePreview()=default;
TexturePreview::TexturePreview(TexturePreview&&) noexcept=default;
TexturePreview& TexturePreview::operator=(TexturePreview&&) noexcept=default;
namespace {
constexpr qint64 MiB=1024*1024, SourceLimit=64*MiB, OutputLimit=96*MiB, CacheLimit=224*MiB;
constexpr int Edge=1024;
constexpr size_t CacheFiles=2048;
const QByteArray Marker="Asset Preview texture derivatives v1\n";
QString sha(const QByteArray& bytes) { return QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex()); }
quint32 word(const QByteArray& bytes,qsizetype offset) {
    if(offset<0 || offset>bytes.size()-4) return 0;
    return quint32(quint8(bytes.at(offset))) | quint32(quint8(bytes.at(offset+1)))<<8 |
        quint32(quint8(bytes.at(offset+2)))<<16 | quint32(quint8(bytes.at(offset+3)))<<24;
}
void appendWord(QByteArray& bytes,quint32 value) { const auto little=qToLittleEndian(value); bytes.append(reinterpret_cast<const char*>(&little),4); }
bool regular(const QString& path) {
    struct stat info{};
    return ::lstat(QFile::encodeName(path).constData(),&info)==0 && S_ISREG(info.st_mode) && info.st_uid==::geteuid() && info.st_nlink==1;
}
QByteArray read(const QString& path,qint64 limit) {
    if(!regular(path)) return {};
    QFile file(path);
    if(!file.open(QIODevice::ReadOnly) || file.size()>limit) return {};
    return file.read(limit+1);
}
bool publish(const QString& path,const QByteArray& bytes) {
    if(QFileInfo::exists(path) && !regular(path)) return false;
    if(QFileInfo(path).isSymLink()) return false;
    QSaveFile file(path); file.setDirectWriteFallback(false);
    return file.open(QIODevice::WriteOnly) && file.setPermissions(QFile::ReadOwner|QFile::WriteOwner) &&
        file.write(bytes)==bytes.size() && file.commit();
}
bool privateDirectory(const QString& path) {
    struct stat info{};
    if(::lstat(QFile::encodeName(path).constData(),&info)!=0) {
        if(!QDir().mkpath(QFileInfo(path).absolutePath()) || ::mkdir(QFile::encodeName(path).constData(),0700)!=0) return false;
        if(::lstat(QFile::encodeName(path).constData(),&info)!=0) return false;
    }
    if(!S_ISDIR(info.st_mode) || info.st_uid!=::geteuid() || (info.st_mode&0077)) return false;
    const auto marker=QDir(path).filePath(".asset-preview-textures");
    if(read(marker,128)==Marker) return true;
    if(!QDir(path).isEmpty()) return false;
    return publish(marker,Marker);
}
// Only this cache's exact versioned filenames are eligible for eviction.
// The global lease also covers importer reads, so another process cannot evict
// an in-use derivative. There is no timer or hidden cache producer.
bool makeRoom(const QString& root,qint64 reserve,const QString& keep={},CacheStore* observer=nullptr) {
    QElapsedTimer timer; timer.start(); int removed=0;
    struct Item { QString path; qint64 size; QDateTime used; };
    std::vector<Item> files;
    qint64 total=0;
    const QRegularExpression owned("^v1-[0-9a-f]{64}\\.(glb|json)$");
    QDirIterator it(root,QDir::Files|QDir::Hidden|QDir::NoDotAndDotDot);
    int count=0;
    auto finish=[&](bool ok) {
        if (observer) observer->observe("glb_disk","prune_scan",{},total,ok?"admission":"incomplete",timer.nsecsElapsed()/1000,
            {{"files",count},{"removed",removed},{"success",ok}});
        return ok;
    };
    while(it.hasNext()) {
        it.next(); if(++count>8192) return finish(false);
        const auto info=it.fileInfo();
        if(!owned.match(info.fileName()).hasMatch()) continue;
        if(!regular(info.filePath())) return finish(false);
        files.push_back({info.filePath(),info.size(),info.lastModified()}); total+=info.size();
    }
    std::sort(files.begin(),files.end(),[](const Item& a,const Item& b) { return a.used<b.used; });
    size_t remaining=files.size();
    const size_t incoming=reserve?2:0;
    for(const auto& file:files) {
        if(total+reserve<=CacheLimit && remaining+incoming<=CacheFiles) return finish(true);
        if(QFileInfo(file.path).completeBaseName()==keep) continue;
        if(!QFile::remove(file.path)) return finish(false);
        if (observer) observer->observe("glb_disk","prune",QFileInfo(file.path).completeBaseName().mid(3),file.size,
            total+reserve>CacheLimit?"disk_bytes":"disk_entries");
        total-=file.size;
        ++removed;
        --remaining;
    }
    return finish(total+reserve<=CacheLimit && remaining+incoming<=CacheFiles);
}
qint64 integer(const QJsonValue& value,qint64 fallback=-1) {
    if(value.isUndefined()) return fallback;
    if(!value.isDouble()) return -1;
    const double number=value.toDouble();
    return std::isfinite(number) && number>=0 && number<=OutputLimit && std::floor(number)==number?qint64(number):-1;
}
QImage scaledChannels(const QImage& image) {
    // glTF normal/ORM maps ignore alpha. Scale RGB and alpha independently so
    // Qt's premultiplied RGBA filtering cannot erase data under zero alpha.
    auto colors=image.convertToFormat(QImage::Format_RGB32).scaled(Edge,Edge,Qt::KeepAspectRatio,Qt::SmoothTransformation);
    if(colors.isNull() || !image.hasAlphaChannel()) return colors;
    const auto alpha=image.convertToFormat(QImage::Format_Alpha8).scaled(colors.size(),Qt::IgnoreAspectRatio,Qt::SmoothTransformation)
        .convertToFormat(QImage::Format_Alpha8);
    auto result=colors.convertToFormat(QImage::Format_RGBA8888);
    if(alpha.isNull() || result.isNull()) return {};
    for(int y=0;y<result.height();++y) {
        auto* row=result.scanLine(y); const auto* values=alpha.constScanLine(y);
        for(int x=0;x<result.width();++x) row[x*4+3]=values[x];
    }
    return result;
}
bool extensionsSupported(const QJsonValue& value,const QSet<QString>& supported,int& remaining,int depth=0) {
    if(--remaining<0 || depth>128) return false;
    if(value.isArray()) {
        for(const auto& child:value.toArray()) if(!extensionsSupported(child,supported,remaining,depth+1)) return false;
    } else if(value.isObject()) {
        const auto object=value.toObject();
        if(object.contains("extensions")) {
            if(!object["extensions"].isObject()) return false;
            for(const auto& key:object["extensions"].toObject().keys()) if(!supported.contains(key)) return false;
        }
        for(auto it=object.begin();it!=object.end();++it)
            if(it.key()!="extras" && !extensionsSupported(it.value(),supported,remaining,depth+1)) return false;
    }
    return true;
}
bool derive(const QByteArray& input,QByteArray& output,int& resized,QString& reason) {
    auto fail=[&](const QString& text) { reason=text; return false; };
    if(input.size()<28 || input.left(4)!="glTF" || word(input,4)!=2 || word(input,8)!=input.size()) return fail("Unsupported GLB header");
    const auto jsonSize=word(input,12);
    if(jsonSize>16*MiB || jsonSize%4 || qint64(jsonSize)+28>input.size() || input.mid(16,4)!="JSON") return fail("Unsupported GLB JSON chunk");
    const qsizetype binHeader=20+jsonSize;
    const auto binSize=word(input,binHeader);
    if(binSize%4 || input.mid(binHeader+4,4)!=QByteArray("BIN\0",4) || binHeader+8+qint64(binSize)!=input.size()) return fail("Expected one embedded GLB buffer");
    QJsonParseError parseError;
    const auto parsed=QJsonDocument::fromJson(input.mid(20,jsonSize),&parseError);
    if(parseError.error!=QJsonParseError::NoError || !parsed.isObject()) return fail("Invalid GLB JSON");
    auto doc=parsed.object(); auto buffers=doc["buffers"].toArray(), images=doc["images"].toArray(), views=doc["bufferViews"].toArray();
    const QSet<QString> supportedExtensions{"KHR_materials_clearcoat","KHR_materials_unlit","KHR_materials_emissive_strength",
        "KHR_materials_ior","KHR_materials_specular","KHR_materials_transmission","KHR_materials_volume",
        "KHR_materials_sheen","KHR_materials_iridescence","KHR_materials_anisotropy","KHR_texture_transform",
        "KHR_mesh_quantization","KHR_lights_punctual","KHR_draco_mesh_compression","EXT_meshopt_compression"};
    for(const auto& name:doc["extensionsUsed"].toArray()) if(!supportedExtensions.contains(name.toString()))
        return fail("Unsupported extension uses the original model");
    int remaining=200000;
    if(!extensionsSupported(doc,supportedExtensions,remaining)) return fail("Unsupported or oversized extension metadata");
    if(buffers.size()!=1 || buffers[0].toObject().contains("uri") || integer(buffers[0].toObject()["byteLength"])<0 ||
        integer(buffers[0].toObject()["byteLength"])>binSize || images.isEmpty() || images.size()>64 || views.size()>65536)
        return fail("Expected bounded embedded images and buffer");
    // Extension-specific images can bypass the ordinary image source fields.
    // Leave those formats and their dependency resolution to the original loader.
    for(const auto& texture:doc["textures"].toArray()) if(!texture.toObject()["extensions"].toObject().isEmpty())
        return fail("Texture extensions use the original model");
    QByteArray binary=input.mid(binHeader+8,binSize);
    for(qsizetype index=0;index<images.size();++index) {
        auto image=images[index].toObject();
        const auto viewIndex=integer(image["bufferView"]);
        const auto mime=image["mimeType"].toString();
        if(image.contains("uri") || !image["extensions"].toObject().isEmpty() || viewIndex<0 || viewIndex>=views.size() ||
            (mime!="image/png" && mime!="image/jpeg")) return fail("Only embedded PNG/JPEG images are eligible");
        const auto view=views[viewIndex].toObject();
        const auto offset=integer(view["byteOffset"],0), length=integer(view["byteLength"]);
        if(integer(view["buffer"])!=0 || !view["extensions"].toObject().isEmpty() || offset<0 || length<=0 || offset+length>integer(buffers[0].toObject()["byteLength"]))
            return fail("Invalid embedded image bounds");
        QByteArray encoded=QByteArray::fromRawData(input.constData()+binHeader+8+offset,length);
        QBuffer buffer(&encoded); buffer.open(QIODevice::ReadOnly);
        QImageReader reader(&buffer,mime=="image/png"?"png":"jpeg");
        reader.setAutoTransform(false);
        const auto size=reader.size();
        if(!size.isValid() || size.width()>4096 || size.height()>4096 || qint64(size.width())*size.height()>16*1024*1024)
            return fail("Image decode exceeds preview bounds");
        if(size.width()<=Edge && size.height()<=Edge) continue;
        const QImage decoded=reader.read();
        if(decoded.isNull() || decoded.size()!=size) return fail("Image decode failed");
        if(decoded.depth()>32 || decoded.format()==QImage::Format_Grayscale16) return fail("High precision textures use the original model");
        const auto scaled=scaledChannels(decoded);
        QByteArray png; QBuffer sink(&png); sink.open(QIODevice::WriteOnly);
        if(scaled.isNull() || !scaled.save(&sink,"PNG") || png.size()>8*MiB || binary.size()+png.size()+3>OutputLimit-16*MiB)
            return fail("Preview output exceeds bounds");
        const qsizetype start=binary.size(); binary+=png; binary.append(QByteArray((4-binary.size()%4)%4,'\0'));
        image["bufferView"]=views.size(); image["mimeType"]="image/png";
        views.append(QJsonObject{{"buffer",0},{"byteOffset",start},{"byteLength",png.size()}});
        images[index]=image; ++resized;
    }
    if(!resized) return fail("Textures already fit within 1024 px");
    auto buffer=buffers[0].toObject(); buffer["byteLength"]=binary.size(); buffers[0]=buffer;
    doc["buffers"]=buffers; doc["bufferViews"]=views; doc["images"]=images;
    auto json=QJsonDocument(doc).toJson(QJsonDocument::Compact); json.append(QByteArray((4-json.size()%4)%4,' '));
    if(json.size()>16*MiB || 28+json.size()+binary.size()>OutputLimit) return fail("Preview output exceeds bounds");
    output="glTF"; appendWord(output,2); appendWord(output,28+json.size()+binary.size());
    appendWord(output,json.size()); output+="JSON"; output+=json;
    appendWord(output,binary.size()); output.append("BIN\0",4); output+=binary;
    return true;
}
}
TexturePreview texturePreview(const QString& source,CacheStore* observer) {
    TexturePreview result; result.path=source;
    result.metrics={{"requested","preview"},{"status","original"},{"max_edge",Edge},{"cache_hit",false}};
    QElapsedTimer timer; timer.start();
    QString diagnosticKey=sourceCacheKey("glb-derivative-v1",source);
    if (observer) observer->identify(diagnosticKey,"glb_derivative",source);
    auto fallback=[&](const QString& reason) {
        result.metrics["reason"]=reason; result.metrics["preparation_ms"]=timer.elapsed();
        if (observer) observer->observe("glb_disk","fallback",diagnosticKey,0,reason,timer.nsecsElapsed()/1000);
        result.lease.reset(); return std::move(result);
    };
    if(QFileInfo(source).suffix().compare("glb",Qt::CaseInsensitive)!=0 || QFileInfo(source).size()>SourceLimit)
        return fallback("Fast preview supports embedded GLB files up to 64 MiB");
    // Source symlinks are valid user inputs, but cache files are always regular
    // private files. Hash the exact bytes, including the complete embedded data.
    QFile file(source);
    if(!file.open(QIODevice::ReadOnly) || file.size()>SourceLimit) return fallback("Source is unreadable or exceeds bounds");
    const auto bytes=file.read(SourceLimit+1);
    if(bytes.size()>SourceLimit || !file.atEnd() || bytes.size()!=file.size()) return fallback("Source changed while reading");
    const auto digest=sha(bytes);
    result.metrics["source_sha256"]=digest; result.metrics["source_bytes"]=bytes.size();
    const auto root=QDir::cleanPath(qEnvironmentVariable("ASSET_PREVIEW_TEXTURE_CACHE",QStandardPaths::writableLocation(QStandardPaths::CacheLocation)+"/texture-previews-v1"));
    if(!QFileInfo(root).isAbsolute() || !privateDirectory(root)) return fallback("Private texture cache is unavailable");
    result.lease=std::make_unique<QLockFile>(QDir(root).filePath("cache.lock")); result.lease->setStaleLockTime(0);
    if(!result.lease->tryLock(0)) return fallback("Texture cache is busy");
    const auto key="v1-"+sha(("1024-channels-v2-"+QString::fromLatin1(qVersion())+"-"+digest).toUtf8());
    diagnosticKey=key.mid(3);
    if (observer) observer->identify(diagnosticKey,"glb_derivative",source);
    const auto target=QDir(root).filePath(key+".glb"), manifest=QDir(root).filePath(key+".json");
    const auto metadata=QJsonDocument::fromJson(read(manifest,4096)).object();
    auto cached=read(target,OutputLimit);
    bool hit=metadata["source_sha256"]==digest && metadata["output_sha256"]==sha(cached) &&
        integer(metadata["bytes"])==cached.size() && cached.size()>=28 && word(cached,8)==cached.size();
    cached.clear();
    if(hit) {
        if(!makeRoom(root,0,key,observer)) return fallback("Texture cache exceeds bounds");
        for(const auto& path:{target,manifest}) { QFile touch(path); if(touch.open(QIODevice::ReadOnly)) touch.setFileTime(QDateTime::currentDateTimeUtc(),QFileDevice::FileModificationTime); }
        result.metrics["resized_images"]=metadata["resized_images"];
    } else {
        if (observer) observer->observe("glb_disk","miss",diagnosticKey,0,"absent_or_invalid");
        QByteArray output; int resized=0; QString reason;
        if(!derive(bytes,output,resized,reason)) return fallback(reason);
        const QJsonObject meta{{"source_sha256",digest},{"output_sha256",sha(output)},{"bytes",output.size()},{"resized_images",resized}};
        if(!makeRoom(root,output.size()+4096,{},observer) || !publish(target,output) || !publish(manifest,QJsonDocument(meta).toJson(QJsonDocument::Compact)))
            return fallback("Texture cache could not be published");
        result.metrics["resized_images"]=resized;
    }
    // Check the original again before allowing the derivative to be imported.
    QFile currentSource(source);
    if(!currentSource.open(QIODevice::ReadOnly) || currentSource.size()!=bytes.size() || sha(currentSource.read(SourceLimit+1))!=digest)
        return fallback("Source changed during preview preparation");
    result.path=target; result.metrics["status"]="preview"; result.metrics["cache_hit"]=hit;
    if (observer) observer->observe("glb_disk",hit?"hit":"write",diagnosticKey,QFileInfo(target).size(),hit?"verified":"derived",timer.nsecsElapsed()/1000);
    result.metrics["preview_bytes"]=QFileInfo(target).size(); result.metrics["preparation_ms"]=timer.elapsed();
    return result;
}
}
