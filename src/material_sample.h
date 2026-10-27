#pragma once
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>
#include <QVector3D>
#include <QtEndian>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>

namespace preview {
// A temporary material assignment. Geometry, transforms and the source file
// stay intact. Tangents are added only when a normal-mapped primitive needs them.
inline QByteArray materialOnModel(const QString& path,const QString& albedo,const QJsonObject& maps) {
    auto fail=[](const char* message) { throw std::runtime_error(message); };
    QFile file(path);
    if(QFileInfo(path).suffix().toLower()!="glb")fail("Choose a UV-mapped GLB model");
    if(!file.open(QIODevice::ReadOnly))fail("Cannot open the preview model");
    if(file.size()>256*1024*1024)fail("Preview model exceeds 256 MiB");
    const auto original=file.readAll();
    if(original.size()>256*1024*1024)fail("Preview model exceeds 256 MiB");
    auto wordAt=[&](qsizetype offset){if(offset<0 || offset+4>original.size())fail("Truncated GLB header");quint32 value=0;for(int i=0;i<4;++i)value|=quint32(uchar(original.at(offset+i)))<<(i*8);return value;};
    if(original.size()<28 || original.left(4)!="glTF" || wordAt(4)!=2 || wordAt(8)!=original.size())fail("Invalid GLB 2 model");
    const auto jsonSize=wordAt(12);
    if(jsonSize>4*1024*1024 || quint64(jsonSize)+28>quint64(original.size()) || original.mid(16,4)!="JSON")fail("Invalid or oversized GLB metadata");
    QJsonParseError parse;
    const auto parsed=QJsonDocument::fromJson(original.mid(20,jsonSize),&parse);
    if(parse.error!=QJsonParseError::NoError || !parsed.isObject())fail("Invalid GLB JSON");
    auto doc=parsed.object();
    const auto binOffset=20+jsonSize;
    const auto binSize=wordAt(binOffset);
    if(original.mid(binOffset+4,4)!=QByteArray("BIN\0",4) || quint64(binOffset)+8+binSize!=quint64(original.size()))fail("Expected a self-contained GLB binary buffer");
    auto buffers=doc["buffers"].toArray();
    if(buffers.size()!=1 || buffers[0].toObject().contains("uri"))fail("Preview models must use one embedded GLB buffer");
    QByteArray binary=original.mid(binOffset+8,binSize);
    auto views=doc["bufferViews"].toArray(), accessors=doc["accessors"].toArray();
    auto meshes=doc["meshes"].toArray();
    if(meshes.isEmpty() || meshes.size()>4096)fail("Preview model has no supported meshes");
    const auto materials=doc["materials"].toArray();const int materialIndex=int(materials.size());
    auto number=[&](int index,int vertex,int component,int width)->float {
        if(index<0 || index>=accessors.size())fail("Invalid model attribute");
        const auto accessor=accessors[index].toObject();
        if(accessor["componentType"]!=5126 || accessor["type"]!=(width==2?"VEC2":"VEC3") || accessor.contains("sparse"))fail("Material preview needs ordinary float UVs, positions and normals");
        if(vertex<0 || vertex>=accessor["count"].toInt())fail("Invalid model vertex index");
        const int viewIndex=accessor["bufferView"].toInt(-1);
        if(viewIndex<0 || viewIndex>=views.size())fail("Invalid model buffer view");
        const auto view=views[viewIndex].toObject();
        if(view["buffer"].toInt()!=0)fail("External vertex buffers are unsupported");
        const qint64 stride=view["byteStride"].toInt(width*4);
        const qint64 local=accessor["byteOffset"].toInteger()+qint64(vertex)*stride+component*4;
        const qint64 offset=view["byteOffset"].toInteger()+local;
        if(stride<width*4 || local<0 || local+4>view["byteLength"].toInteger() || offset<0 || offset+4>binary.size())fail("Model attribute exceeds its buffer");
        const auto bits=qFromLittleEndian<quint32>(binary.constData()+offset);float value;std::memcpy(&value,&bits,4);
        if(!std::isfinite(value))fail("Model contains non-finite attributes");
        return value;
    };
    auto indexAt=[&](int index,int item)->unsigned {
        if(index<0)return unsigned(item);
        if(index>=accessors.size())fail("Invalid triangle accessor");
        const auto accessor=accessors[index].toObject();const int type=accessor["componentType"].toInt();
        const int bytes=type==5121?1:type==5123?2:type==5125?4:0;
        if(!bytes || accessor["type"]!="SCALAR" || accessor.contains("sparse"))fail("Unsupported triangle indices");
        const int viewIndex=accessor["bufferView"].toInt(-1);if(viewIndex<0 || viewIndex>=views.size())fail("Invalid index buffer");
        const auto view=views[viewIndex].toObject();const qint64 local=accessor["byteOffset"].toInteger()+qint64(item)*bytes;
        const qint64 offset=view["byteOffset"].toInteger()+local;
        if(view["buffer"].toInt()!=0 || local<0 || local+bytes>view["byteLength"].toInteger() || offset<0 || offset+bytes>binary.size())fail("Triangle indices exceed their buffer");
        const auto* p=reinterpret_cast<const uchar*>(binary.constData()+offset);
        return bytes==1?unsigned(*p):bytes==2?unsigned(qFromLittleEndian<quint16>(p)):qFromLittleEndian<quint32>(p);
    };
    for(int m=0;m<meshes.size();++m) {
        auto mesh=meshes[m].toObject();auto primitives=mesh["primitives"].toArray();
        if(primitives.isEmpty())fail("Model mesh has no primitives");
        for(int p=0;p<primitives.size();++p) {
            auto primitive=primitives[p].toObject();auto attributes=primitive["attributes"].toObject();
            if(!attributes.contains("POSITION") || !attributes.contains("TEXCOORD_0"))fail("Every preview mesh needs UV coordinates (TEXCOORD_0)");
            if(primitive["extensions"].toObject().contains("KHR_draco_mesh_compression"))fail("Use an uncompressed GLB for material preview");
            if(!maps["normal"].toString().isEmpty() && !attributes.contains("TANGENT")) {
                if(!attributes.contains("NORMAL") || primitive["mode"].toInt(4)!=4)fail("Normal-mapped preview needs triangle meshes with normals");
                const int position=attributes["POSITION"].toInt(-1),normal=attributes["NORMAL"].toInt(-1),uv=attributes["TEXCOORD_0"].toInt(-1);
                if(position<0 || position>=accessors.size())fail("Invalid position accessor");
                const int count=accessors[position].toObject()["count"].toInt();
                if(count<1 || count>4000000)fail("Preview model vertex count is unsupported");
                const int indices=primitive["indices"].toInt(-1);
                if(indices>=accessors.size())fail("Invalid index accessor");
                const int indexCount=indices<0?count:accessors[indices].toObject()["count"].toInt();
                if(indexCount<3 || indexCount%3 || indexCount>24000000)fail("Invalid triangle count");
                std::vector<QVector3D> tangent(count),bitangent(count);
                auto point=[&](unsigned i){return QVector3D(number(position,int(i),0,3),number(position,int(i),1,3),number(position,int(i),2,3));};
                for(int i=0;i<indexCount;i+=3) {
                    const auto a=indexAt(indices,i),b=indexAt(indices,i+1),c=indexAt(indices,i+2);
                    if(a>=unsigned(count) || b>=unsigned(count) || c>=unsigned(count))fail("Triangle references an invalid vertex");
                    const auto e1=point(b)-point(a),e2=point(c)-point(a);
                    const float u1=number(uv,int(b),0,2)-number(uv,int(a),0,2),v1=number(uv,int(b),1,2)-number(uv,int(a),1,2);
                    const float u2=number(uv,int(c),0,2)-number(uv,int(a),0,2),v2=number(uv,int(c),1,2)-number(uv,int(a),1,2);
                    const float determinant=u1*v2-u2*v1;
                    if(std::abs(determinant)<1e-12f)continue;
                    const auto t=(e1*v2-e2*v1)/determinant,bn=(e2*u1-e1*u2)/determinant;
                    for(const auto vertex:{a,b,c}){tangent[vertex]+=t;bitangent[vertex]+=bn;}
                }
                while(binary.size()%4)binary.append('\0');
                const auto offset=binary.size();
                for(int i=0;i<count;++i) {
                    const QVector3D n(number(normal,i,0,3),number(normal,i,1,3),number(normal,i,2,3));
                    auto t=tangent[i]-n*QVector3D::dotProduct(n,tangent[i]);
                    if(t.lengthSquared()<1e-12f)t=QVector3D::crossProduct(n,std::abs(n.y())<.9f?QVector3D(0,1,0):QVector3D(1,0,0));
                    t.normalize();const float sign=QVector3D::dotProduct(QVector3D::crossProduct(n,t),bitangent[i])<0?-1.f:1.f;
                    for(float value:{t.x(),t.y(),t.z(),sign}){quint32 bits;std::memcpy(&bits,&value,4);const auto little=qToLittleEndian(bits);binary.append(reinterpret_cast<const char*>(&little),4);}
                }
                views.append(QJsonObject{{"buffer",0},{"byteOffset",offset},{"byteLength",count*16}});
                attributes["TANGENT"]=accessors.size();accessors.append(QJsonObject{{"bufferView",views.size()-1},{"componentType",5126},{"count",count},{"type","VEC4"}});
            }
            // Baked vertex colors belong to the old finish. Keep their source
            // data intact, but do not multiply them into the replacement maps.
            attributes.remove("COLOR_0");
            primitive["attributes"]=attributes;primitive["material"]=materialIndex;primitives[p]=primitive;
        }
        mesh["primitives"]=primitives;meshes[m]=mesh;
    }
    auto images=doc["images"].toArray(),textures=doc["textures"].toArray();
    // Relocated GLB copies still resolve any external images beside the source.
    for(int i=0;i<images.size();++i){auto image=images[i].toObject();const auto uri=image["uri"].toString();if(!uri.isEmpty() && QUrl(uri).isRelative()){image["uri"]=QUrl::fromLocalFile(QFileInfo(path).dir().absoluteFilePath(QUrl::fromPercentEncoding(uri.toUtf8()))).toString(QUrl::FullyEncoded);images[i]=image;}}
    auto texture=[&](const QString& image){const int result=int(textures.size());images.append(QJsonObject{{"uri",QString::fromLatin1(QUrl::toPercentEncoding(QDir(QDir::tempPath()).relativeFilePath(image),"/"))}});textures.append(QJsonObject{{"source",images.size()-1}});return QJsonObject{{"index",result}};};
    QJsonObject pbr{{"baseColorFactor",QJsonArray{1,1,1,1}},{"baseColorTexture",texture(albedo)},{"roughnessFactor",1.0},{"metallicFactor",maps["orm"].toString().isEmpty()?0.0:1.0}};
    QJsonObject assignment{{"name","Live material preview"},{"alphaMode","OPAQUE"},{"doubleSided",true}};
    if(!maps["orm"].toString().isEmpty()){const auto orm=texture(maps["orm"].toString());pbr["metallicRoughnessTexture"]=orm;assignment["occlusionTexture"]=orm;}
    if(!maps["normal"].toString().isEmpty())assignment["normalTexture"]=texture(maps["normal"].toString());
    assignment["pbrMetallicRoughness"]=pbr;auto newMaterials=materials;newMaterials.append(assignment);
    doc["materials"]=newMaterials;doc["images"]=images;doc["textures"]=textures;doc["meshes"]=meshes;doc["accessors"]=accessors;doc["bufferViews"]=views;
    auto buffer=buffers[0].toObject();buffer["byteLength"]=binary.size();buffers[0]=buffer;doc["buffers"]=buffers;
    auto json=QJsonDocument(doc).toJson(QJsonDocument::Compact);while(json.size()%4)json.append(' ');while(binary.size()%4)binary.append('\0');
    QByteArray result;auto word=[&](quint32 value){const auto little=qToLittleEndian(value);result.append(reinterpret_cast<const char*>(&little),4);};
    result.append("glTF",4);word(2);word(quint32(28+json.size()+binary.size()));word(quint32(json.size()));result.append("JSON",4);result+=json;word(quint32(binary.size()));result.append("BIN\0",4);result+=binary;
    return result;
}
}
