#include "vtk_view.h"
#include "../gl_framebuffer.h"
#include "vtk_source.h"
#include "matching.h"
#include "../model_frame.h"
#include <QApplication>
#include <QJsonArray>
#include <QMouseEvent>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QStyleHints>
#include <QWheelEvent>
#include <vtkActor.h>
#include <vtkAutoInit.h>
#include <vtkActorCollection.h>
#include <vtkCallbackCommand.h>
#include <vtkCamera.h>
#include <vtkCellPicker.h>
#include <vtkCommand.h>
#include <vtkDataAssembly.h>
#include <vtkExternalOpenGLRenderWindow.h>
#include <vtkGLTFImporter.h>
#include <vtkLight.h>
#include <vtkLightCollection.h>
#include <vtkLightKit.h>
#include <vtkMapper.h>
#include <vtkMatrix4x4.h>
#include <vtkNew.h>
#include <vtkOpenGLState.h>
#include <vtkPlane.h>
#include <vtkPlaneCollection.h>
#include <vtkPolyDataMapper.h>
#include <vtkProperty.h>
#include <vtkRenderer.h>
#include <vtkSphereSource.h>
#include <vtkSmartPointer.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <numbers>
#include <set>
#include <tuple>
#include <unordered_map>

VTK_MODULE_INIT(vtkRenderingOpenGL2);

namespace {
// VTK error macros can repeat once per malformed primitive. Suppress their
// process-global display only for a synchronous adapter call, then restore it.
// The caller receives one bounded error through the normal load/action path.
struct QuietVtk {
    vtkTypeBool prior=vtkObject::GetGlobalWarningDisplay();
    QuietVtk() { vtkObject::GlobalWarningDisplayOff(); }
    ~QuietVtk() { vtkObject::SetGlobalWarningDisplay(prior); }
};
QJsonArray triple(const double* d) { return {d[0],d[1],d[2]}; }
QJsonObject cameraJson(vtkCamera* camera) {
    return {{"position",triple(camera->GetPosition())},{"focal",triple(camera->GetFocalPoint())},{"up",triple(camera->GetViewUp())},
        {"angle",camera->GetViewAngle()},{"parallel_span",camera->GetParallelProjection()?2*camera->GetParallelScale():2*camera->GetDistance()*std::tan(camera->GetViewAngle()*std::numbers::pi/360)}};
}
bool vector3(const QJsonValue& value) {
    const auto a=value.toArray();
    return a.size()==3 && std::all_of(a.begin(),a.end(),[](const auto& v) { return v.isDouble() && std::isfinite(v.toDouble()); });
}
bool boundedVector3(const QJsonValue& value) {
    const auto a=value.toArray();
    return vector3(value) && std::all_of(a.begin(),a.end(),[](const auto& v) { return std::abs(v.toDouble())<=1e12; });
}
bool validCamera(const QJsonObject& c) {
    if(!boundedVector3(c["position"]) || !boundedVector3(c["focal"]) || !boundedVector3(c["up"])) return false;
    if((c.contains("angle") && !c["angle"].isDouble()) || (c.contains("parallel_span") && !c["parallel_span"].isDouble())) return false;
    double distance=0, up=0;
    for(int i=0;i<3;++i) { distance+=std::pow(c["position"].toArray()[i].toDouble()-c["focal"].toArray()[i].toDouble(),2); up+=std::pow(c["up"].toArray()[i].toDouble(),2); }
    const double angle=c["angle"].toDouble(30), span=c["parallel_span"].toDouble(1);
    return std::isfinite(distance) && distance>1e-18 && std::isfinite(up) && up>1e-18 && std::isfinite(angle) && angle>0 && angle<180 && std::isfinite(span) && span>0 && span<=1e12;
}
QString targetId(const QJsonValue& target) { return target.isObject()?target.toObject()["id"].toString():target.toString(); }
bool targetReference(const QJsonValue& target) { return target.isString() || (target.isObject() && target.toObject()["id"].isString()); }
bool boundedHidden(const QSet<QString>& hidden) {
    if(hidden.size()>8192) return false;
    qsizetype size=0;
    for(const auto& id:hidden) { size+=id.toUtf8().size()+4; if(size>128*1024) return false; }
    return true;
}
bool normalizeCuts(const QJsonArray& cuts,QJsonArray& result,QString& error) {
    if(cuts.size()>3) { error="Inspection view exceeds limits"; return false; }
    QSet<QString> axes;
    for(const auto& value:cuts) {
        const auto cut=value.toObject(); const QString axis=cut["axis"].toString(),keep=cut["keep"].toString("positive");
        if((axis!="x"&&axis!="y"&&axis!="z") || axes.contains(axis) || (keep!="positive"&&keep!="negative") ||
            !cut["position"].isDouble() || !std::isfinite(cut["position"].toDouble()) || std::abs(cut["position"].toDouble())>1e12 ||
            (cut.contains("enabled") && !cut["enabled"].isBool()) || (cut.contains("keep") && !cut["keep"].isString())) {
            error="Invalid clipping plane"; return false;
        }
        axes.insert(axis);
        result.append(QJsonObject{{"axis",axis},{"position",cut["position"]},{"keep",keep},{"enabled",cut["enabled"].toBool(true)}});
    }
    return true;
}
void copyProperty(vtkProperty* destination,vtkProperty* source) {
    // VTK 9.7.1 DeepCopy copies texture pointers without registering them.
    // Use public scalar accessors and SetAllTextures to preserve ownership.
#define COPY_PROPERTY(field) destination->Set##field(source->Get##field())
    COPY_PROPERTY(Ambient); COPY_PROPERTY(AmbientColor); COPY_PROPERTY(Anisotropy); COPY_PROPERTY(AnisotropyRotation);
    COPY_PROPERTY(BackfaceCulling); COPY_PROPERTY(BaseIOR); COPY_PROPERTY(CoatColor); COPY_PROPERTY(CoatIOR);
    COPY_PROPERTY(CoatNormalScale); COPY_PROPERTY(CoatRoughness); COPY_PROPERTY(CoatStrength); COPY_PROPERTY(Color);
    COPY_PROPERTY(Diffuse); COPY_PROPERTY(DiffuseColor); COPY_PROPERTY(EdgeColor); COPY_PROPERTY(EdgeOpacity);
    COPY_PROPERTY(EdgeTint); COPY_PROPERTY(EdgeVisibility); COPY_PROPERTY(EdgeWidth); COPY_PROPERTY(EmissiveFactor);
    COPY_PROPERTY(FrontfaceCulling); COPY_PROPERTY(Interpolation); COPY_PROPERTY(Lighting); COPY_PROPERTY(LineStipplePattern);
    COPY_PROPERTY(LineStippleRepeatFactor); COPY_PROPERTY(LineWidth); COPY_PROPERTY(MaterialName); COPY_PROPERTY(Metallic);
    COPY_PROPERTY(NormalScale); COPY_PROPERTY(OcclusionStrength); COPY_PROPERTY(Opacity); COPY_PROPERTY(Point2DShape);
    COPY_PROPERTY(PointSize); COPY_PROPERTY(RenderLinesAsTubes); COPY_PROPERTY(RenderPointsAsSpheres); COPY_PROPERTY(Representation);
    COPY_PROPERTY(Roughness); COPY_PROPERTY(SelectionColor); COPY_PROPERTY(SelectionLineWidth); COPY_PROPERTY(SelectionPointSize);
    COPY_PROPERTY(Shading); COPY_PROPERTY(ShowTexturesOnBackface); COPY_PROPERTY(Specular); COPY_PROPERTY(SpecularColor);
    COPY_PROPERTY(SpecularPower); COPY_PROPERTY(UseLineWidthForEdgeThickness); COPY_PROPERTY(VertexColor); COPY_PROPERTY(VertexVisibility);
#undef COPY_PROPERTY
    destination->RemoveAllTextures(); destination->SetAllTextures(source->GetAllTextures());
}
}

struct InspectionVtkView::Scene {
    inspection::Source source;
    vtkSmartPointer<vtkExternalOpenGLRenderWindow> window;
    vtkSmartPointer<vtkRenderer> renderer;
    vtkSmartPointer<vtkGLTFImporter> importer;
    vtkSmartPointer<vtkCellPicker> picker;
    vtkSmartPointer<vtkLightKit> lights;
    std::vector<std::pair<vtkSmartPointer<vtkLight>,double>> authoredLights;
    std::map<QString,std::vector<vtkActor*>> actors;
    std::map<vtkActor*,int> actorSources;
    std::map<vtkActor*,vtkSmartPointer<vtkProperty>> originalProperties;
    std::vector<int> parts;
    QSet<QString> hidden;
    std::vector<QSet<QString>> undo;
    QJsonArray cuts;
    std::array<vtkSmartPointer<vtkPlane>,3> planes;
    std::array<bool,3> activeCuts{};
    vtkSmartPointer<vtkPlaneCollection> clippingPlanes;
    QString highlighted;
    std::vector<vtkSmartPointer<vtkActor>> markers;
    QJsonObject home, defaults;
    double bounds[6]{-1,1,-1,1,-1,1};
    int unmapped=0;
    int mappingComparisons=0;
    int mappingStepsRemaining=1000000;
    int cutPlaneUpdates=0,cutMapperBindings=0,selectionActorUpdates=0;

    bool parseHidden(const QJsonArray& values,QSet<QString>& result,QString& error) const {
        if(values.size()>8192) { error="Inspection view exceeds limits"; return false; }
        for(const auto& value:values) {
            const QString id=targetId(value);
            if(!targetReference(value) || !actors.contains(id)) { error="Hidden target is unavailable in this model revision"; return false; }
            result.insert(id);
        }
        if(!boundedHidden(result)) { error="Hidden target state exceeds 128 KiB"; return false; }
        return true;
    }
    void rememberHidden(QSet<QString> prior) {
        undo.push_back(std::move(prior));
        if(undo.size()>100) undo.erase(undo.begin());
    }
    void resetFrame(const QJsonObject& settings,int width,int height) {
        const preview::ModelFrame frame(settings);
        auto* camera=renderer->GetActiveCamera();
        if(!home.isEmpty()) camera->SetViewAngle(home["angle"].toDouble(30));
        const auto direction=frame.direction("isometric");
        double center[3]; for(int i=0;i<3;++i) center[i]=(bounds[i*2]+bounds[i*2+1])*.5;
        camera->SetFocalPoint(center);
        camera->SetPosition(center[0]+direction[0],center[1]+direction[1],center[2]+direction[2]);
        camera->SetViewUp(frame.up[0],frame.up[1],frame.up[2]);
        // Keep the square comparison reference separate from viewport framing.
        window->SetSize(1024,1024); renderer->ResetCamera(bounds); home=cameraJson(camera);
        window->SetSize(width,height); renderer->ResetCamera(bounds); defaults=cameraJson(camera);
    }

    // The hierarchy provides flat_actor_id explicitly. Source matching uses
    // original labels, topology and world matrices, never collection ordering.
    bool map(QString& error) {
        auto* hierarchy=importer->GetSceneHierarchy();
        auto* collection=importer->GetImportedActors();
        if(!hierarchy || !collection) { error="The importer provides no source hierarchy"; return false; }
        auto sourceChildren=[&](int parent) -> const std::vector<int>& { return parent<0?source.roots:source.nodes[parent].children; };
        using Key=std::tuple<QString,int,size_t>;
        using Buckets=std::map<Key,std::vector<int>>;
        struct ImportedNode {
            QString label;
            std::vector<int> children;
            std::vector<vtkActor*> primitives;
            Buckets childGroups;
        };
        std::map<int,ImportedNode> imported;
        std::vector<int> pending{0};
        while(!pending.empty()) {
            const int id=pending.back(); pending.pop_back();
            if(imported.contains(id) || imported.size()>=1000000) { error="Importer hierarchy exceeds limits"; return false; }
            auto& node=imported[id];
            node.label=QString::fromUtf8(hierarchy->GetAttributeOrDefault(id,"label",hierarchy->GetNodeName(id)));
            for(int child:hierarchy->GetChildNodes(id,false)) {
                int actor=-1;
                if(hierarchy->GetAttribute(child,"flat_actor_id",actor)) {
                    if(actor>=0 && actor<collection->GetNumberOfItems())
                        node.primitives.push_back(vtkActor::SafeDownCast(collection->GetItemAsObject(actor)));
                } else { node.children.push_back(child); pending.push_back(child); }
            }
        }
        auto importedKey=[&](int id) { const auto& n=imported.at(id); return Key{n.label,int(n.primitives.size()),n.children.size()}; };
        for(auto& [id,node]:imported) {
            (void)id;
            for(int child:node.children) node.childGroups[importedKey(child)].push_back(child);
        }
        std::map<int,Buckets> sourceBuckets;
        auto buckets=[&](int parent) -> const Buckets& {
            auto [it,inserted]=sourceBuckets.try_emplace(parent);
            if(inserted) for(int id:sourceChildren(parent)) {
                const auto& n=source.nodes[id];
                it->second[{n.hierarchyName,n.primitives,n.children.size()}].push_back(id);
            }
            return it->second;
        };
        // A pair's exact result is reused for topology and both uniqueness
        // checks. Bucket keys only reject candidates; they never prove identity.
        std::unordered_map<uint64_t,bool> memo;
        bool exhausted=false;
        std::function<bool(int,int,int)> matches=[&](int src,int dst,int depth) {
            if(exhausted) return false;
            if(depth>256) { exhausted=true; return false; }
            const uint64_t pair=(uint64_t(uint32_t(src))<<32)|uint32_t(dst);
            if(const auto found=memo.find(pair);found!=memo.end()) return found->second;
            if(++mappingComparisons>1000000) { exhausted=true; return false; }
            const auto& node=source.nodes[src];
            const auto& target=imported.at(dst);
            auto result=[&](bool value) { memo.emplace(pair,value); return value; };
            if(importedKey(dst)!=Key{node.hierarchyName,node.primitives,node.children.size()}) return result(false);
            for(auto* actor:target.primitives) {
                if(!actor) return result(false);
                auto* matrix=actor->GetMatrix();
                for(int i=0;i<4;++i) for(int j=0;j<4;++j) {
                    const double a=node.world[i*4+j], b=matrix->GetElement(i,j);
                    if(!std::isfinite(b) || std::abs(a-b)>1e-6*std::max({1.0,std::abs(a),std::abs(b)})) return result(false);
                }
            }
            const auto& candidates=buckets(src);
            if(candidates.size()!=target.childGroups.size()) return result(false);
            for(const auto& [key,children]:target.childGroups) {
                const auto group=candidates.find(key);
                if(group==candidates.end() || group->second.size()!=children.size()) return result(false);
                const auto match=inspection::matchChildren(group->second,children,[&](int sourceChild,int child) {
                    const bool same=matches(sourceChild,child,depth+1);
                    return exhausted?inspection::MatchResult::Exhausted:same?inspection::MatchResult::Match:inspection::MatchResult::Mismatch;
                },mappingStepsRemaining);
                if(match==inspection::MatchResult::Exhausted) { exhausted=true; return false; }
                if(match==inspection::MatchResult::Mismatch) return result(false);
            }
            return result(true);
        };
        std::set<vtkActor*> assigned;
        bool invalidHierarchy=false;
        std::function<void(int,int)> join=[&](int srcParent,int hierarchyParent) {
            const auto& candidates=buckets(srcParent);
            const auto& dstChildren=imported.at(hierarchyParent).children;
            std::map<int,std::pair<int,int>> forward;
            std::map<int,int> reverse;
            for(int dst:dstChildren) {
                int match=-1, count=0;
                const auto group=candidates.find(importedKey(dst));
                if(group!=candidates.end()) for(int src:group->second) {
                    if(matches(src,dst,0)) { match=src; ++count; ++reverse[src]; }
                    if(exhausted) return;
                }
                forward[dst]={match,count};
            }
            for(int dst:dstChildren) {
                const auto [match,count]=forward.at(dst);
                if(count!=1 || reverse[match]!=1) continue;
                const auto& node=source.nodes[match];
                for(auto* actor:imported.at(dst).primitives) {
                    if(!assigned.insert(actor).second) { error="Importer hierarchy reuses a primitive actor"; invalidHierarchy=true; continue; }
                    actors[node.target["id"].toString()].push_back(actor);
                    actorSources[actor]=match;
                }
                join(match,dst);
                if(exhausted) return;
            }
        };
        join(-1,0);
        if(invalidHierarchy) return false;
        // An incomplete uniqueness check cannot establish any source identity.
        if(exhausted) { actors.clear(); actorSources.clear(); }
        for(int i=0;i<int(source.nodes.size());++i) if(source.nodes[i].primitives>0) {
            parts.push_back(i);
            source.nodes[i].target["mapped"]=actors.contains(source.nodes[i].target["id"].toString());
        }
        unmapped=collection->GetNumberOfItems()-int(actorSources.size());
        collection->InitTraversal();
        while(auto* actor=collection->GetNextActor()) {
            vtkNew<vtkProperty> original; copyProperty(original,actor->GetProperty());
            originalProperties[actor]=original;
        }
        return true;
    }
};

InspectionVtkView::InspectionVtkView(QJsonObject cfg):config(std::move(cfg)) {
    setMinimumSize({100,100}); setCursor(Qt::OpenHandCursor);
    retainedView=config["inspection"].toObject()["view"].toObject();
}
InspectionVtkView::~InspectionVtkView() {
    if(context()) disconnect(context(),nullptr,this,nullptr);
    cleanup();
}
void InspectionVtkView::cleanup() {
    if(scene) {
        retainedView=state();
        if(context()) { makeCurrent(); scene->window->Finalize(); scene.reset(); doneCurrent(); }
        else scene.reset();
    }
    initialized=false;
}
void InspectionVtkView::initializeGL() {
    initialized=true;
    connect(context(),&QOpenGLContext::aboutToBeDestroyed,this,[this] { cleanup(); },Qt::DirectConnection);
    const auto* name=context()->functions()->glGetString(GL_RENDERER);
    rendererName=name?QString::fromUtf8(reinterpret_cast<const char*>(name)):"unknown";
    qInfo("Asset Preview inspection OpenGL renderer: %s",qPrintable(rendererName));
    QString error; const bool ok=load(error);
    if(loaded) loaded(ok,error);
}
bool InspectionVtkView::load(QString& error) {
    QuietVtk quiet;
    try {
        auto next=std::make_unique<Scene>();
        const QString path=config["path"].toString();
        if(!inspection::loadSource(path,next->source,error)) return false;
        next->window=vtkSmartPointer<vtkExternalOpenGLRenderWindow>::New();
        next->window->SetAutomaticWindowPositionAndResize(false);
        next->window->SetUseExternalContent(false);
        next->window->SetFrameBlitModeToBlitToCurrent();
        next->window->SetMultiSamples(0);
        next->window->SetSize(std::max(1,int(width()*devicePixelRatio())),std::max(1,int(height()*devicePixelRatio())));
        next->window->SetMapped(true);
        next->window->SetOwnContext(false);
        next->window->SetIsCurrent(true);
        next->window->SetSupportsOpenGL(1);
        vtkNew<vtkCallbackCommand> callbacks;
        callbacks->SetClientData(this);
        callbacks->SetCallback([](vtkObject*,unsigned long event,void* client,void* data) {
            auto* self=static_cast<InspectionVtkView*>(client);
            // Retain the caller's capture FBO (and Wayland EGL back buffer)
            // when VTK asks to activate the context that is already current.
            if(event==vtkCommand::WindowMakeCurrentEvent && QOpenGLContext::currentContext()!=self->context()) self->makeCurrent();
            if(event==vtkCommand::WindowIsCurrentEvent && data) *static_cast<bool*>(data)=QOpenGLContext::currentContext()==self->context();
            if(event==vtkCommand::WindowSupportsOpenGLEvent && data) *static_cast<int*>(data)=1;
            if(event==vtkCommand::WindowIsDirectEvent && data) *static_cast<int*>(data)=1;
        });
        for(auto event:{vtkCommand::WindowMakeCurrentEvent,vtkCommand::WindowIsCurrentEvent,vtkCommand::WindowSupportsOpenGLEvent,vtkCommand::WindowIsDirectEvent}) next->window->AddObserver(event,callbacks);
        next->window->OpenGLInit();
        next->renderer=vtkSmartPointer<vtkRenderer>::New();
        next->window->AddRenderer(next->renderer);
        next->importer=vtkSmartPointer<vtkGLTFImporter>::New();
        next->importer->SetFileName(path.toUtf8().constData());
        next->importer->SetRenderWindow(next->window);
        if(!next->importer->Update() || next->importer->GetImportedActors()->GetNumberOfItems()==0) { error="glTF importer could not load renderable geometry"; return false; }
        if(!next->map(error)) return false;
        auto* importedLights=next->importer->GetImportedLights();
        importedLights->InitTraversal();
        while(auto* light=importedLights->GetNextItem()) next->authoredLights.emplace_back(light,light->GetIntensity());
        if(!inspection::unchanged(path,next->source,error)) return false;
        next->renderer->ComputeVisiblePropBounds(next->bounds);
        if(next->bounds[0]>next->bounds[1] || !std::all_of(std::begin(next->bounds),std::end(next->bounds),[](double d) { return std::isfinite(d); })) {
            error="The model has no finite visible bounds"; return false;
        }
        next->picker=vtkSmartPointer<vtkCellPicker>::New();
        next->picker->SetTolerance(0.0005);
        next->picker->PickClippingPlanesOff();
        // Cell picking traverses the existing mapper datasets on demand. No
        // second mesh copy or unbounded locator/index is allocated.
        const auto saved=scene?state():retainedView;
        auto previous=std::move(scene);
        scene=std::move(next);
        options();
        scene->resetFrame(config["settings"].toObject(),std::max(1,int(width()*devicePixelRatio())),std::max(1,int(height()*devicePixelRatio())));
        const QString priorRevision=saved["revision"].toString();
        const bool changedRevision=!priorRevision.isEmpty() && (priorRevision!=scene->source.revision ||
            (!saved["dependency_revision"].toString().isEmpty() && saved["dependency_revision"].toString()!=scene->source.dependencyRevision));
        selected.clear(); selectedTarget={}; staleSelection.clear();
        if(changedRevision) {
            staleSelection=saved["selected"].toString();
            if(validCamera(saved["camera"].toObject())) restoreCamera(saved["camera"].toObject());
        } else {
            QString ignored;
            applyView(saved,ignored);
            if(saved["camera"].toObject().isEmpty() && validCamera(config["camera"].toObject())) restoreCamera(config["camera"].toObject());
        }
        if(previous) previous->window->Finalize();
        ++loads; changed(); update();
        return true;
    } catch(const std::exception& e) { error=QString::fromUtf8(e.what()).left(2048); return false; }
}
bool InspectionVtkView::reload(const QJsonObject& cfg,QString& error) {
    config=cfg;
    if(!initialized) { retainedView=config["inspection"].toObject()["view"].toObject(); return true; }
    makeCurrent(); const bool ok=load(error); doneCurrent(); update(); return ok;
}
void InspectionVtkView::settings(const QJsonObject& cfg) {
    const bool changedFrame=preview::frameChanged(config["settings"].toObject(),cfg["settings"].toObject());
    config=cfg;
    if(!scene) return;
    makeCurrent(); options();
    if(changedFrame) scene->resetFrame(config["settings"].toObject(),std::max(1,int(width()*devicePixelRatio())),std::max(1,int(height()*devicePixelRatio())));
    applySelection(); doneCurrent(); update(); rememberCamera();
}
void InspectionVtkView::options() {
    if(!scene) return;
    const auto s=config["settings"].toObject();
    const bool light=s["background"].toString()=="light";
    scene->renderer->SetBackground(light?.8:.075,light?.8:.09,light?.8:.115);
    scene->renderer->GetActiveCamera()->SetParallelProjection(s["orthographic"].toBool());
    for(auto& [actor,original]:scene->originalProperties) {
        copyProperty(actor->GetProperty(),original);
        actor->GetProperty()->SetEdgeVisibility(s["edges"].toBool());
        if(!s["textures"].toBool(true) || !s["materials"].toBool(true)) actor->GetProperty()->RemoveAllTextures();
        if(!s["materials"].toBool(true)) {
            auto* p=actor->GetProperty(); p->SetColor(.65,.67,.70); p->SetOpacity(1); p->SetMetallic(0); p->SetRoughness(.8);
            actor->GetMapper()->ScalarVisibilityOff();
        } else actor->GetMapper()->ScalarVisibilityOn();
    }
    // The material reset above also removes the previous highlight.
    scene->highlighted.clear();
    if(!scene->lights && scene->renderer->GetLights()->GetNumberOfItems()==0) {
        scene->lights=vtkSmartPointer<vtkLightKit>::New();
        scene->lights->AddLightsToRenderer(scene->renderer);
    }
    if(scene->lights) scene->lights->SetKeyLightIntensity(config["settings"].toObject()["light"].toDouble(1.0));
    for(auto& [light,intensity]:scene->authoredLights) light->SetIntensity(intensity*config["settings"].toObject()["light"].toDouble(1.0));
}
void InspectionVtkView::resizeGL(int w,int h) {
    if(scene) scene->window->SetSize(std::max(1,int(w*devicePixelRatio())),std::max(1,int(h*devicePixelRatio())));
}
void InspectionVtkView::paintGL() { paintFrame(defaultFramebufferObject()); }
void InspectionVtkView::paintFrame(GLuint framebuffer) {
    ++renders;
    auto* f=context()->functions();
    f->glBindFramebuffer(GL_FRAMEBUFFER,framebuffer);
    f->glViewport(0,0,int(width()*devicePixelRatio()),int(height()*devicePixelRatio()));
    f->glDisable(GL_SCISSOR_TEST); f->glEnable(GL_DEPTH_TEST); f->glDepthMask(GL_TRUE);
    f->glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
    f->glClearColor(.075f,.09f,.115f,1); f->glClearDepthf(1);
    f->glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT|GL_STENCIL_BUFFER_BIT);
    if(scene) { QuietVtk quiet; scene->window->GetState()->Reset(); scene->window->Render(); }
}
QImage InspectionVtkView::snapshot() {
    return preview::captureFramebuffer(*this,[this](GLuint framebuffer) { paintFrame(framebuffer); });
}
QJsonObject InspectionVtkView::cameraState() const {
    if(!scene) return {};
    return cameraJson(scene->renderer->GetActiveCamera());
}
QJsonObject InspectionVtkView::cameraHome() const { return scene?scene->home:QJsonObject{}; }
void InspectionVtkView::restoreCamera(const QJsonObject& saved) {
    if(!scene || !validCamera(saved)) return;
    auto* c=scene->renderer->GetActiveCamera();
    const auto p=saved["position"].toArray(),f=saved["focal"].toArray(),u=saved["up"].toArray();
    c->SetPosition(p[0].toDouble(),p[1].toDouble(),p[2].toDouble());
    c->SetFocalPoint(f[0].toDouble(),f[1].toDouble(),f[2].toDouble());
    c->SetViewUp(u[0].toDouble(),u[1].toDouble(),u[2].toDouble());
    c->SetViewAngle(saved["angle"].toDouble(30));
    if(saved["parallel_span"].toDouble()>0) c->SetParallelScale(saved["parallel_span"].toDouble()*.5);
    scene->renderer->ResetCameraClippingRange(scene->bounds);
}
void InspectionVtkView::rememberCamera(bool persist) {
    const auto saved=cameraState();
    if(!saved.isEmpty()) { config["camera"]=saved; if(inspectionChanged) inspectionChanged(saved,persist); }
    if(persist) changed();
}
void InspectionVtkView::setCameraState(const QJsonObject& saved) {
    config["camera"]=saved;
    if(scene) { restoreCamera(saved); update(); }
}
void InspectionVtkView::fit() {
    if(!scene) return;
    scene->renderer->ResetCamera(scene->bounds); update(); rememberCamera();
}
void InspectionVtkView::resetView() {
    if(!scene) return;
    restoreCamera(scene->defaults); update(); rememberCamera();
}
void InspectionVtkView::cameraView(const QString& view) {
    if(!scene) return;
    auto* c=scene->renderer->GetActiveCamera();
    const preview::ModelFrame frame(config["settings"].toObject());
    const auto direction=frame.direction(view),up=(view=="top"||view=="bottom")?-frame.front:frame.up;
    const double distance=c->GetDistance(); const auto* focal=c->GetFocalPoint();
    c->SetPosition(focal[0]+direction[0]*distance,focal[1]+direction[1]*distance,focal[2]+direction[2]*distance);
    c->SetViewUp(up[0],up[1],up[2]); scene->renderer->ResetCameraClippingRange(scene->bounds);
    update(); rememberCamera();
}
void InspectionVtkView::orbit(double yawDelta,double pitchDelta) {
    auto* c=scene->renderer->GetActiveCamera();
    const bool zUp=config["settings"].toObject()["up_axis"].toString("y")=="z";
    const int horizontal=zUp?1:2,vertical=zUp?2:1;
    const auto* p=c->GetPosition(); const auto* f=c->GetFocalPoint();
    double d[3]{p[0]-f[0],p[1]-f[1],p[2]-f[2]}; const double radius=c->GetDistance();
    if(radius<1e-9) return;
    const double yaw=std::atan2(d[0],d[horizontal])+yawDelta;
    const double pitch=std::clamp(std::asin(std::clamp(d[vertical]/radius,-1.0,1.0))+pitchDelta,-std::numbers::pi*.49,std::numbers::pi*.49);
    d[0]=radius*std::cos(pitch)*std::sin(yaw); d[horizontal]=radius*std::cos(pitch)*std::cos(yaw); d[vertical]=radius*std::sin(pitch);
    c->SetPosition(f[0]+d[0],f[1]+d[1],f[2]+d[2]); c->SetViewUp(0,zUp?0:1,zUp?1:0);
}
QJsonObject InspectionVtkView::state() const {
    if(!scene) return {{"supported",true},{"loaded",false}};
    QJsonArray hidden,bounds;
    auto hiddenIds=scene->hidden.values(); std::sort(hiddenIds.begin(),hiddenIds.end());
    for(const auto& id:hiddenIds) hidden.append(id);
    for(double d:scene->bounds) bounds.append(d);
    QJsonObject result{{"supported",true},{"loaded",true},{"backend","vtk-gltf"},{"revision",scene->source.revision},
        {"dependency_revision",scene->source.dependencyRevision},{"selected",selected},{"selection",selectedTarget},
        {"camera",cameraState()},{"cuts",scene->cuts},{"hidden",hidden},{"bounds",bounds},{"part_count",int(scene->parts.size())},
        {"orthographic",bool(scene->renderer->GetActiveCamera()->GetParallelProjection())},
        {"mapped_actors",int(scene->actorSources.size())},{"unmapped_actors",scene->unmapped},
        {"capabilities",QJsonObject{{"select",true},{"pick",true},{"hide",!scene->actors.empty()},{"cuts",true},{"pins",true},
            {"identity",scene->unmapped==0?"exact":"partial"},{"isolate",scene->unmapped==0 && !scene->actors.empty()},
            {"grid",false},{"axes",false},{"studio_lighting",false},{"edges",true},{"materials",true},{"textures",true},{"orthographic",true},
            {"transparent_pick","frontmost pickable geometry, transparent surfaces are eligible"},
            {"lighting","authored lights or VTK light kit"},{"picker_index_bytes",0}}}};
    if(!staleSelection.isEmpty()) result["stale_selection"]=staleSelection;
    return result;
}
void InspectionVtkView::changed() { if(inspectionEvent) inspectionEvent(state()); }
QJsonObject InspectionVtkView::metrics() const {
    QJsonObject result{{"loads",loads},{"renders",renders},{"picks",picks},{"engine",bool(scene)},
        {"renderer",rendererName},{"backend","vtk-gltf"},{"picker_index_bytes",0}};
    result["triangles"]=scene && scene->source.triangles?QJsonValue(*scene->source.triangles):QJsonValue(QJsonValue::Null);
    if(scene) {
        result["camera"]=cameraState(); result["parts"]=int(scene->parts.size()); result["unmapped_actors"]=scene->unmapped;
        result["revision"]=scene->source.revision; result["mapping_comparisons"]=scene->mappingComparisons;
        result["mapping_match_steps"]=1000000-scene->mappingStepsRemaining;
        result["cut_plane_updates"]=scene->cutPlaneUpdates; result["cut_mapper_bindings"]=scene->cutMapperBindings;
        result["selection_actor_updates"]=scene->selectionActorUpdates;
    }
    return result;
}
void InspectionVtkView::applyVisibility() {
    for(auto& [id,actors]:scene->actors) for(auto* actor:actors) actor->SetVisibility(!scene->hidden.contains(id));
    for(auto& marker:scene->markers) marker->SetVisibility(true);
    applySelection();
}
void InspectionVtkView::applyCuts() {
    std::array<bool,3> active{};
    bool changed=false;
    for(const auto& value:scene->cuts) {
        const auto cut=value.toObject(); if(!cut["enabled"].toBool(true)) continue;
        const QString axis=cut["axis"].toString(); const int index=axis=="x"?0:axis=="y"?1:2;
        active[index]=true;
        double normal[3]{},origin[3]{}; normal[index]=cut["keep"].toString()=="negative"?-1:1; origin[index]=cut["position"].toDouble();
        auto& plane=scene->planes[index];
        if(!plane) plane=vtkSmartPointer<vtkPlane>::New();
        if(!std::equal(normal,normal+3,plane->GetNormal()) || !std::equal(origin,origin+3,plane->GetOrigin())) {
            plane->SetNormal(normal); plane->SetOrigin(origin); ++scene->cutPlaneUpdates; changed=true;
        }
    }
    if(active!=scene->activeCuts) {
        if(!scene->clippingPlanes) {
            scene->clippingPlanes=vtkSmartPointer<vtkPlaneCollection>::New();
            std::set<vtkMapper*> mappers;
            for(const auto& [actor,original]:scene->originalProperties) {
                (void)original;
                if(auto* mapper=actor->GetMapper();mapper && mappers.insert(mapper).second) {
                    mapper->SetClippingPlanes(scene->clippingPlanes); ++scene->cutMapperBindings;
                }
            }
        }
        scene->clippingPlanes->RemoveAllItems();
        for(int i=0;i<3;++i) if(active[i]) scene->clippingPlanes->AddItem(scene->planes[i]);
        scene->activeCuts=active; changed=true;
    }
    // A collection does not aggregate the modification times of its items.
    if(changed && scene->clippingPlanes) scene->clippingPlanes->Modified();
}
void InspectionVtkView::applySelection() {
    if(!scene || scene->highlighted==selected) return;
    const bool edges=config["settings"].toObject()["edges"].toBool();
    const auto prior=scene->actors.find(scene->highlighted);
    if(prior!=scene->actors.end()) for(auto* actor:prior->second) {
        const auto& original=scene->originalProperties.at(actor);
        auto* p=actor->GetProperty(); p->SetEdgeVisibility(edges); p->SetEdgeColor(original->GetEdgeColor()); p->SetLineWidth(original->GetLineWidth());
        ++scene->selectionActorUpdates;
    }
    if(const auto found=scene->actors.find(selected);found!=scene->actors.end()) for(auto* actor:found->second) {
        actor->GetProperty()->EdgeVisibilityOn(); actor->GetProperty()->SetEdgeColor(1,.65,.15); actor->GetProperty()->SetLineWidth(2);
        ++scene->selectionActorUpdates;
    }
    scene->highlighted=selected;
}
QJsonObject InspectionVtkView::pick(double x,double y,QString& error) {
    if(!scene || !std::isfinite(x) || !std::isfinite(y) || x<0 || y<0 || x>=width() || y>=height()) { error="Pick coordinates must lie on the loaded surface"; return {}; }
    ++picks;
    QuietVtk quiet;
    // Qt is top-left logical pixels. VTK uses bottom-left physical pixels.
    const double ratio=devicePixelRatio();
    const int hit=scene->picker->Pick(x*ratio,(height()-y)*ratio-1,0,scene->renderer);
    selected.clear(); selectedTarget={}; staleSelection.clear();
    if(hit) {
        auto* actor=scene->picker->GetActor();
        if(auto found=scene->actorSources.find(actor);found!=scene->actorSources.end()) {
            selectedTarget=scene->source.nodes[found->second].target;
            selected=selectedTarget["id"].toString();
            selectedTarget["anchor"]=QJsonObject{{"kind","surface_point"},{"position_local",triple(scene->picker->GetMapperPosition())},
                {"normal_local",triple(scene->picker->GetMapperNormal())}};
        } else selectedTarget={{"mapped",false},{"name","Source identity unavailable"},{"position_world",triple(scene->picker->GetPickPosition())}};
    }
    applySelection(); update(); changed(); return state();
}
bool InspectionVtkView::applyView(const QJsonObject& view,QString& error) {
    if(!scene) { error="Inspection is not loaded"; return false; }
    for(const auto& binding:{std::pair{"revision",scene->source.revision},std::pair{"dependency_revision",scene->source.dependencyRevision}}) {
        if(view.contains(binding.first) && (!view[binding.first].isString() || view[binding.first].toString()!=binding.second)) {
            error="Inspection view belongs to a different model or dependency revision"; return false;
        }
    }
    if((view.contains("cuts") && !view["cuts"].isArray()) || (view.contains("hidden") && !view["hidden"].isArray()) ||
        (view.contains("camera") && !view["camera"].isObject()) || (view.contains("selection") && !view["selection"].isObject()) ||
        (view.contains("selected") && !targetReference(view["selected"]))) {
        error="Invalid inspection view field type"; return false;
    }
    const auto cuts=view["cuts"].toArray(), hidden=view["hidden"].toArray();
    QSet<QString> hiddenSet;
    QJsonArray normalizedCuts;
    if(!normalizeCuts(cuts,normalizedCuts,error)) return false;
    if(!scene->parseHidden(hidden,hiddenSet,error)) return false;
    const QString nextSelected=targetId(view["selected"]);
    if(!nextSelected.isEmpty() && !scene->actors.contains(nextSelected)) { error="Selected target is unavailable in this model revision"; return false; }
    const auto camera=view["camera"].toObject();
    if(!camera.isEmpty() && !validCamera(camera)) { error="Invalid inspection camera"; return false; }
    if(view.contains("orthographic") && !view["orthographic"].isBool()) { error="Inspection projection must be boolean"; return false; }
    const auto priorSelection=view["selection"].toObject();
    const auto anchor=priorSelection["anchor"].toObject();
    double normalLength=0; for(const auto& value:anchor["normal_local"].toArray()) normalLength+=std::pow(value.toDouble(),2);
    if(priorSelection.contains("anchor") && (!priorSelection["anchor"].isObject() || !boundedVector3(anchor["position_local"]) ||
        !boundedVector3(anchor["normal_local"]) || !std::isfinite(normalLength) || normalLength<1e-18 || anchor["kind"].toString()!="surface_point")) {
        error="Invalid inspection surface anchor"; return false;
    }
    if(view.contains("orthographic")) {
        auto settings=config["settings"].toObject(); settings["orthographic"]=view["orthographic"];
        config["settings"]=settings;
        scene->renderer->GetActiveCamera()->SetParallelProjection(view["orthographic"].toBool());
    }
    scene->cuts=normalizedCuts; scene->hidden=hiddenSet; selected=nextSelected; selectedTarget={};
    if(!selected.isEmpty()) for(int index:scene->parts) if(scene->source.nodes[index].target["id"].toString()==selected) { selectedTarget=scene->source.nodes[index].target; break; }
    if(!selected.isEmpty() && priorSelection["id"].toString()==selected && !anchor.isEmpty())
        selectedTarget["anchor"]=QJsonObject{{"kind","surface_point"},{"position_local",anchor["position_local"]},{"normal_local",anchor["normal_local"]}};
    applyCuts(); applyVisibility(); if(!camera.isEmpty()) restoreCamera(camera);
    return true;
}
QJsonObject InspectionVtkView::inspect(const QJsonObject& request,QString& error) {
    const QString action=request["action"].toString("state");
    if(action=="state") return state();
    if(!scene) { error="Inspection is not loaded"; return {}; }
    if(action=="parts") {
        const int offset=request["offset"].toInt(0),limit=request["limit"].toInt(100);
        if(offset<0 || limit<1 || limit>100) { error="Parts require offset >= 0 and limit 1 to 100"; return {}; }
        QJsonArray items; qsizetype bytes=0;
        for(int i=offset;i<std::min(qint64(offset)+limit,qint64(scene->parts.size()));++i) {
            const auto target=scene->source.nodes[scene->parts[i]].target;
            const auto size=QJsonDocument(target).toJson(QJsonDocument::Compact).size();
            if(bytes+size>240*1024) break;
            items.append(target); bytes+=size;
        }
        return {{"items",items},{"total",int(scene->parts.size())},{"offset",offset},{"has_more",offset+items.size()<qsizetype(scene->parts.size())},{"revision",scene->source.revision}};
    }
    if(action=="pick") return pick(request["x"].toDouble(-1),request["y"].toDouble(-1),error);
    if(action=="apply") {
        if(!request["view"].isObject() || (request.contains("record_undo") && !request["record_undo"].isBool())) { error="Apply requires an inspection view object and boolean record_undo"; return {}; }
        const auto prior=scene->hidden;
        if(!applyView(request["view"].toObject(),error)) return {};
        if(request["record_undo"].toBool(true)) scene->rememberHidden(prior);
    } else if(action=="commit_hidden") {
        if(!request["hidden"].isArray()) { error="Visibility undo requires a hidden target array"; return {}; }
        QSet<QString> prior;
        if(!scene->parseHidden(request["hidden"].toArray(),prior,error)) return {};
        scene->rememberHidden(std::move(prior));
        return state();
    } else if(action=="select") {
        const QString id=targetId(request["target"]);
        if(!id.isEmpty() && !scene->actors.contains(id)) { error="Target identity is unavailable in this model revision"; return {}; }
        selected=id; selectedTarget={}; staleSelection.clear();
        for(int index:scene->parts) if(scene->source.nodes[index].target["id"].toString()==id) { selectedTarget=scene->source.nodes[index].target; break; }
        applySelection();
    } else if(action=="hide" || action=="show" || action=="isolate") {
        const QString id=request.contains("target")?targetId(request["target"]):selected;
        if(!scene->actors.contains(id)) { error="Select an available component first"; return {}; }
        if(action=="isolate" && scene->unmapped>0) { error="Isolate requires complete source identity mapping"; return {}; }
        auto nextHidden=scene->hidden;
        if(action=="hide") nextHidden.insert(id);
        else if(action=="show") nextHidden.remove(id);
        else { nextHidden.clear(); for(const auto& [other,actors]:scene->actors) { (void)actors; if(other!=id) nextHidden.insert(other); } }
        if(!boundedHidden(nextHidden)) { error="Hidden target state exceeds 128 KiB"; return {}; }
        scene->rememberHidden(scene->hidden);
        scene->hidden=std::move(nextHidden);
        applyVisibility();
    } else if(action=="restore") {
        if(request["all"].toBool()) { scene->hidden.clear(); scene->undo.clear(); }
        else if(request["last"].toBool() && !scene->undo.empty()) { scene->hidden=scene->undo.back(); scene->undo.pop_back(); }
        else if(!request["last"].toBool()) { error="Restore requires all:true or last:true"; return {}; }
        applyVisibility();
    } else if(action=="cut") {
        if((request.contains("enabled") && !request["enabled"].isBool()) || (request.contains("keep") && !request["keep"].isString())) {
            error="Clipping enabled must be boolean and keep must be a string"; return {};
        }
        QJsonArray cuts; const auto axis=request["axis"].toString();
        for(const auto& cut:scene->cuts) if(cut.toObject()["axis"].toString()!=axis) cuts.append(cut);
        cuts.append(QJsonObject{{"axis",axis},{"position",request["position"]},{"keep",request["keep"].toString("positive")},{"enabled",request["enabled"].toBool(true)}});
        QJsonArray normalized;
        if(!normalizeCuts(cuts,normalized,error)) return {};
        scene->cuts=normalized; applyCuts();
    } else if(action=="markers") {
        const auto items=request["items"].toArray();
        if(items.size()>200) { error="At most 200 viewport markers are supported"; return {}; }
        std::vector<std::pair<const inspection::SourceNode*,QJsonArray>> anchors;
        for(const auto& value:items) {
          const auto targets=value.toObject()["targets"].toArray();
          if(targets.size()>16) { error="Each viewport marker may refer to at most 16 targets"; return {}; }
          for(const auto& target:targets) {
            const auto t=target.toObject(),anchor=t["anchor"].toObject();
            if(!vector3(anchor["position_local"])) continue;
            for(int index:scene->parts) if(scene->source.nodes[index].target["id"]==t["id"]) {
                anchors.emplace_back(&scene->source.nodes[index],anchor["position_local"].toArray()); break;
            }
            if(anchors.size()>200) { error="At most 200 viewport pin anchors are supported"; return {}; }
          }
        }
        for(auto& marker:scene->markers) scene->renderer->RemoveActor(marker);
        scene->markers.clear();
        double extent=0; for(int i=0;i<3;++i) extent=std::max(extent,scene->bounds[i*2+1]-scene->bounds[i*2]);
        vtkNew<vtkSphereSource> sphere; sphere->SetRadius(std::max(extent*.006,1e-5)); sphere->SetThetaResolution(10); sphere->SetPhiResolution(8); sphere->Update();
        vtkNew<vtkPolyDataMapper> mapper; mapper->SetInputData(sphere->GetOutput());
        for(const auto& [node,point]:anchors) {
            double world[3]{}; for(int i=0;i<3;++i) { world[i]=node->world[i*4+3]; for(int j=0;j<3;++j) world[i]+=node->world[i*4+j]*point[j].toDouble(); }
            vtkNew<vtkActor> actor; actor->SetMapper(mapper); actor->SetPosition(world); actor->PickableOff(); actor->UseBoundsOff();
            actor->GetProperty()->SetColor(1,.65,.15); actor->GetProperty()->SetAmbient(1); actor->GetProperty()->SetDiffuse(0);
            scene->renderer->AddActor(actor); scene->markers.push_back(actor);
        }
    } else { error="Unknown inspection action"; return {}; }
    update();
    if(action!="markers") changed();
    return state();
}
void InspectionVtkView::wheelEvent(QWheelEvent* e) {
    if(scene) {
        auto* c=scene->renderer->GetActiveCamera(); const double factor=std::pow(1.0015,e->angleDelta().y());
        if(c->GetParallelProjection()) c->SetParallelScale(c->GetParallelScale()/factor); else c->Dolly(factor);
        scene->renderer->ResetCameraClippingRange(scene->bounds); update(); rememberCamera();
    }
    e->accept();
}
void InspectionVtkView::mousePressEvent(QMouseEvent* e) {
    pressed=last=e->position(); dragged=false; doubleClick=false;
    if(activated) activated();
    requestActivate(); e->accept();
}
void InspectionVtkView::mouseMoveEvent(QMouseEvent* e) {
    if(!scene || e->buttons()==Qt::NoButton) { last=e->position(); return; }
    if(!dragged && (e->position()-pressed).manhattanLength()<QApplication::startDragDistance()) return;
    dragged=true;
    const auto delta=e->position()-last; last=e->position(); auto* c=scene->renderer->GetActiveCamera();
    if(e->buttons().testFlag(Qt::MiddleButton) || e->buttons().testFlag(Qt::RightButton) || e->modifiers().testFlag(Qt::ShiftModifier)) {
        const auto* p=c->GetPosition(); const auto* f=c->GetFocalPoint();
        scene->renderer->SetWorldPoint(f[0],f[1],f[2],1); scene->renderer->WorldToDisplay(); const double depth=scene->renderer->GetDisplayPoint()[2];
        scene->renderer->SetDisplayPoint(0,0,depth); scene->renderer->DisplayToWorld();
        double origin[4]; scene->renderer->GetWorldPoint(origin);
        scene->renderer->SetDisplayPoint(delta.x()*devicePixelRatio(),-delta.y()*devicePixelRatio(),depth); scene->renderer->DisplayToWorld();
        double moved[4]; scene->renderer->GetWorldPoint(moved); double shift[3];
        for(int i=0;i<3;++i) shift[i]=origin[i]/origin[3]-moved[i]/moved[3];
        c->SetPosition(p[0]+shift[0],p[1]+shift[1],p[2]+shift[2]); c->SetFocalPoint(f[0]+shift[0],f[1]+shift[1],f[2]+shift[2]);
    } else if(config["settings"].toObject()["lock_horizon"].toBool(true)) orbit(-delta.x()*.4*std::numbers::pi/180,delta.y()*.4*std::numbers::pi/180);
    else { c->Azimuth(-delta.x()*.4); c->Elevation(delta.y()*.4); c->OrthogonalizeViewUp(); }
    scene->renderer->ResetCameraClippingRange(scene->bounds); update(); rememberCamera(false); e->accept();
}
void InspectionVtkView::mouseReleaseEvent(QMouseEvent* e) {
    if(scene && !dragged && !doubleClick && e->button()==Qt::LeftButton && e->modifiers()==Qt::NoModifier) {
        QString ignored; pick(e->position().x(),e->position().y(),ignored);
    } else if(dragged) rememberCamera();
    e->accept();
}
void InspectionVtkView::mouseDoubleClickEvent(QMouseEvent* e) { doubleClick=true; fit(); e->accept(); }

extern "C" Q_DECL_EXPORT ModelHandle asset_preview_create_inspection(QJsonObject config,QWidget* parent,LoadedCallback loaded,InspectionCallback changed,InspectionEventCallback event) {
    auto* view=new InspectionVtkView(std::move(config));
    view->loaded=std::move(loaded); view->inspectionChanged=std::move(changed); view->inspectionEvent=std::move(event);
    auto* surface=QWidget::createWindowContainer(view,parent); surface->setFocusPolicy(Qt::StrongFocus);
    return {surface,view};
}
