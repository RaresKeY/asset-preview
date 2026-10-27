#include "../src/dependency_metadata.h"
#include "../src/triangle_counts.h"
#include <QSaveFile>
#include <QTemporaryDir>
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>
#if defined(Q_OS_LINUX)
#include <fcntl.h>
#include <sys/inotify.h>
#include <unistd.h>
#endif

static void expect(bool condition,const char* message) {
    if (!condition) throw std::runtime_error(message);
}
static void write(const QString& path,const QByteArray& bytes) {
    QFile file(path);
    expect(file.open(QIODevice::WriteOnly),"open fixture");
    expect(file.write(bytes)==bytes.size(),"write fixture");
}
static QByteArray glbPrefix(QByteArray json,quint32 binaryBytes=0) {
    while (json.size()%4) json+=' ';
    QByteArray prefix(20,0);
    prefix.replace(0,4,"glTF"); prefix.replace(16,4,"JSON");
    qToLittleEndian<quint32>(2,prefix.data()+4);
    qToLittleEndian<quint32>(quint32(20+json.size())+binaryBytes,prefix.data()+8);
    qToLittleEndian<quint32>(quint32(json.size()),prefix.data()+12);
    return prefix+json;
}

// Any attempt to read past the JSON chunk fails. This models a large BIN chunk
// without allocating one and observes bytes requested by the shared GLB reader.
class JsonOnlyDevice final : public QIODevice {
public:
    explicit JsonOnlyDevice(QByteArray bytes) : data(std::move(bytes)) {
        open(QIODevice::ReadOnly|QIODevice::Unbuffered);
    }
    qint64 bytesRead=0;
    bool overread=false;
protected:
    qint64 readData(char* destination,qint64 maximum) override {
        if (maximum>data.size()-bytesRead) { overread=true; return -1; }
        std::memcpy(destination,data.constData()+bytesRead,size_t(maximum));
        bytesRead+=maximum;
        return maximum;
    }
    qint64 writeData(const char*,qint64) override { return -1; }
private:
    QByteArray data;
};

#if defined(Q_OS_LINUX)
class AccessWatch {
public:
    AccessWatch(const QStringList& paths) {
        descriptor=::inotify_init1(IN_NONBLOCK|IN_CLOEXEC);
        expect(descriptor>=0,"create metadata access watch");
        for (const auto& path:paths)
            expect(::inotify_add_watch(descriptor,QFile::encodeName(path).constData(),IN_OPEN|IN_ACCESS|IN_CLOSE_NOWRITE)>=0,
                "watch metadata access");
    }
    ~AccessWatch() { ::close(descriptor); }
    int opens() {
        int total=0;
        alignas(inotify_event) std::array<char,16384> bytes;
        for (;;) {
            const auto size=::read(descriptor,bytes.data(),bytes.size());
            if (size<0) { expect(errno==EAGAIN,"read metadata access watch"); break; }
            if (!size) break;
            for (ssize_t offset=0;offset<size;) {
                const auto* event=reinterpret_cast<const inotify_event*>(bytes.data()+offset);
                if (event->mask&IN_OPEN) ++total;
                offset+=ssize_t(sizeof(inotify_event)+event->len);
            }
        }
        return total;
    }
private:
    int descriptor=-1;
};
static void keepMtime(const QString& path,const struct stat& before) {
    const struct timespec times[2]={before.st_atim,before.st_mtim};
    expect(::utimensat(AT_FDCWD,QFile::encodeName(path).constData(),times,0)==0,"preserve fixture mtime");
}
#endif

int main() {
    QTemporaryDir dir;
    expect(dir.isValid(),"temporary fixture directory");
    const auto model=dir.filePath("scene.gltf");
    const auto image=dir.filePath("wall rough.png");
    const auto buffer=dir.filePath("geometry.bin");
    const auto resources=QByteArray(R"({"images":[{"uri":"wall%20rough.png"},{"uri":"wall%20rough.png"},
      {"uri":"data:image/png;base64,aaaa"},{"uri":"https://example.invalid/a.png"},
      {"uri":"file:///tmp/a.png"},{"uri":"/tmp/a.png"}],"buffers":[{"uri":"geometry.bin"}]})");
    write(model,resources);
    preview::DependencyMetadata metadata;
    const auto resourcePaths=QStringList{image,QStringLiteral("/tmp/a.png"),buffer};
    expect(metadata.paths(model)==resourcePaths,"decode local URIs, preserve local absolute paths, exclude external/embedded resources and deduplicate");
    expect(!metadata.complete(),"skipped URI schemes do not prove resource coverage");
    const auto closed=QByteArray(R"({"images":[{"uri":"wall%20rough.png"},
      {"bufferView":0,"mimeType":"image/png"},{"uri":"data:image/jpeg;base64,aaaa"}],
      "buffers":[{"uri":"geometry.bin"}],"extensionsUsed":["KHR_materials_emissive_strength"]})");
    write(model,closed);
    expect(metadata.paths(model)==QStringList{image,buffer} && metadata.complete(),"fully parsed local and embedded core resources have complete coverage");
    expect(metadata.imagePaths()==QStringList{image},"external images remain distinct from opaque buffers");
    expect(metadata.paths(model)==QStringList{image,buffer} && metadata.complete(),"coverage survives unchanged cache hits");
    for (const auto& document:{QByteArray("[]"),QByteArray("{partial"),
          QByteArray(R"({"buffers":[{"uri":"file:///tmp/a.bin"}]})"),
          QByteArray(R"({"buffers":[{"uri":"a.bin?version=1"}]})"),
          QByteArray(R"({"buffers":[{"uri":"a.bin#part"}]})"),
          QByteArray(R"({"buffers":[{"uri":"//host/a.bin"}]})"),
          QByteArray(R"({"images":[{"uri":"data:image/svg+xml;base64,aaaa"}]})"),
          QByteArray(R"({"images":[{"bufferView":0,"mimeType":"image/svg+xml"}]})"),
          QByteArray(R"({"buffers":[{"uri":17}]})"),QByteArray(R"({"images":{}})"),
          QByteArray(R"({"extensionsUsed":["unknown_external_resources"]})")}) {
        write(model,document); metadata.paths(model);
        expect(!metadata.complete(),"invalid or unrepresented resources keep coverage incomplete");
    }
    write(model,QByteArray("{}")+QByteArray(preview::DependencyMetadata::MaxBytes,' '));
    metadata.paths(model);
    expect(!metadata.complete(),"valid JSON prefix in an oversized file is incomplete");
    metadata.clear();
    expect(!metadata.complete() && metadata.imagePaths().isEmpty(),"eviction drops coverage and texture metadata");

    const auto binary=dir.filePath("large.glb");
    const auto prefix=glbPrefix(resources,128*1024*1024);
    JsonOnlyDevice device(prefix);
    expect(!preview::readGlbJson(device,prefix.size()+128*1024*1024,preview::DependencyMetadata::MaxBytes).isEmpty(),
        "read bounded GLB JSON with a large binary payload");
    expect(device.bytesRead==prefix.size() && !device.overread,"GLB metadata never requests BIN bytes");
    write(binary,prefix);
    QFile large(binary); expect(large.open(QIODevice::ReadWrite),"open sparse GLB");
    expect(large.resize(prefix.size()+128*1024*1024),"add sparse binary payload"); large.close();
    expect(metadata.paths(binary)==resourcePaths,"large GLB discovers external dependencies");
    write(binary,glbPrefix(closed,128*1024*1024));
    expect(large.open(QIODevice::ReadWrite) && large.resize(glbPrefix(closed).size()+128*1024*1024),"restore sparse GLB payload");
    large.close();
    expect(metadata.paths(binary)==QStringList{image,buffer} && metadata.complete(),"large BIN does not make bounded complete JSON incomplete");
    auto oversized=glbPrefix("{}",quint32(preview::DependencyMetadata::MaxBytes));
    qToLittleEndian<quint32>(quint32(preview::DependencyMetadata::MaxBytes+1),oversized.data()+12);
    JsonOnlyDevice bounded(oversized);
    expect(preview::readGlbJson(bounded,oversized.size()+preview::DependencyMetadata::MaxBytes,
        preview::DependencyMetadata::MaxBytes).isEmpty(),"oversized GLB JSON is rejected");
    expect(bounded.bytesRead==20 && !bounded.overread,"reject oversized JSON before requesting its payload");
    auto wrongChunk=prefix; wrongChunk.replace(16,4,"BIN "); write(binary,wrongChunk);
    expect(metadata.paths(binary).isEmpty(),"non-JSON GLB first chunk is unavailable");
    expect(!metadata.complete(),"invalid GLB header cannot prove coverage");

    const auto obj=dir.filePath("mesh.obj"), mtl=dir.filePath("surface.mtl");
    write(obj,"mtllib surface.mtl\nmtllib surface.mtl\nf 1 2 3\n");
    write(mtl,"newmtl surface\nmap_Kd wall rough.png\nmap_Bump -bm 0.5 normal.png\ndisp height.png\nnorm normal.png\n");
    const auto original=QStringList{mtl,image,dir.filePath("normal.png"),dir.filePath("height.png")};
#if defined(Q_OS_LINUX)
    AccessWatch discoveryWatch({obj,mtl});
#endif
    expect(metadata.paths(obj)==original,"ordinary OBJ/MTL paths, options and duplicate texture references");
    expect(!metadata.complete(),"best-effort OBJ/MTL grammar does not certify complete coverage");
#if defined(Q_OS_LINUX)
    expect(discoveryWatch.opens()==2,"duplicate mtllib lines parse each distinct file once");
#endif
    expect(metadata.paths(obj)==original,"unchanged metadata cache result");
#if defined(Q_OS_LINUX)
    expect(discoveryWatch.opens()==0,"unchanged dependencies stat without opening or reparsing source files");
#endif
    write(image,"changed external texture");
    expect(metadata.paths(obj)==original,"external texture edits keep derived metadata valid");
#if defined(Q_OS_LINUX)
    expect(discoveryWatch.opens()==0,"texture changes do not reparse model/MTL metadata");
    struct stat prior{};
    expect(::stat(QFile::encodeName(mtl).constData(),&prior)==0,"stat original MTL");
    const auto oldStamp=preview::sourceFingerprint(mtl);
#endif
    const auto edited=QByteArray("newmtl surface\nmap_Kd sand rough.png\nmap_Bump -bm 0.5 normal.png\ndisp height.png\nnorm normal.png\n");
    write(mtl,edited);
#if defined(Q_OS_LINUX)
    keepMtime(mtl,prior);
    const auto newStamp=preview::sourceFingerprint(mtl);
    expect(oldStamp.size==newStamp.size && oldStamp.modifiedSeconds==newStamp.modifiedSeconds &&
        oldStamp.modifiedNanoseconds==newStamp.modifiedNanoseconds && oldStamp!=newStamp,
        "ctime invalidates same-size MTL edits with preserved nanosecond mtime");
    discoveryWatch.opens();
#endif
    auto expected=original; expected[1]=dir.filePath("sand rough.png");
    expect(metadata.paths(obj)==expected,"MTL edit invalidates derived texture paths");
#if defined(Q_OS_LINUX)
    expect(discoveryWatch.opens()==2,"changed MTL rebuilds the bounded manifest");
#endif
    QSaveFile replacement(mtl);
    expect(replacement.open(QIODevice::WriteOnly),"open atomic MTL replacement");
    const auto replacementBytes=QByteArray("map_Kd replaced.png\n");
    expect(replacement.write(replacementBytes)==replacementBytes.size() && replacement.commit(),"publish atomic MTL replacement");
    expect(metadata.paths(obj)==QStringList{mtl,dir.filePath("replaced.png")},"atomic MTL replacement invalidates cached dependencies");
    expect(QFile::remove(mtl),"remove MTL");
    expect(metadata.paths(obj)==QStringList{mtl},"missing MTL remains a dependency");
    write(mtl,"map_Kd restored.png\n");
    expect(metadata.paths(obj)==QStringList{mtl,dir.filePath("restored.png")},"created MTL invalidates an initially missing library");
    const auto later=dir.filePath("later/material.mtl");
    write(obj,"mtllib later/material.mtl\nf 1 2 3\n");
    expect(metadata.paths(obj)==QStringList{later},"model edit replaces its library dependencies, including missing directories");
    expect(QDir().mkpath(QFileInfo(later).absolutePath()),"create later MTL directory");
    write(later,"map_Kd later.png\n");
    expect(metadata.paths(obj)==QStringList{later,dir.filePath("later/later.png")},"new directory and MTL introduce texture dependencies");

    const auto missing=dir.filePath("missing.gltf");
    expect(metadata.paths(missing).isEmpty(),"missing model metadata");
    write(missing,resources);
    expect(metadata.paths(missing)==resourcePaths,"created model replaces cached missing metadata");
    write(missing,"{partial");
    expect(metadata.paths(missing).isEmpty(),"partial JSON is best-effort unavailable");
    write(missing,resources);
    expect(metadata.paths(missing)==resourcePaths,"parse failures recover when source changes");

    QJsonArray images;
    for (int i=0;i<300;++i) images.append(QJsonObject{{"uri",QString("image-%1.png").arg(i)}});
    write(model,QJsonDocument(QJsonObject{{"images",images}}).toJson(QJsonDocument::Compact));
    const auto capped=metadata.paths(model);
    expect(capped.size()==preview::DependencyMetadata::MaxPaths && capped.last()==dir.filePath("image-255.png"),
        "dependency paths stop at the bound during discovery");
    expect(!metadata.complete(),"a capped path inventory cannot certify coverage");
    QJsonArray longImages;
    for (int i=0;i<200;++i) longImages.append(QJsonObject{{"uri",QString(12000,'a')+QString::number(i)}});
    write(model,QJsonDocument(QJsonObject{{"images",longImages}}).toJson(QJsonDocument::Compact));
    metadata.paths(model);
    expect(!metadata.complete(),"retained-text refusal makes coverage incomplete below the path-count cap");
    QByteArray libraries;
    for (int i=0;i<70;++i) {
        const auto name=QString("wide-%1.mtl").arg(i);
        libraries+="mtllib "+name.toUtf8()+'\n';
        write(dir.filePath(name),"map_Kd "+QByteArray(64*1024,'a')+QByteArray::number(i)+".png\n");
    }
    write(obj,libraries);
    qsizetype pathBytes=0;
    const auto wide=metadata.paths(obj);
    for (const auto& path:wide) pathBytes+=path.size()*qsizetype(sizeof(QChar));
    expect(pathBytes<=preview::DependencyMetadata::MaxBytes && wide.size()<140,"aggregate retained path text is bounded");
    write(obj,QByteArray(preview::DependencyMetadata::MaxBytes,'#')+"\nmtllib invisible.mtl\n");
    expect(metadata.paths(obj).isEmpty(),"OBJ discovery reads only its bounded prefix");
    metadata.clear();

    preview::TriangleMetadata counts;
    write(obj,"f 1 2 3\n");
#if defined(Q_OS_LINUX)
    AccessWatch countWatch({obj});
#endif
    expect(counts.count(obj)==1,"initial cached triangle count");
#if defined(Q_OS_LINUX)
    expect(countWatch.opens()==1,"initial scalar triangle count reads source once");
#endif
    expect(counts.count(obj)==1,"reuse scalar count for unchanged source");
#if defined(Q_OS_LINUX)
    expect(countWatch.opens()==0,"scalar cache hit does not read geometry metadata");
#endif
    const auto beforeReplacement=preview::sourceFingerprint(obj);
    write(obj,"f 1 2 3 4\n");
    expect(!counts.count(obj,beforeReplacement),"reject scalar metadata for a source changed during import");
    expect(counts.count(obj)==2,"raced source does not poison the next scalar cache result");
    write(obj,"f 1\n");
    expect(!counts.count(obj),"unavailable scalar metadata is distinct from zero");
    write(obj,"v 0 0 0\n");
    expect(counts.count(obj)==0,"source change recovers cached unavailable scalar metadata");
#if defined(Q_OS_LINUX)
    countWatch.opens();
#endif
    counts.clear();
    expect(counts.count(obj)==0,"explicit scalar cache eviction remains reloadable");
#if defined(Q_OS_LINUX)
    expect(countWatch.opens()==1,"evicting a visible view drops its scalar cache");
#endif
    std::cout << "Dependency metadata checks passed\n";
}
