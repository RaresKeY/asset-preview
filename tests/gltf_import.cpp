#include "gltf_pixels.h"
#include <vtkNew.h>
#include <array>
#include <iostream>
#include <stdexcept>

void expect(bool ok,const char* reason) { if(!ok) throw std::runtime_error(reason); }
int main() {
    try {
        for(int components:{1,2,3,4,8}) for(vtkIdType count:{0,1,17,1024}) {
            vtkNew<vtkUnsignedCharArray> reference;
            vtkNew<preview::GltfPixels> candidate;
            for(auto* array:std::array<vtkUnsignedCharArray*,2>{reference,candidate}) {
                array->SetNumberOfComponents(components); array->SetNumberOfTuples(count);
                for(vtkIdType i=0;i<count*components;++i) array->SetValue(i,static_cast<unsigned char>((i*73+21)%256));
            }
            expect(!count || reference->GetPointer(0)!=candidate->GetPointer(0),"independent oracle buffers");
            for(int component=0;component<components;++component) for(double value:{0.,255.,17.25,129.9}) {
                reference->FillComponent(component,value); candidate->FillComponent(component,value);
                expect(reference->GetNumberOfTuples()==candidate->GetNumberOfTuples(),"tuple count");
                for(vtkIdType i=0;i<count*components;++i)
                    expect(reference->GetValue(i)==candidate->GetValue(i),"exact native byte-fill parity");
            }
            expect(candidate->fills==components*4 && candidate->texels==count*components*4,"finite work counters");
        }
        vtkNew<preview::GltfPixels> pixels;
        unsigned char* address=nullptr;
        {
            vtkNew<vtkUnsignedCharArray> original;
            original->SetNumberOfComponents(4); original->SetNumberOfTuples(23);
            original->Fill(37); address=original->GetPointer(0);
            pixels->ShallowCopy(original);
            expect(pixels->GetPointer(0)==address,"same reference-counted pixel buffer");
            pixels->FillComponent(0,255);
            expect(original->GetValue(0)==255 && original->GetValue(1)==37,"shared pixels preserve other channels");
        }
        expect(pixels->GetPointer(0)==address && pixels->GetValue(88)==255,"buffer survives original wrapper release");
        // Signedness/precision changes never enter this byte-array subclass.
        expect(pixels->GetDataType()==VTK_UNSIGNED_CHAR,"native data type");
        std::cout<<"Exact native fill parity: 20 array shapes, 288 fills; shared-buffer lifetime passed\n";
    } catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
