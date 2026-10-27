#pragma once
#include <vtkUnsignedCharArray.h>

namespace preview {
// VTK's generic loop recomputes tuple count (including division) per texel.
// Hoist invariant bounds/stride, preserving fill and invalidation semantics.
class GltfPixels final : public vtkUnsignedCharArray {
public:
    static GltfPixels* New() { return new GltfPixels; }
    vtkTypeMacro(GltfPixels,vtkUnsignedCharArray);
    void FillTypedComponent(int component,unsigned char value) override {
        const auto stride=GetNumberOfComponents();
        if(component<0 || component>=stride) {
            Superclass::FillTypedComponent(component,value); return;
        }
        const auto count=GetNumberOfTuples();
        ++fills; texels+=count;
        if(!count) return;
        auto* pixels=GetPointer(0);
        for(vtkIdType i=0;i<count;++i) pixels[i*stride+component]=value;
    }
    vtkIdType fills=0, texels=0;
private:
    GltfPixels()=default;
};
}
