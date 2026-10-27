#pragma once
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <QUrl>
#include <QtEndian>
#include <array>
#include <cstring>
#include <utility>
#if defined(Q_OS_LINUX) || defined(Q_OS_DARWIN)
#include <sys/stat.h>
#endif

namespace preview {
// Follow the actual source, including atomic replacement and changes whose size
// and mtime were preserved. Missing paths are also a cache identity.
struct SourceFingerprint {
    bool exists=false;
    quint64 device=0, inode=0, mode=0;
    qint64 size=0, modifiedSeconds=0, modifiedNanoseconds=0;
    qint64 changedSeconds=0, changedNanoseconds=0;
    bool operator==(const SourceFingerprint&) const = default;
};

inline SourceFingerprint sourceFingerprint(const QString& path) {
    SourceFingerprint value;
#if defined(Q_OS_LINUX) || defined(Q_OS_DARWIN)
    struct stat info{};
    if (::stat(QFile::encodeName(path).constData(),&info)) return value;
    value.exists=true;
    value.device=info.st_dev; value.inode=info.st_ino; value.mode=info.st_mode;
    value.size=info.st_size;
#if defined(Q_OS_DARWIN)
    value.modifiedSeconds=info.st_mtimespec.tv_sec; value.modifiedNanoseconds=info.st_mtimespec.tv_nsec;
    value.changedSeconds=info.st_ctimespec.tv_sec; value.changedNanoseconds=info.st_ctimespec.tv_nsec;
#else
    value.modifiedSeconds=info.st_mtim.tv_sec; value.modifiedNanoseconds=info.st_mtim.tv_nsec;
    value.changedSeconds=info.st_ctim.tv_sec; value.changedNanoseconds=info.st_ctim.tv_nsec;
#endif
#else
    const QFileInfo info(path);
    if (!info.exists()) return value;
    value.exists=true; value.size=info.size();
    value.mode=quint64(info.permissions()) | (quint64(info.isFile())<<32) | (quint64(info.isDir())<<33);
    value.modifiedSeconds=info.lastModified().toMSecsSinceEpoch();
    value.changedSeconds=info.fileTime(QFileDevice::FileMetadataChangeTime).toMSecsSinceEpoch();
#endif
    return value;
}

// The caller opens GLBs unbuffered so no read-ahead copies the BIN payload.
// Keeping the device interface also lets the metadata tests reject BIN reads.
inline QByteArray readGlbJson(QIODevice& file,qint64 fileSize,qint64 maximum) {
    std::array<char,20> header{};
    if (file.read(header.data(),qint64(header.size()))!=qint64(header.size()) || std::memcmp(header.data(),"glTF",4)!=0 ||
        qFromLittleEndian<quint32>(header.data()+4)!=2 || qFromLittleEndian<quint32>(header.data()+8)!=fileSize ||
        std::memcmp(header.data()+16,"JSON",4)!=0) return {};
    const auto length=qFromLittleEndian<quint32>(header.data()+12);
    if (length>maximum || length>fileSize-20) return {};
    const auto json=file.read(length);
    return json.size()==length ? json : QByteArray{};
}

// One active entry owns one bounded manifest. No JSON, texture or geometry data
// survives discovery. Explicit caller watch/map paths remain outside this cache.
class DependencyMetadata {
public:
    static constexpr qsizetype MaxPaths=256;
    static constexpr qint64 MaxBytes=4*1024*1024;

    QStringList paths(const QString& modelPath) {
        const auto before=sourceFingerprint(modelPath);
        if (ready && modelPath==record.modelPath && before==record.modelStamp && librariesUnchanged(record))
            return record.paths;

        Record next;
        next.modelPath=modelPath; next.modelStamp=before;
        discover(next);
        // Do not preserve a manifest parsed across an in-place or replacement
        // save. A later notification can retry from the authoritative files.
        if (before==sourceFingerprint(modelPath) && librariesUnchanged(next)) {
            record=std::move(next); ready=true;
            return record.paths;
        }
        auto result=std::move(next.paths);
        clear();
        return result;
    }

    void clear() { record=Record{}; ready=false; }
    bool complete() const { return ready && record.complete; }
    QStringList imagePaths() const { return ready ? record.images : QStringList{}; }

private:
    struct Library { QString path; SourceFingerprint stamp; };
    struct Record {
        QString modelPath;
        SourceFingerprint modelStamp;
        QStringList paths, images;
        QList<Library> libraries;
        bool complete=false;
    };
    Record record;
    bool ready=false;

    static bool librariesUnchanged(const Record& value) {
        for (const auto& library:value.libraries)
            if (sourceFingerprint(library.path)!=library.stamp) return false;
        return true;
    }
    static QString absolute(const QString& path) {
        return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    }
    static QByteArray readSmall(const QString& path) {
        QFile file(path);
        return QFileInfo(path).isFile() && file.open(QIODevice::ReadOnly) ? file.read(MaxBytes) : QByteArray{};
    }
    static void discover(Record& value) {
        const auto suffix=QFileInfo(value.modelPath).suffix().toLower();
        if (suffix!="gltf" && suffix!="glb" && suffix!="obj") return;
        const QDir dir=QFileInfo(value.modelPath).absoluteDir();
        QSet<QString> seen;
        qsizetype pathBytes=0;
        auto append=[&](const QString& path) {
            if (seen.contains(path)) return true;
            // Bound retained strings as well as their count, even for malicious
            // metadata containing extremely long or repeated path prefixes.
            if (value.paths.size()>=MaxPaths || path.size()>(MaxBytes-pathBytes)/qsizetype(sizeof(QChar))) {
                value.complete=false;
                return false;
            }
            seen.insert(path); value.paths.append(path); pathBytes+=path.size()*qsizetype(sizeof(QChar));
            return true;
        };
        if (suffix=="gltf" || suffix=="glb") {
            QByteArray json;
            if (suffix=="glb") {
                QFile file(value.modelPath);
                if (QFileInfo(value.modelPath).isFile() && file.open(QIODevice::ReadOnly|QIODevice::Unbuffered))
                    json=readGlbJson(file,file.size(),MaxBytes);
            } else json=readSmall(value.modelPath);
            QJsonParseError error;
            const auto document=QJsonDocument::fromJson(json,&error);
            const auto root=document.object();
            value.complete=error.error==QJsonParseError::NoError && document.isObject() &&
                (suffix=="glb" || (value.modelStamp.exists && value.modelStamp.size==json.size()));
            static const QSet<QString> coveredExtensions{"KHR_lights_punctual","KHR_materials_unlit",
                "KHR_texture_transform","KHR_materials_emissive_strength","EXT_mesh_gpu_instancing"};
            for (const auto& field:{"extensionsUsed","extensionsRequired"}) {
                if (root.contains(field) && !root[field].isArray()) value.complete=false;
                for (const auto& extension:root[field].toArray())
                    if (!extension.isString() || !coveredExtensions.contains(extension.toString())) value.complete=false;
            }
            QSet<QString> images;
            for (const auto& key:{"images","buffers"}) {
                const bool imageResources=qstrcmp(key,"images")==0;
                if (root.contains(key) && !root[key].isArray()) value.complete=false;
                for (const auto& item:root[key].toArray()) {
                    if (value.paths.size()>=MaxPaths) { value.complete=false; return; }
                    const auto resource=item.toObject();
                    if (!item.isObject() || (resource.contains("uri") && !resource["uri"].isString())) value.complete=false;
                    const auto uri=resource["uri"].toString();
                    const QUrl url(uri);
                    if (imageResources && (uri.isEmpty() || uri.startsWith("data:"))) {
                        const auto mime=resource["mimeType"].toString();
                        const bool embedded=uri.isEmpty() ? mime=="image/png" || mime=="image/jpeg" || mime=="image/webp" :
                            uri.startsWith("data:image/png;") || uri.startsWith("data:image/jpeg;") || uri.startsWith("data:image/webp;");
                        if (!embedded) value.complete=false;
                    }
                    if (uri.isEmpty() || uri.startsWith("data:")) continue;
                    if (!url.isValid() || !url.isRelative() || !url.host().isEmpty() || url.hasQuery() || url.hasFragment())
                        value.complete=false;
                    if (url.isRelative()) {
                        const auto path=absolute(dir.filePath(QUrl::fromPercentEncoding(uri.toUtf8())));
                        if (append(path) && imageResources && !images.contains(path)) {
                            value.images.append(path); images.insert(path);
                        }
                    }
                }
            }
            return;
        }
        static const QRegularExpression libraries("^\\s*mtllib\\s+(.+)$",QRegularExpression::MultilineOption);
        static const QRegularExpression maps("^\\s*(?:map_\\w+|bump|disp|norm)\\s+(.+)$",QRegularExpression::MultilineOption);
        auto matches=libraries.globalMatch(QString::fromUtf8(readSmall(value.modelPath)));
        QSet<QString> parsedLibraries;
        while (matches.hasNext() && value.paths.size()<MaxPaths) {
            const auto path=absolute(dir.filePath(matches.next().captured(1).trimmed()));
            if (parsedLibraries.contains(path) || !append(path)) continue;
            parsedLibraries.insert(path);
            value.libraries.append({path,sourceFingerprint(path)});
            auto textures=maps.globalMatch(QString::fromUtf8(readSmall(path)));
            const auto materialDir=QFileInfo(path).absoluteDir();
            while (textures.hasNext() && value.paths.size()<MaxPaths) {
                auto name=textures.next().captured(1).trimmed();
                if (name.startsWith('-')) {
                    const auto words=QProcess::splitCommand(name);
                    name=words.value(words.size()-1);
                }
                append(absolute(materialDir.filePath(name)));
            }
        }
    }
};
}
