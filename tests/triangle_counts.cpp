#include "../src/triangle_counts.h"
#include <QTemporaryDir>
#include <iostream>
#include <stdexcept>

static void expect(bool condition,const char* message) {
    if (!condition) throw std::runtime_error(message);
}

int main() {
    QTemporaryDir dir;
    auto file=[&](const QString& name,const QByteArray& data) {
        const QString path=dir.filePath(name);
        QFile output(path); expect(output.open(QIODevice::WriteOnly),"open fixture");
        expect(output.write(data)==data.size(),"write fixture"); return path;
    };
    const auto obj=file("faces.obj","v 0 0 0\nf 1 2 3\nf 1 2 3 4 # quad\nf 1/1 2/2 3/3 4/4 5/5\nf -4 -3 \\\n -2 -1\n");
    expect(preview::sourceTriangles(obj)==8,"OBJ triangles, quads, polygons, comments and continuation");
    expect(preview::sourceTriangles(file("points.obj","v 0 0 0\nl 1 2\n"))==0,"point/line mesh is zero");
    expect(!preview::sourceTriangles(file("bad.obj","f 1 2 \\\n")),"unfinished face is unavailable");
    const auto json=QByteArray(R"({"asset":{"version":"2.0"},"scene":1,"scenes":[{"nodes":[2]},{"nodes":[0,1]}],
      "nodes":[{"mesh":0},{"mesh":0},{"mesh":1}],"accessors":[{"count":6},{"count":5},{"count":3000}],
      "meshes":[{"primitives":[{"indices":0},{"mode":6,"attributes":{"POSITION":1}},{"mode":1,"indices":0}]},
                  {"primitives":[{"indices":2}]}]})");
    auto doc=QJsonDocument::fromJson(json).object();
    expect(preview::gltfTriangles(doc)==10,"selected scene counts shared mesh instances and ignores unused meshes/lines");
    expect(preview::sourceTriangles(file("scene.gltf",json))==10,"glTF JSON file");
    QByteArray padded=json; while (padded.size()%4) padded+=' ';
    QByteArray glb(20,0); glb.replace(0,4,"glTF"); glb.replace(16,4,"JSON");
    qToLittleEndian<quint32>(2,glb.data()+4);
    qToLittleEndian<quint32>(20+padded.size(),glb.data()+8);
    qToLittleEndian<quint32>(padded.size(),glb.data()+12);
    glb+=padded;
    expect(preview::sourceTriangles(file("scene.GLB",glb))==10,"GLB JSON chunk and case-insensitive suffix");
    auto nodes=doc["nodes"].toArray(); auto node=nodes[0].toObject();
    node["extensions"]=QJsonObject{{"EXT_mesh_gpu_instancing",QJsonObject{{"attributes",QJsonObject{{"TRANSLATION",3}}}}}};
    nodes[0]=node; doc["nodes"]=nodes;
    auto accessors=doc["accessors"].toArray(); accessors.append(QJsonObject{{"count",3}}); doc["accessors"]=accessors;
    expect(preview::gltfTriangles(doc)==20,"explicit GPU instances");
    node.remove("extensions"); node["children"]=QJsonArray{0}; nodes[0]=node; doc["nodes"]=nodes;
    expect(!preview::gltfTriangles(doc),"cyclic nodes are unavailable, not an infinite traversal");
    doc["scenes"]=QJsonArray{QJsonObject{}}; doc["scene"]=0;
    expect(preview::gltfTriangles(doc)==0,"empty scene is zero");
    expect(!preview::sourceTriangles(file("unknown.fbx","anything")),"unknown format is unavailable");
    expect(!preview::sourceTriangles(file("broken.glb",glb.first(24))),"truncated GLB is unavailable");
    expect(!preview::sourceTriangles(file("broken.gltf","{partial")),"partial JSON is unavailable");
    const auto oversized=file("oversized.gltf",json);
    QFile large(oversized); expect(large.open(QIODevice::ReadWrite),"open large metadata fixture");
    expect(large.resize(16*1024*1024+1),"resize large metadata fixture"); large.close();
    expect(!preview::sourceTriangles(oversized),"oversized JSON is unavailable without reading it");
    expect(!preview::sourceTriangles(file("long-line.obj",QByteArray("f ")+QByteArray(1024*1024,'1')+'\n')),
        "oversized OBJ line is unavailable");
    std::cout << "Triangle metadata checks passed\n";
}
