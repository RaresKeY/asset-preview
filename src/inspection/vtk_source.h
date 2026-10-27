#pragma once

#include "../triangle_counts.h"

// Bounded source metadata, separate from VTK's single geometry copy. Node
// indices and paths are exact only within the accompanying artifact revision.
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QUrl>
#include <QtEndian>
#include <array>
#include <cmath>
#include <functional>
#include <vector>

namespace inspection {
using Matrix = std::array<double,16>; // row major
inline Matrix identityMatrix() { return {1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1}; }
inline Matrix multiply(const Matrix& a, const Matrix& b) {
    Matrix c{};
    for (int i=0;i<4;++i) for (int j=0;j<4;++j)
        for (int k=0;k<4;++k) c[i*4+j]+=a[i*4+k]*b[k*4+j];
    return c;
}
inline Matrix nodeMatrix(const QJsonObject& node) {
    Matrix m=identityMatrix();
    const auto saved=node["matrix"].toArray();
    if (saved.size()==16) {
        for (int i=0;i<4;++i) for(int j=0;j<4;++j) m[i*4+j]=saved[j*4+i].toDouble();
        return m;
    }
    const auto r=node["rotation"].toArray(), s=node["scale"].toArray(), t=node["translation"].toArray();
    const double x=r[0].toDouble(), y=r[1].toDouble(), z=r[2].toDouble(), w=r.size()==4?r[3].toDouble():1;
    m={1-2*(y*y+z*z),2*(x*y-z*w),2*(x*z+y*w),0,
       2*(x*y+z*w),1-2*(x*x+z*z),2*(y*z-x*w),0,
       2*(x*z-y*w),2*(y*z+x*w),1-2*(x*x+y*y),0,0,0,0,1};
    for(int i=0;i<3;++i) for(int j=0;j<3;++j) m[i*4+j]*=s.size()==3?s[j].toDouble():1;
    for(int i=0;i<3;++i) m[i*4+3]=t[i].toDouble();
    return m;
}
struct SourceNode {
    int index=-1, primitives=0;
    QString exportedName, hierarchyName;
    QJsonObject target;
    Matrix world=identityMatrix();
    std::vector<int> children;
};
struct Source {
    QString revision, dependencyRevision;
    std::vector<SourceNode> nodes;
    std::vector<int> roots;
    QMap<QString,QString> dependencies;
    std::optional<qint64> triangles;
};
inline QString fileHash(const QString& path, QString& error) {
    QFile f(path);
    if(!f.open(QIODevice::ReadOnly)) { error="Cannot read "+path; return {}; }
    if(f.size()>256*1024*1024) { error="Model or dependency exceeds 256 MiB"; return {}; }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if(!hash.addData(&f)) { error="Cannot hash "+path; return {}; }
    return QString::fromLatin1(hash.result().toHex());
}
inline bool loadSource(const QString& path, Source& out, QString& error) {
    const QString suffix=QFileInfo(path).suffix().toLower();
    if(suffix!="glb" && suffix!="gltf") { error="Inspection supports static GLB and glTF models"; return false; }
    out.revision=fileHash(path,error);
    if(out.revision.isEmpty()) return false;
    QFile file(path);
    if(!file.open(QIODevice::ReadOnly)) { error="Waiting for file"; return false; }
    if(file.size()>256*1024*1024) { error="File exceeds 256 MiB"; return false; }
    QByteArray json;
    if(suffix=="glb") {
        std::array<char,20> header{};
        if(file.read(header.data(),qint64(header.size()))!=qint64(header.size()) || qFromLittleEndian<quint32>(header.data())!=0x46546c67 ||
            qFromLittleEndian<quint32>(header.data()+4)!=2 ||
            qFromLittleEndian<quint32>(header.data()+8)!=quint64(file.size()) ||
            qFromLittleEndian<quint32>(header.data()+16)!=0x4e4f534a) { error="Invalid GLB header"; return false; }
        const auto size=qFromLittleEndian<quint32>(header.data()+12);
        if(size>16*1024*1024 || size>quint64(file.size()-20)) { error="Invalid or oversized GLB metadata"; return false; }
        json=file.read(size);
    } else {
        if(file.size()>16*1024*1024) { error="glTF metadata exceeds 16 MiB"; return false; }
        json=file.readAll();
    }
    QJsonParseError parse;
    const auto document=QJsonDocument::fromJson(json,&parse);
    if(parse.error!=QJsonParseError::NoError || !document.isObject()) { error="Invalid glTF metadata"; return false; }
    const auto root=document.object();
    const auto nodes=root["nodes"].toArray(), meshes=root["meshes"].toArray(), scenes=root["scenes"].toArray();
    if(!root["asset"].toObject()["version"].toString().startsWith("2.") || nodes.size()>65536 || scenes.isEmpty()) {
        error="Unsupported glTF version or node inventory"; return false;
    }
    if(!root["skins"].toArray().isEmpty() || !root["animations"].toArray().isEmpty()) {
        error="Inspection currently supports static glTF scenes without skins or animations"; return false;
    }
    const QSet<QString> supportedExtensions{"KHR_lights_punctual","KHR_materials_unlit","KHR_texture_transform"};
    for(const auto& extension:root["extensionsRequired"].toArray()) if(!supportedExtensions.contains(extension.toString())) {
        error="A required glTF extension is unsupported for inspection"; return false;
    }
    out.triangles=preview::gltfTriangles(root);
    for(const auto& mesh:meshes) for(const auto& primitive:mesh.toObject()["primitives"].toArray())
        if(!primitive.toObject()["targets"].toArray().isEmpty()) { error="Morph targets are unsupported for inspection"; return false; }
    for(const auto& field:{"buffers","images"}) for(const auto& resource:root[field].toArray()) {
        const auto uri=resource.toObject()["uri"].toString();
        if(uri.isEmpty() || uri.startsWith("data:")) continue;
        const QUrl url(uri);
        if(!url.scheme().isEmpty() || !url.host().isEmpty() || uri.size()>4096 || url.hasQuery() || url.hasFragment()) {
            error="Inspection resources must be local relative paths or embedded data"; return false;
        }
        const QString dependency=QDir(QFileInfo(path).absolutePath()).absoluteFilePath(QUrl::fromPercentEncoding(uri.toUtf8()));
        if(out.dependencies.contains(dependency)) continue;
        if(out.dependencies.size()>=4096) { error="Inspection dependency inventory exceeds 4096 files"; return false; }
        const auto digest=fileHash(dependency,error);
        if(digest.isEmpty()) return false;
        out.dependencies[dependency]=digest;
    }
    QCryptographicHash dependencyHash(QCryptographicHash::Sha256);
    for(auto it=out.dependencies.cbegin();it!=out.dependencies.cend();++it) {
        dependencyHash.addData(QDir(QFileInfo(path).absolutePath()).relativeFilePath(it.key()).toUtf8());
        dependencyHash.addData(QByteArray(1,'\0')); dependencyHash.addData(it.value().toLatin1());
    }
    out.dependencyRevision=QString::fromLatin1(dependencyHash.result().toHex());
    const int scene=root["scene"].toInt(0);
    if(scene<0 || scene>=scenes.size()) { error="Invalid glTF scene"; return false; }
    QSet<int> visited;
    qsizetype identityBytes=0;
    int partCount=0;
    std::function<int(int,const Matrix&,const QString&,int)> add;
    add=[&](int nodeIndex,const Matrix& parent,const QString& parentPath,int depth)->int {
        if(nodeIndex<0 || nodeIndex>=nodes.size() || depth>64 || visited.contains(nodeIndex)) {
            error="Invalid, repeated or excessively deep scene node"; return -1;
        }
        visited.insert(nodeIndex);
        const auto node=nodes[nodeIndex].toObject();
        if(node["extensions"].toObject().contains("EXT_mesh_gpu_instancing")) { error="GPU-instanced glTF nodes require a supported source-instance mapping"; return -1; }
        SourceNode item;
        item.index=nodeIndex;
        item.exportedName=node["name"].toString();
        if(item.exportedName.size()>1024 || item.exportedName.toUtf8().size()>2048) { error="Node label exceeds 1024 characters or 2048 UTF-8 bytes"; return -1; }
        item.hierarchyName=item.exportedName.isEmpty()?QString("node_%1").arg(nodeIndex):item.exportedName;
        item.world=multiply(parent,nodeMatrix(node));
        for(double d:item.world) if(!std::isfinite(d)) { error="Non-finite node transform"; return -1; }
        const QString selector=parentPath+QString("/nodes:%1").arg(nodeIndex);
        if(selector.size()>4096) { error="Scene selector exceeds 4096 characters"; return -1; }
        const auto extras=node["extras"].toObject();
        const auto semantic=extras["semantic_id"].toString(), label=extras["source_manufactured_label"].toString();
        if(semantic.size()>1024 || label.size()>1024 || semantic.toUtf8().size()>2048 || label.toUtf8().size()>2048) {
            error="Source identity exceeds 1024 characters or 2048 UTF-8 bytes"; return -1;
        }
        QString display=!label.isEmpty()?label:!semantic.isEmpty()?semantic:item.exportedName;
        if(display.isEmpty()) display=QString("Unnamed part %1").arg(nodeIndex);
        item.target={{"id",selector},{"path",selector},{"instance_selector",selector},{"node_index",nodeIndex},
            {"name",display},{"exported_name",item.exportedName}};
        if(!semantic.isEmpty()) item.target["semantic_id"]=semantic;
        if(!label.isEmpty()) item.target["source_manufactured_label"]=label;
        if(node.contains("mesh")) {
            const int mesh=node["mesh"].toInt(-1);
            if(mesh<0 || mesh>=meshes.size()) { error="Invalid glTF mesh"; return -1; }
            item.primitives=meshes[mesh].toObject()["primitives"].toArray().size();
            if(item.primitives>4096) { error="Too many mesh primitives"; return -1; }
            if(item.primitives>0 && ++partCount>8192) { error="Inspection inventory exceeds 8192 rendered parts"; return -1; }
        }
        identityBytes+=QJsonDocument(item.target).toJson(QJsonDocument::Compact).size();
        if(identityBytes>16*1024*1024) { error="Inspection source inventory exceeds 16 MiB"; return -1; }
        const int result=int(out.nodes.size()); out.nodes.push_back(std::move(item));
        for(const auto& child:node["children"].toArray()) {
            const int childInstance=add(child.toInt(-1),out.nodes[result].world,selector,depth+1);
            if(childInstance<0) return -1;
            out.nodes[result].children.push_back(childInstance);
        }
        return result;
    };
    for(const auto& node:scenes[scene].toObject()["nodes"].toArray()) {
        const int instance=add(node.toInt(-1),identityMatrix(),QString("scene:%1").arg(scene),0);
        if(instance<0) return false;
        out.roots.push_back(instance);
    }
    return true;
}
inline bool unchanged(const QString& path,const Source& source,QString& error) {
    if(fileHash(path,error)!=source.revision) { error="Model changed during inspection import"; return false; }
    for(auto it=source.dependencies.cbegin();it!=source.dependencies.cend();++it)
        if(fileHash(it.key(),error)!=it.value()) { error="Model dependency changed during inspection import"; return false; }
    return true;
}
}
