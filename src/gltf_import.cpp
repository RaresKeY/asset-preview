#include "gltf_import.h"

#ifdef ASSET_PREVIEW_GLTF_PIXELS
#include "gltf_pixels.h"
#include <QCoreApplication>
#include <f3d/engine.h>
#include <f3d/plugin.h>
#include <f3d/vtk_ext/vtkF3DGLTFImporter.h>
#include <vtkGLTFDocumentLoader.h>
#include <vtkImageData.h>
#include <vtkPointData.h>
#include <memory>
#include <stdexcept>

namespace {
bool available=false;
// Imports run serially on the native render thread. Views snapshot these finite counters
// after add(); they retain no scene/image references and schedule no work.
thread_local vtkIdType arrayViews=0, componentFills=0, filledTexels=0;
class GltfImporter final : public vtkF3DGLTFImporter {
public:
    static GltfImporter* New() { return new GltfImporter; }
    vtkTypeMacro(GltfImporter,vtkF3DGLTFImporter);
protected:
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
    const std::string getShortDescription() const override { return "Native glTF with strided byte fills"; }
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
    static f3d::plugin plugin("asset_preview_gltf","Native glTF byte-fill optimization","1",{std::make_shared<GltfReader>()});
    return &plugin;
}

namespace preview {
void loadGltfImportPlugin() {
    try {
        f3d::engine::loadPlugin((QCoreApplication::applicationDirPath()+"/asset-preview-f3d.so").toStdString());
        available=true;
    } catch(const std::exception& error) {
        qWarning("Native glTF byte-fill optimization unavailable: %s",error.what());
    }
}
void resetGltfImportMetrics() { arrayViews=0; componentFills=0; filledTexels=0; }
QJsonObject gltfImportMetrics() {
    return {{"available",available},{"array_views",arrayViews},{"byte_component_fills",componentFills},{"filled_texels",filledTexels}};
}
}
#else
namespace preview {
void loadGltfImportPlugin() {}
void resetGltfImportMetrics() {}
QJsonObject gltfImportMetrics() { return {{"available",false}}; }
}
#endif
