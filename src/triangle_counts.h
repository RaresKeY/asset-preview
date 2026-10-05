#pragma once
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QtEndian>
#include <algorithm>
#include <limits>
#include <optional>

namespace preview {
// Count source primitives without importing a second copy of the geometry.
// Unavailable metadata is distinct from a scene with zero surface triangles.
inline std::optional<qint64> gltfTriangles(const QJsonObject& document) {
    const auto accessors=document["accessors"].toArray();
    const auto meshes=document["meshes"].toArray();
    const auto nodes=document["nodes"].toArray();
    const auto scenes=document["scenes"].toArray();
    const int scene=document["scene"].toInt(0);
    if (scene<0 || scene>=scenes.size()) return {};
    auto count=[&](QJsonValue index)->std::optional<qint64> {
        const qint64 i=index.toInteger(-1);
        if (!index.isDouble() || index.toDouble()!=double(i) || i<0 || i>=accessors.size()) return {};
        const QJsonValue value=accessors[i].toObject()["count"];
        const qint64 n=value.toInteger(-1);
        if (!value.isDouble() || value.toDouble()!=double(n) || n<0) return {};
        return n;
    };
    QJsonArray pending=scenes[scene].toObject()["nodes"].toArray();
    QSet<int> visited;
    qint64 total=0;
    while (!pending.isEmpty()) {
        const auto value=pending.takeAt(pending.size()-1);
        const int index=value.toInt(-1);
        if (!value.isDouble() || value.toDouble()!=index || index<0 || index>=nodes.size() || visited.contains(index)) return {};
        visited.insert(index);
        const auto node=nodes[index].toObject();
        for (const auto& child:node["children"].toArray()) pending.append(child);
        if (!node.contains("mesh")) continue;
        const int mesh=node["mesh"].toInt(-1);
        if (mesh<0 || mesh>=meshes.size() || node["mesh"].toDouble()!=mesh) return {};
        qint64 instances=1;
        const auto extensions=node["extensions"].toObject();
        if (extensions.contains("EXT_mesh_gpu_instancing")) {
            const auto attributes=extensions["EXT_mesh_gpu_instancing"].toObject()["attributes"].toObject();
            if (attributes.isEmpty()) return {};
            const auto first=count(attributes.begin().value());
            if (!first) return {};
            instances=*first;
            for (auto i=attributes.begin();i!=attributes.end();++i) if (count(i.value())!=first) return {};
        }
        for (const auto& item:meshes[mesh].toObject()["primitives"].toArray()) {
            const auto primitive=item.toObject();
            const int mode=primitive["mode"].toInt(4);
            if (mode<0 || mode>6) return {};
            if (mode<4) continue; // Points and lines have no surface triangles.
            const auto n=count(primitive.contains("indices") ? primitive["indices"] : primitive["attributes"].toObject()["POSITION"]);
            if (!n || (mode==4 && *n%3!=0)) return {};
            const qint64 triangles=mode==4 ? *n/3 : std::max(qint64(0),*n-2);
            if (triangles && instances>(std::numeric_limits<qint64>::max()-total)/triangles) return {};
            total+=triangles*instances;
        }
    }
    return total;
}

inline std::optional<qint64> sourceTriangles(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    const auto suffix=QFileInfo(path).suffix().toLower();
    constexpr qint64 MaxJson=16*1024*1024;
    if (suffix=="glb" || suffix=="gltf") {
        QByteArray json;
        if (suffix=="glb") {
            const auto header=file.read(20);
            if (header.size()!=20 || header.first(4)!="glTF" || qFromLittleEndian<quint32>(header.constData()+4)!=2 ||
                qFromLittleEndian<quint32>(header.constData()+8)!=file.size() || header.mid(16,4)!="JSON") return {};
            const auto length=qFromLittleEndian<quint32>(header.constData()+12);
            if (length>MaxJson || length>file.size()-20) return {};
            json=file.read(length);
        } else {
            if (file.size()>MaxJson) return {};
            json=file.readAll();
        }
        QJsonParseError error;
        const auto document=QJsonDocument::fromJson(json,&error);
        if (error.error!=QJsonParseError::NoError || !document.isObject()) return {};
        return gltfTriangles(document.object());
    }
    if (suffix!="obj") return {};
    qint64 total=0, vertices=0;
    bool continued=false;
    constexpr qint64 MaxLine=1024*1024;
    while (!file.atEnd()) {
        auto line=file.readLine(MaxLine);
        if (line.size()==MaxLine-1 && !line.endsWith('\n') && !file.atEnd()) return {};
        const auto comment=line.indexOf('#');
        if (comment>=0) line.truncate(comment);
        line=line.simplified();
        const bool more=line.endsWith('\\');
        if (more) line.chop(1);
        if (!continued) {
            if (!line.startsWith("f ")) continue;
            vertices=0; line.remove(0,2);
        }
        for (const auto& token:line.split(' ')) if (!token.isEmpty()) ++vertices;
        continued=more;
        if (!more) {
            if (vertices<3 || total>std::numeric_limits<qint64>::max()-(vertices-2)) return {};
            total+=vertices-2;
        }
    }
    return continued ? std::nullopt : std::optional<qint64>(total);
}
}
