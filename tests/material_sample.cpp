#include "../src/material_sample.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QTemporaryDir>
#include <iostream>

static void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
static QByteArray fixture(bool uv=true){
    QByteArray binary;QJsonArray views,accessors;
    auto append=[&](const auto& values,int type,const char* shape,int width){const auto offset=binary.size();binary.append(reinterpret_cast<const char*>(values.data()),qsizetype(values.size()*sizeof(values[0])));views.append(QJsonObject{{"buffer",0},{"byteOffset",offset},{"byteLength",binary.size()-offset}});accessors.append(QJsonObject{{"bufferView",views.size()-1},{"componentType",type},{"type",shape},{"count",int(values.size())/width}});};
    append(std::vector<float>{-1,0,-1,1,0,-1,1,0,1,-1,0,1},5126,"VEC3",3);
    append(std::vector<float>{0,1,0,0,1,0,0,1,0,0,1,0},5126,"VEC3",3);
    append(std::vector<float>{0,0,1,0,1,1,0,1},5126,"VEC2",2);
    append(std::vector<quint32>{0,2,1,0,3,2},5125,"SCALAR",1);
    QJsonObject attrs{{"POSITION",0},{"NORMAL",1},{"COLOR_0",1}};if(uv)attrs["TEXCOORD_0"]=2;
    QJsonObject doc{{"asset",QJsonObject{{"version","2.0"}}},{"scene",0},{"scenes",QJsonArray{QJsonObject{{"nodes",QJsonArray{0}}}}},
        {"nodes",QJsonArray{QJsonObject{{"name","Original model"},{"mesh",0},{"translation",QJsonArray{2,0,1}}}}},
        {"meshes",QJsonArray{QJsonObject{{"primitives",QJsonArray{QJsonObject{{"attributes",attrs},{"indices",3},{"material",0}}}}}}},
        {"materials",QJsonArray{QJsonObject{{"name","Original finish"}}}},{"buffers",QJsonArray{QJsonObject{{"byteLength",binary.size()}}}},{"bufferViews",views},{"accessors",accessors}};
    auto json=QJsonDocument(doc).toJson(QJsonDocument::Compact);while(json.size()%4)json.append(' ');
    QByteArray result;auto word=[&](quint32 n){const auto little=qToLittleEndian(n);result.append(reinterpret_cast<const char*>(&little),4);};
    result+="glTF";word(2);word(quint32(28+json.size()+binary.size()));word(quint32(json.size()));result+="JSON";result+=json;word(quint32(binary.size()));result.append("BIN\0",4);result+=binary;return result;
}
static QJsonObject document(const QByteArray& bytes){return QJsonDocument::fromJson(bytes.mid(20,qFromLittleEndian<quint32>(bytes.constData()+12))).object();}
static QByteArray binary(const QByteArray& bytes){return bytes.mid(28+qFromLittleEndian<quint32>(bytes.constData()+12));}
int main(int argc,char** argv){
    QCoreApplication app(argc,argv);
    try{
        QTemporaryDir directory;require(directory.isValid(),"Temporary fixture directory");
        const auto path=directory.filePath("preview model.glb");const auto original=fixture();QFile file(path);require(file.open(QIODevice::WriteOnly),"Write fixture");file.write(original);file.close();
        const auto output=preview::materialOnModel(path,directory.filePath("albedo with space.png"),{{"normal",directory.filePath("normal.png")},{"orm",directory.filePath("orm.png")}});
        const auto source=document(original),derived=document(output);
        require(derived["nodes"]==source["nodes"] && derived["scenes"]==source["scenes"],"Transforms and hierarchy changed");
        require(binary(output).startsWith(binary(original)),"Source geometry bytes changed");
        const auto primitive=derived["meshes"].toArray()[0].toObject()["primitives"].toArray()[0].toObject();
        require(primitive["material"]==1 && primitive["indices"]==3 && primitive["attributes"].toObject()["POSITION"]==0,"Assignment changed original attributes or indices");
        require(!primitive["attributes"].toObject().contains("COLOR_0"),"Original vertex colors tint the new material");
        const int tangent=primitive["attributes"].toObject()["TANGENT"].toInt(-1);require(tangent==4,"Missing generated tangents");
        require(derived["accessors"].toArray()[tangent].toObject()["type"]=="VEC4","Invalid tangent format");
        const auto material=derived["materials"].toArray()[1].toObject();require(material["alphaMode"]=="OPAQUE" && material.contains("normalTexture") && material.contains("occlusionTexture"),"Material maps or opaque depth contract missing");
        const auto uri=derived["images"].toArray()[0].toObject()["uri"].toString();require(uri.contains("%20"),"Image URI must encode spaces");
        require(file.open(QIODevice::ReadOnly) && file.readAll()==original,"Source model was modified");file.close();
        require(file.open(QIODevice::WriteOnly),"Write missing-UV fixture");file.write(fixture(false));file.close();bool rejected=false;
        try{preview::materialOnModel(path,"color.png",{});}catch(const std::exception&){rejected=true;}require(rejected,"Missing UVs accepted");
        require(file.open(QIODevice::WriteOnly),"Write truncated fixture");file.write("glTF");file.close();rejected=false;
        try{preview::materialOnModel(path,"color.png",{});}catch(const std::exception&){rejected=true;}require(rejected,"Truncated GLB accepted");
        std::cout<<"PASS: model material assignment, geometry/transform preservation, UV tangents, map bindings, encoded paths and invalid-model rejection\n";return 0;
    }catch(const std::exception& error){std::cerr<<"FAIL: "<<error.what()<<'\n';return 1;}
}
