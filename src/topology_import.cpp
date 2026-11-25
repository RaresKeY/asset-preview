#include "topology_import.h"
#ifdef ASSET_PREVIEW_GLTF_PIXELS
#include <vtkOBJImporter.h>
#include <vtkPLYReader.h>
#include <vtkActor.h>
#include <vtkActorCollection.h>
#include <vtkCellArray.h>
#include <vtkPolyData.h>
#include <vtkPolyDataMapper.h>
#include <vtkPointData.h>
#include <vtkTriangleFilter.h>
#include <vtkTrivialProducer.h>
#include <stdexcept>

namespace {
thread_local bool triangleEdges=false;
thread_local bool observed=false;
thread_local qint64 triangles=0,quads=0,ngons=0,faces=0;

vtkSmartPointer<vtkPolyData> topology(vtkPolyData* source,bool triangulate) {
    if(!source)return nullptr;
    observed=true;
    auto* cells=source->GetPolys();
    vtkIdType count; const vtkIdType* indices;
    cells->InitTraversal();
    while(cells->GetNextCell(count,indices))if(count>=3) {
        ++faces; triangles+=count-2;
        if(count==4)++quads;
        else if(count>4)++ngons;
    }
    if(!triangulate)return source;
    vtkNew<vtkTriangleFilter> filter; filter->SetInputData(source); filter->Update();
    vtkNew<vtkPolyData> result; result->ShallowCopy(filter->GetOutput());
    // VTK's polygon edge flags suppress tessellation diagonals. Explicit
    // triangle inspection must expose those diagonals instead.
    result->GetPointData()->SetAttribute(nullptr,vtkDataSetAttributes::EDGEFLAG);
    return result;
}

class ObjImporter final : public vtkOBJImporter {
public:
    static ObjImporter* New() { return new ObjImporter; }
    vtkTypeMacro(ObjImporter,vtkOBJImporter);
    bool triangulate=false;
protected:
    void ReadData() override {
        this->Superclass::ReadData();
        auto* actors=this->GetImportedActors(); actors->InitTraversal();
        while(auto* actor=actors->GetNextActor()) {
            auto* mapper=vtkPolyDataMapper::SafeDownCast(actor->GetMapper());
            if(!mapper)continue;
            mapper->Update(); const auto data=topology(mapper->GetInput(),triangulate);
            if(data)mapper->SetInputData(data);
        }
    }
};
class ObjReader final : public f3d::reader {
public:
    const std::string getName() const override {return "asset_preview_obj_topology";}
    const std::string getShortDescription() const override {return "OBJ with source polygons and optional triangulated edges";}
    const std::vector<std::string> getExtensions() const override {return {"obj"};}
    const std::vector<std::string> getMimeTypes() const override {return {"model/obj"};}
    int getScore() const override {return 100;}
    bool canRead(vtkResourceStream* stream) const override {return vtkOBJImporter::CanReadFile(stream);}
    bool hasSceneReader() override {return true;}
    vtkSmartPointer<vtkImporter> createSceneReader(const std::string& path) const override {
        vtkNew<ObjImporter> importer; importer->triangulate=triangleEdges;
        importer->SetFileName(path.c_str()); return importer;
    }
};
class PlyReader final : public f3d::reader {
public:
    const std::string getName() const override {return "asset_preview_ply_topology";}
    const std::string getShortDescription() const override {return "PLY with source polygons and optional triangulated edges";}
    const std::vector<std::string> getExtensions() const override {return {"ply"};}
    const std::vector<std::string> getMimeTypes() const override {return {"application/ply"};}
    int getScore() const override {return 100;}
    bool canRead(vtkResourceStream* stream) const override {return vtkPLYReader::CanReadFile(stream);}
    bool hasGeometryReader() override {return true;}
    vtkSmartPointer<vtkAlgorithm> createGeometryReader(const std::string& path) const override {
        vtkNew<vtkPLYReader> reader; reader->SetFileName(path.c_str()); reader->Update();
        if(reader->GetErrorCode()!=0)throw std::runtime_error("Cannot read PLY polygon data");
        const auto data=topology(reader->GetOutput(),triangleEdges);
        // A finite producer retains the reader's existing arrays. F3D owns
        // its ordinary pipeline after import; there is no second geometry load.
        vtkNew<vtkTrivialProducer> producer;
        producer->SetOutput(data);
        return producer;
    }
};
}
namespace preview {
void prepareTopologyImport(const QString& mode) {
    triangleEdges=mode=="triangles"; observed=false; triangles=quads=ngons=faces=0;
}
QJsonObject topologyImportMetrics() {
    if(!observed)return {{"available",true},{"status","unavailable"}};
    return {{"available",true},{"status","source"},{"faces",faces},{"quads",quads},{"ngons",ngons},
        {"triangles",triangles},{"edge_topology",triangleEdges?"triangles":"source"}};
}
std::vector<std::shared_ptr<f3d::reader>> topologyReaders() {
    return {std::make_shared<ObjReader>(),std::make_shared<PlyReader>()};
}
}
#else
namespace preview {
void prepareTopologyImport(const QString&) {}
QJsonObject topologyImportMetrics() {return {{"available",false},{"status","unavailable"}};}
}
#endif
