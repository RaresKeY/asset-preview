#include "gltf_import.h"
#include "topology_import.h"

#ifdef ASSET_PREVIEW_GLTF_PIXELS
#include "gltf_pixels.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QtEndian>
#include <f3d/engine.h>
#include <f3d/plugin.h>
#include <f3d/vtk_ext/vtkF3DGLTFImporter.h>
#include <vtkGLTFDocumentLoader.h>
#include <vtkImageData.h>
#include <vtkPointData.h>
#include <vtkCellData.h>
#include <vtkPolyData.h>
#include <vtkDataArray.h>
#include <vtkActor.h>
#include <vtkActorCollection.h>
#include <vtkAppendPolyData.h>
#include <vtkMatrix4x4.h>
#include <vtkPolyDataMapper.h>
#include <vtkRenderer.h>
#include <vtkPNGReader.h>
#include <vtkMemoryResourceStream.h>
#include <vtkExecutive.h>
#include <atomic>
#include <thread>
#include <cstdlib>
#include <memory>
#include <stdexcept>

namespace {
bool available=false;
// Imports run serially on the native render thread. Views snapshot these finite counters
// after add(); they retain no scene/image references and schedule no work.
thread_local vtkIdType arrayViews=0, componentFills=0, filledTexels=0;
thread_local vtkIdType sourceActors=0, renderActors=0;
thread_local QJsonObject imageDecode;

// A finite import-local decode pass. Keep the native decoder and independent
// pixel storage: material setup can overwrite channels of an image afterwards.
#if VTK_VERSION_NUMBER >= VTK_VERSION_CHECK(9, 7, 0)
class GltfLoader final : public vtkGLTFDocumentLoader {
public:
    static GltfLoader* New() { return new GltfLoader; }
    vtkTypeMacro(GltfLoader,vtkGLTFDocumentLoader);
    void PrepareData() override {
        if(!this->GetLoadImages() || std::getenv("ASSET_PREVIEW_DISABLE_GLTF_IMAGE_DECODE"))return;
        const auto model=this->GetInternalModel();
        if(!model || model->Images.size()<16 || model->Images.size()>2048)return;
        QElapsedTimer clock;clock.start();
        struct Job {
            QByteArrayView encoded;
            vtkSmartPointer<vtkPNGReader> reader;
            vtkSmartPointer<vtkImageData> decoded;
        };
        std::vector<Job> jobs;
        std::vector<size_t> indices;
        std::map<QByteArray,std::vector<size_t>> hashes;
        quint64 texels=0;
        for(const auto& image:model->Images) {
            if(image.MimeType!="image/png" || image.BufferView<0 ||
                size_t(image.BufferView)>=model->BufferViews.size())return;
            const auto& view=model->BufferViews[image.BufferView];
            if(view.Buffer<0 || size_t(view.Buffer)>=model->Buffers.size() || view.ByteOffset<0 || view.ByteLength<33)return;
            const auto& buffer=model->Buffers[view.Buffer];
            const size_t offset=view.ByteOffset,length=view.ByteLength;
            if(offset>buffer.size() || length>buffer.size()-offset)return;
            const QByteArrayView bytes(buffer.data()+offset,qsizetype(length));
            if(bytes.first(8)!=QByteArrayView("\x89PNG\r\n\x1a\n",8) || bytes.sliced(12,4)!="IHDR" || bytes[24]!=8)return;
            const auto width=qFromBigEndian<quint32>(bytes.data()+16),height=qFromBigEndian<quint32>(bytes.data()+20);
            if(!width || !height || width>4096 || height>4096)return;
            // VTK's PNG failure/logging path is not safe for a header-only
            // stream on the supported runtime. Reject incomplete chunk framing
            // before entering it; libpng still owns pixel/CRC interpretation.
            bool idat=false,iend=false;
            for(qsizetype cursor=8;cursor<bytes.size();) {
                if(bytes.size()-cursor<12)throw std::runtime_error("Truncated embedded PNG chunk");
                const auto size=qFromBigEndian<quint32>(bytes.data()+cursor);
                if(quint64(size)>quint64(bytes.size()-cursor-12))throw std::runtime_error("Truncated embedded PNG data");
                const auto kind=bytes.sliced(cursor+4,4);
                if(kind=="IDAT")idat=true;
                if(kind=="IEND") { iend=size==0;break; }
                cursor+=qsizetype(size)+12;
            }
            if(!idat || !iend)throw std::runtime_error("Incomplete embedded PNG");
            texels+=quint64(width)*height;
            if(texels>quint64(2)*1024*1024*1024/4)return;
            auto& candidates=hashes[QCryptographicHash::hash(bytes,QCryptographicHash::Sha256)];
            size_t index=jobs.size();
            for(const auto candidate:candidates)if(jobs[candidate].encoded==bytes) { index=candidate;break; }
            if(index==jobs.size()) {
                candidates.push_back(index);
                jobs.push_back({bytes,vtkSmartPointer<vtkPNGReader>::New(),nullptr});
            }
            indices.push_back(index);
        }
        const double prepareMs=clock.nsecsElapsed()/1e6;
        const unsigned workers=std::min<unsigned>(4,std::max(1u,std::thread::hardware_concurrency()));
        std::atomic<size_t> next{0};std::atomic<bool> failed{false};
        {
            std::vector<std::jthread> threads;
            for(unsigned worker=0;worker<std::min<size_t>(workers,jobs.size());++worker)threads.emplace_back([&] {
                try {
                    for(size_t i=next.fetch_add(1);i<jobs.size() && !failed;i=next.fetch_add(1)) {
                        auto& job=jobs[i];
                        vtkNew<vtkMemoryResourceStream> stream;
                        stream->SetBuffer(job.encoded.data(),job.encoded.size());
                        job.reader->SetStream(stream);
                        if(!job.reader->GetExecutive()->Update() || job.reader->GetErrorCode()) { failed=true;break; }
                        job.decoded=job.reader->GetOutput();
                        if(!job.decoded || !job.decoded->GetPointData()->GetScalars()) { failed=true;break; }
                    }
                } catch(...) { failed=true; }
            });
        }
        // A decoder failure is not an eligibility fallback. In particular, do
        // not retry a truncated embedded stream with the native loader's broader
        // buffer range. Propagate through the existing last-good-scene boundary.
        if(failed)throw std::runtime_error("Failed to decode embedded PNG");
        std::vector<vtkSmartPointer<vtkImageData>> images;images.reserve(indices.size());
        std::vector<bool> used(jobs.size(),false);
        for(const auto index:indices) {
            if(!used[index]) { images.push_back(jobs[index].decoded);used[index]=true; }
            else {
                auto copy=vtkSmartPointer<vtkImageData>::New();
                copy->DeepCopy(jobs[index].decoded);images.push_back(copy);
            }
        }
        for(size_t i=0;i<images.size();++i)model->Images[i].ImageData=images[i];
        this->SetLoadImages(false);
        imageDecode={{"images",int(images.size())},{"unique_decodes",int(jobs.size())},
            {"workers",int(std::min<size_t>(workers,jobs.size()))},{"prepare_ms",prepareMs},
            {"total_ms",clock.nsecsElapsed()/1e6}};
    }
};
#endif
bool staticDocument(const char* filename) {
    if(!filename)return false;
    QFile file(QString::fromUtf8(filename));if(!file.open(QIODevice::ReadOnly))return false;
    constexpr qint64 limit=16*1024*1024;
    const auto header=file.read(20);QByteArray json;
    if(header.size()==20 && header.first(4)=="glTF") {
        const auto size=qFromLittleEndian<quint32>(header.constData()+12);
        if(size>limit || header.mid(16,4)!="JSON")return false;
        json=file.read(size);if(json.size()!=size)return false;
    } else {
        if(file.size()>limit || !file.seek(0))return false;
        json=file.readAll();
    }
    const auto doc=QJsonDocument::fromJson(json);if(!doc.isObject())return false;
    const auto object=doc.object();
    // VTK has not loaded animation payloads yet when ImportActors runs; its
    // internal Animations vector can be empty for an animated source here.
    for(const auto* key:{"animations","skins"})
        if(object.contains(key) && (!object[key].isArray() || !object[key].toArray().isEmpty()))return false;
    return true;
}
// Append keeps only attributes common to every input. Never silently drop an
// authored array: partition by exact point/cell schema and active semantics.
QString schema(vtkDataSetAttributes* data) {
    QString result;
    for(int i=0;i<data->GetNumberOfArrays();++i) {
        auto* array=data->GetAbstractArray(i);
        if(!array || !array->GetName())return {};
        result+=QString("%1:%2:%3:%4;").arg(QString::fromUtf8(array->GetName()).toUtf8().toHex())
            .arg(array->GetDataType()).arg(array->GetNumberOfComponents()).arg(data->IsArrayAnAttribute(i));
    }
    return "["+result+"]";
}
class GltfImporter final : public vtkF3DGLTFImporter {
public:
    static GltfImporter* New() { return new GltfImporter; }
    vtkTypeMacro(GltfImporter,vtkF3DGLTFImporter);
protected:
#if VTK_VERSION_NUMBER >= VTK_VERSION_CHECK(9, 7, 0)
    void InitializeLoader() override { this->Loader=vtkSmartPointer<GltfLoader>::New(); }
#endif
    void ImportActors(vtkRenderer* renderer) override {
        const auto model=this->Loader?this->Loader->GetInternalModel():nullptr;
        if(model) for(auto& image:model->Images) {
            if(!image.ImageData) continue;
            auto* data=image.ImageData->GetPointData();
            auto* bytes=vtkUnsignedCharArray::SafeDownCast(data->GetScalars());
            if(!bytes || preview::GltfPixels::SafeDownCast(bytes)) continue;
            vtkNew<preview::GltfPixels> pixels;
            // Public AOS ShallowCopy shares its reference-counted buffer. Only
            // the array wrapper changes; no pixels are copied or decoded.
            pixels->ShallowCopy(bytes); data->SetScalars(pixels); ++arrayViews;
        }
        this->Superclass::ImportActors(renderer);
        sourceActors=this->ActorCollection->GetNumberOfItems();
        // Batch only static, opaque triangle geometry already in world space.
        // Animated/skinned/morphed/transformed data stays on the native path.
        if(model && model->Animations.empty() && model->Skins.empty() && sourceActors>=32 && staticDocument(this->GetFileName()) &&
            !std::getenv("ASSET_PREVIEW_DISABLE_GLTF_BATCHING")) {
            std::map<std::pair<int,QString>,std::vector<vtkActor*>> groups;
            for(const auto& [nodeId,actors]:this->Actors) {
                if(nodeId<0 || nodeId>=int(model->Nodes.size()))continue;
                const auto& node=model->Nodes[nodeId];
                if(node.Mesh<0 || node.Mesh>=int(model->Meshes.size()))continue;
                const auto& primitives=model->Meshes[node.Mesh].Primitives;
                if(primitives.size()!=actors.size())continue;
                for(size_t i=0;i<actors.size();++i) {
                    auto* actor=actors[i].GetPointer();const auto& primitive=primitives[i];
                    auto* mapper=vtkPolyDataMapper::SafeDownCast(actor->GetMapper());
                    auto* geometry=mapper?mapper->GetInput():nullptr;
                    if(primitive.Mode!=4 || !primitive.Targets.empty() || primitive.Material<0 ||
                        primitive.Material>=int(model->Materials.size()) ||
                        model->Materials[primitive.Material].AlphaMode!=vtkGLTFDocumentLoader::Material::AlphaModeType::OPAQUE ||
                        !actor->GetMatrix()->IsIdentity() || !geometry || !geometry->GetPoints() ||
                        geometry->GetPoints()->GetDataType()!=VTK_FLOAT)continue;
                    const auto points=schema(geometry->GetPointData()),cells=schema(geometry->GetCellData());
                    if(points.isEmpty() || cells.isEmpty())continue;
                    groups[{primitive.Material,points+cells}].push_back(actor);
                }
            }
            for(const auto& [material,actors]:groups) {
                (void)material;
                if(actors.size()<2)continue;
                vtkNew<vtkAppendPolyData> append;
                append->SetOutputPointsPrecision(vtkAlgorithm::SINGLE_PRECISION);
                for(auto* actor:actors)append->AddInputData(vtkPolyDataMapper::SafeDownCast(actor->GetMapper())->GetInput());
                append->Update();
                vtkNew<vtkPolyDataMapper> mapper;mapper->ShallowCopy(actors.front()->GetMapper());
                mapper->SetInputData(append->GetOutput());actors.front()->SetMapper(mapper);
                for(size_t i=1;i<actors.size();++i) {
                    renderer->RemoveActor(actors[i]);this->ActorCollection->RemoveItem(actors[i]);
                }
            }
        }
        renderActors=this->ActorCollection->GetNumberOfItems();
        if(model) for(const auto& image:model->Images) {
            const auto* pixels=image.ImageData?preview::GltfPixels::SafeDownCast(image.ImageData->GetPointData()->GetScalars()):nullptr;
            if(pixels) { componentFills+=pixels->fills; filledTexels+=pixels->texels; }
        }
    }
};
// Installed F3D plugin SDK ABI. Native F3D/VTK still owns scene semantics,
// material setup, animations, geometry and armature shaders.
class GltfReader final : public f3d::reader {
public:
    const std::string getName() const override { return "asset_preview_gltf"; }
    const std::string getShortDescription() const override { return "Native glTF with strided byte fills and static material batches"; }
    const std::vector<std::string> getExtensions() const override { return {"glb","gltf"}; }
    const std::vector<std::string> getMimeTypes() const override { return {"model/gltf-binary","model/gltf+json"}; }
    int getScore() const override { return 100; }
    bool canRead(vtkResourceStream* stream) const override { return vtkGLTFImporter::CanReadFile(stream); }
    bool hasSceneReader() override { return true; }
    vtkSmartPointer<vtkImporter> createSceneReader(const std::string& path) const override {
        vtkNew<GltfImporter> importer; importer->SetFileName(path.c_str()); return importer;
    }
};
}

// The existing backend .so is also an F3D reader plugin. No second module,
// global VTK object-factory override, private F3D access or copied importer.
extern "C" __attribute__((visibility("default"))) f3d::plugin* init_plugin() {
    static f3d::plugin plugin("asset_preview_gltf","Native glTF and source polygon inspection","1",[] {
        auto readers=preview::topologyReaders(); readers.push_back(std::make_shared<GltfReader>()); return readers;
    }());
    return &plugin;
}

namespace preview {
void loadGltfImportPlugin() {
    try {
        f3d::engine::loadPlugin((QCoreApplication::applicationDirPath()+"/asset-preview-f3d.so").toStdString());
        available=true;
    } catch(const std::exception& error) {
        qWarning("Native glTF/topology adapters unavailable: %s",error.what());
    }
}
void resetGltfImportMetrics() { arrayViews=0; componentFills=0; filledTexels=0; sourceActors=0; renderActors=0; imageDecode={}; }
QJsonObject gltfImportMetrics() {
    return {{"available",available},{"array_views",arrayViews},{"byte_component_fills",componentFills},{"filled_texels",filledTexels},
        {"source_actors",sourceActors},{"render_actors",renderActors},{"image_decode",imageDecode}};
}
}
#else
namespace preview {
void loadGltfImportPlugin() {}
void resetGltfImportMetrics() {}
QJsonObject gltfImportMetrics() { return {{"available",false}}; }
}
#endif
