#include "../src/texture_preview.h"
#include <QBuffer>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QTemporaryDir>
#include <QtEndian>
#include <iostream>
#include <stdexcept>

void expect(bool ok,const char* reason) { if(!ok) throw std::runtime_error(reason); }
void write(const QString& path,const QByteArray& bytes) {
    QFile file(path); expect(file.open(QIODevice::WriteOnly),"open test file"); expect(file.write(bytes)==bytes.size(),"write test file");
}
QByteArray read(const QString& path) { QFile file(path); expect(file.open(QIODevice::ReadOnly),"read test file"); return file.readAll(); }
void word(QByteArray& bytes,quint32 number) { auto little=qToLittleEndian(number); bytes.append(reinterpret_cast<const char*>(&little),4); }
QByteArray glb(QJsonObject document,QByteArray binary) {
    auto json=QJsonDocument(document).toJson(QJsonDocument::Compact);
    json.append(QByteArray((4-json.size()%4)%4,' ')); binary.append(QByteArray((4-binary.size()%4)%4,0));
    QByteArray result="glTF"; word(result,2); word(result,28+json.size()+binary.size());
    word(result,json.size()); result+="JSON"; result+=json; word(result,binary.size()); result.append("BIN\0",4); return result+binary;
}
QJsonObject doc(const QByteArray& bytes) { return QJsonDocument::fromJson(bytes.mid(20,qFromLittleEndian<quint32>(bytes.constData()+12))).object(); }
QByteArray bin(const QByteArray& bytes) { return bytes.mid(28+qFromLittleEndian<quint32>(bytes.constData()+12)); }
int check(int argc,char** argv) {
    QCoreApplication app(argc,argv); app.setApplicationName("asset-preview-cache-test");
    QTemporaryDir dir; expect(dir.isValid(),"temporary directory");
    const auto root=dir.filePath("cache"), source=dir.filePath("fixture.glb");
    qputenv("ASSET_PREVIEW_TEXTURE_CACHE",root.toUtf8());
    QImage image(2048,1024,QImage::Format_RGBA8888); image.fill(QColor(30,100,200,128));
    QByteArray png; QBuffer output(&png); output.open(QIODevice::WriteOnly); expect(image.save(&output,"PNG"),"PNG fixture");
    QByteArray binary("geometry01234567"); const auto offset=binary.size(); binary+=png;
    QJsonObject document{{"asset",QJsonObject{{"version","2.0"}}},
        {"buffers",QJsonArray{QJsonObject{{"byteLength",binary.size()}}}},
        {"bufferViews",QJsonArray{QJsonObject{{"buffer",0},{"byteOffset",0},{"byteLength",offset}},QJsonObject{{"buffer",0},{"byteOffset",offset},{"byteLength",png.size()}}}},
        {"images",QJsonArray{QJsonObject{{"bufferView",1},{"mimeType","image/png"}}}},
        {"textures",QJsonArray{QJsonObject{{"source",0}}}},
        {"nodes",QJsonArray{QJsonObject{{"name","authored node"},{"mesh",0}}}},
        {"meshes",QJsonArray{QJsonObject{{"primitives",QJsonArray{QJsonObject{{"attributes",QJsonObject{{"POSITION",0}}}}}}}}},
        {"accessors",QJsonArray{QJsonObject{{"bufferView",0},{"count",1},{"type","VEC3"},{"componentType",5126}}}},
        {"animations",QJsonArray{QJsonObject{{"name","authored animation"},{"samplers",QJsonArray{QJsonObject{{"input",0},{"output",0}}}},
            {"channels",QJsonArray{QJsonObject{{"sampler",0},{"target",QJsonObject{{"node",0},{"path","translation"}}}}}}}}}};
    const auto original=glb(document,binary); write(source,original);
    QString target;
    {
        auto result=preview::texturePreview(source); target=result.path;
        expect(target!=source && result.metrics["status"]=="preview" && !result.metrics["cache_hit"].toBool(),"cold derivative");
        auto content=read(target); const auto derived=doc(content);
        for(const auto* key:{"nodes","meshes","accessors","textures","animations"}) expect(derived[key]==document[key],"authored geometry and references preserved");
        expect(bin(content).startsWith(bin(original)),"complete original binary preserved");
        const auto views=derived["bufferViews"].toArray();
        expect(views.size()==3 && views[0]==document["bufferViews"].toArray()[0] && views[1]==document["bufferViews"].toArray()[1],"original buffer views preserved");
        const auto view=views[2].toObject();
        const auto scaled=QImage::fromData(bin(content).mid(view["byteOffset"].toInt(),view["byteLength"].toInt()),"PNG");
        const auto pixel=scaled.pixelColor(50,50), reference=image.pixelColor(100,100);
        expect(scaled.size()==QSize(1024,512) && pixel.alpha()==reference.alpha() &&
            std::abs(pixel.red()-reference.red())<=1 && std::abs(pixel.green()-reference.green())<=1 && std::abs(pixel.blue()-reference.blue())<=1,
            "bounded texture, channel and alpha preservation within resampling quantization");
        auto busy=preview::texturePreview(source);
        expect(busy.path==source && busy.metrics["reason"]=="Texture cache is busy","busy cache falls back");
    }
    {
        auto hit=preview::texturePreview(source); expect(hit.path==target && hit.metrics["cache_hit"].toBool(),"warm verified hit");
    }
    const auto good=read(target);
    write(target,"corrupt");
    {
        auto repaired=preview::texturePreview(source); expect(repaired.path==target && !repaired.metrics["cache_hit"].toBool() && read(target)==good,"corrupt cache regenerated");
    }
    auto changed=document; changed["nodes"]=QJsonArray{QJsonObject{{"name","changed node"},{"mesh",0}}};
    write(source,glb(changed,binary));
    {
        auto updated=preview::texturePreview(source); expect(updated.path!=target && updated.path!=source,"source bytes invalidate cache");
    }
    write(source,original);
    const auto unrelated=QDir(root).filePath("unrelated.keep"); write(unrelated,"preserve");
    // Sparse task-owned entries make quota and eviction observable without
    // decoding large assets or imposing an unstable time/memory budget.
    for(int i=0;i<4;++i) {
        QFile sparse(QDir(root).filePath("v1-"+QString(64,QChar('0'+i))+".glb"));
        expect(sparse.open(QIODevice::WriteOnly) && sparse.resize(70*1024*1024),"sparse cache fixture");
    }
    {
        auto bounded=preview::texturePreview(source); expect(bounded.path!=source,"cache pruned before verified hit");
    }
    qint64 total=0;
    for(const auto& info:QDir(root).entryInfoList({"v1-*.glb","v1-*.json"},QDir::Files)) total+=info.size();
    expect(total<=256*1024*1024 && read(unrelated)=="preserve","bounded cache preserves unrelated files");
    QFile::remove(target); expect(QFile::link(source,target),"cache symlink fixture");
    {
        auto unsafe=preview::texturePreview(source); expect(unsafe.path==source && read(source)==original,"cache symlinks are never overwritten");
    }
    QFile::remove(target);
    const auto alias=dir.filePath("alias-cache"); expect(QFile::link(root,alias),"directory symlink fixture");
    qputenv("ASSET_PREVIEW_TEXTURE_CACHE",alias.toUtf8());
    {
        auto unsafe=preview::texturePreview(source); expect(unsafe.path==source,"cache root symlink rejected");
    }
    qputenv("ASSET_PREVIEW_TEXTURE_CACHE",(alias+"/").toUtf8());
    expect(preview::texturePreview(source).path==source,"cache root symlink with trailing slash rejected");
    qputenv("ASSET_PREVIEW_TEXTURE_CACHE",root.toUtf8());
    auto external=document; external["images"]=QJsonArray{QJsonObject{{"uri","elsewhere.png"}}}; write(source,glb(external,binary));
    expect(preview::texturePreview(source).path==source,"external dependencies use original location");
    auto extension=document; extension["extensionsUsed"]=QJsonArray{"EXT_unknown"}; write(source,glb(extension,binary));
    expect(preview::texturePreview(source).path==source,"unknown extension uses original");
    extension=document; extension["nodes"]=QJsonArray{QJsonObject{{"extensions",QJsonObject{{"EXT_undeclared",QJsonObject{{"uri","other.bin"}}}}}}};
    write(source,glb(extension,binary)); expect(preview::texturePreview(source).path==source,"undeclared unknown extension uses original");
    auto invalid=document; auto images=invalid["images"].toArray(); images.append(QJsonObject{{"bufferView",999},{"mimeType","image/png"}}); invalid["images"]=images;
    write(source,glb(invalid,binary)); expect(preview::texturePreview(source).path==source,"partly eligible input does not publish a partial derivative");
    for(const auto& bytes:{QByteArray("partial"),original.left(original.size()-4)}) {
        write(source,bytes); expect(preview::texturePreview(source).path==source,"partial source falls back");
    }
    auto imageFixture=[&](const QImage& pixels,const char* format) {
        QByteArray encoded; QBuffer sink(&encoded); sink.open(QIODevice::WriteOnly); expect(pixels.save(&sink,format),"image encoder");
        auto test=document;
        test["buffers"]=QJsonArray{QJsonObject{{"byteLength",encoded.size()}}};
        test["bufferViews"]=QJsonArray{QJsonObject{{"buffer",0},{"byteOffset",0},{"byteLength",encoded.size()}}};
        test["images"]=QJsonArray{QJsonObject{{"bufferView",0},{"mimeType",QString(format)=="JPEG"?"image/jpeg":"image/png"}}};
        return glb(test,encoded);
    };
    QImage transparent(image.size(),QImage::Format_RGBA8888);
    transparent.fill(QColor(128,64,255,0));
    write(source,imageFixture(transparent,"PNG"));
    {
        auto data=preview::texturePreview(source); expect(data.path!=source,"transparent embedded texture derivative");
        const auto bytes=read(data.path); const auto derived=doc(bytes);
        const auto view=derived["bufferViews"].toArray().last().toObject();
        const auto scaled=QImage::fromData(bin(bytes).mid(view["byteOffset"].toInt(),view["byteLength"].toInt()),"PNG");
        expect(scaled.pixelColor(50,50)==transparent.pixelColor(100,100),"RGB data beneath zero alpha survives resizing");
    }
    write(source,imageFixture(image,"JPEG"));
    {
        auto jpeg=preview::texturePreview(source); expect(jpeg.path!=source && jpeg.metrics["status"]=="preview","embedded JPEG derivative");
    }
    QImage small(32,32,QImage::Format_RGB32); small.fill(Qt::red); write(source,imageFixture(small,"PNG"));
    expect(preview::texturePreview(source).path==source,"small textures need no derivative");
    QImage wide(5000,1,QImage::Format_RGB32); wide.fill(Qt::red); write(source,imageFixture(wide,"PNG"));
    expect(preview::texturePreview(source).path==source,"oversized image rejected before decode");
    QImage precise(2048,2,QImage::Format_Grayscale16); precise.fill(Qt::gray); write(source,imageFixture(precise,"PNG"));
    expect(preview::texturePreview(source).path==source,"high precision channels are never silently quantized");
    QFile large(source); expect(large.open(QIODevice::WriteOnly) && large.resize(64*1024*1024+1),"large sparse source"); large.close();
    expect(preview::texturePreview(source).path==source,"oversized source rejected before read");
    write(source,original);
    {
        auto warm=preview::texturePreview(source); expect(warm.path!=source,"prime revision cache");
    }
    for(int i=0;i<4100;++i)
        write(QDir(root).filePath("v1-"+QString("%1").arg(i,64,16,QChar('0'))+".json"),"{}");
    {
        auto warm=preview::texturePreview(source);
        expect(warm.path!=source && warm.metrics["cache_hit"].toBool(),"many small revisions do not disable verified hits");
        expect(QDir(root).entryList({"v1-*.glb","v1-*.json"},QDir::Files).size()<=2048,"cache has a recoverable file-count bound");
        expect(read(unrelated)=="preserve","entry eviction preserves unrelated files");
    }
    std::cout<<"Texture preview integrity, invalidation, quota and fallback checks passed\n";
    return 0;
}
int main(int argc,char** argv) {
    try { return check(argc,argv); }
    catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
