#include "model_view.h"
#include "f3d_compat.h"
#include "gltf_import.h"
#include "animation_time.h"
#include "model_frame.h"
#include "triangle_counts.h"
#include "texture_preview.h"
#include "material_sample.h"
#include <QFileInfo>
#include <QCoreApplication>
#include <QImageReader>
#include <QLibrary>
#include <QJsonArray>
#include <QJsonDocument>
#include <QVector3D>
#include <QTemporaryFile>
#include <QDir>
#include <QUrl>
#include <QMouseEvent>
#include <QPainter>
#include <QScopeGuard>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace {
void loadModelPlugins() {
    static const bool ready=[] {
        f3d::engine::loadPlugin("native"); f3d::engine::loadPlugin("assimp"); preview::loadGltfImportPlugin(); return true;
    }();
    (void)ready;
}
bool isOVoxel(const QString& path) {
    const auto suffix=QFileInfo(path).suffix().toLower();
    return suffix=="vxz" || suffix=="ovoxel";
}
void loadTrellisReader(const QString& source,const QString& mode) {
    if(!isOVoxel(source) && QFileInfo(source).suffix().compare("post",Qt::CaseInsensitive)!=0)return;
#ifdef ASSET_PREVIEW_F3D4
    static bool ready=false;
    if(!ready) {
        const QStringList candidates{QCoreApplication::applicationDirPath()+"/libf3d-plugin-trellispost.so",
            QDir::homePath()+"/.local/lib/f3d/libf3d-plugin-trellispost.so"};
        QString failure;
        for(const auto& candidate:candidates) if(QFileInfo(candidate).isFile()) {
            try {f3d::engine::loadPlugin(candidate.toStdString());ready=true;break;}
            catch(const std::exception& e){failure=QString::fromUtf8(e.what());}
        }
        if(!ready)throw std::runtime_error(("Install the TRELLIS/O-Voxel F3D reader for this F3D/VTK version. "+failure).toStdString());
    }
    if(isOVoxel(source))f3d::engine::setReaderOption("OVoxel.mode",mode.toStdString());
#else
    (void)mode;
    throw std::runtime_error("TRELLIS/O-Voxel previews require F3D 4 and its installed reader plugin");
#endif
}
}

static f3d::mesh_t primitive(const QString& shape) {
    f3d::mesh_t mesh;
    auto vertex = [&](float x, float y, float z, float nx, float ny, float nz, float u, float v) {
        mesh.points.insert(mesh.points.end(), {x, y, z});
        mesh.normals.insert(mesh.normals.end(), {nx, ny, nz});
        mesh.texture_coordinates.insert(mesh.texture_coordinates.end(), {u, v});
    };
    auto quad = [&](unsigned a, unsigned b, unsigned c, unsigned d) {
        mesh.face_sides.insert(mesh.face_sides.end(), {3,3});
        mesh.face_indices.insert(mesh.face_indices.end(), {a,b,c,a,c,d});
    };
    if (shape == "plane") {
        vertex(-1, 0, -1, 0, 1, 0, 0, 0); vertex(-1, 0, 1, 0, 1, 0, 0, 1);
        vertex(1, 0, 1, 0, 1, 0, 1, 1); vertex(1, 0, -1, 0, 1, 0, 1, 0);
        quad(0, 1, 2, 3);
    } else if (shape == "cube") {
        const float faces[6][12] = {
            {-1,-1,1, 1,-1,1, 1,1,1, -1,1,1},
            {1,-1,-1, -1,-1,-1, -1,1,-1, 1,1,-1},
            {1,-1,1, 1,-1,-1, 1,1,-1, 1,1,1},
            {-1,-1,-1, -1,-1,1, -1,1,1, -1,1,-1},
            {-1,1,1, 1,1,1, 1,1,-1, -1,1,-1},
            {-1,-1,-1, 1,-1,-1, 1,-1,1, -1,-1,1}};
        const float normals[6][3] = {{0,0,1},{0,0,-1},{1,0,0},{-1,0,0},{0,1,0},{0,-1,0}};
        for (unsigned f = 0; f < 6; ++f) {
            for (unsigned i = 0; i < 4; ++i)
                vertex(faces[f][i*3], faces[f][i*3+1], faces[f][i*3+2],
                       normals[f][0], normals[f][1], normals[f][2], i == 1 || i == 2, i >= 2);
            quad(f*4, f*4+1, f*4+2, f*4+3);
        }
    } else {
        constexpr unsigned rings = 24, segments = 48;
        for (unsigned r = 0; r <= rings; ++r) {
            const double phi = std::numbers::pi * r / rings;
            for (unsigned s = 0; s <= segments; ++s) {
                const double theta = 2 * std::numbers::pi * s / segments;
                const float x = std::sin(phi) * std::cos(theta), y = std::cos(phi), z = std::sin(phi) * std::sin(theta);
                vertex(x, y, z, x, y, z, float(s)/segments, 1 - float(r)/rings);
            }
        }
        for (unsigned r = 0; r < rings; ++r)
            for (unsigned s = 0; s < segments; ++s) {
                unsigned a = r * (segments+1) + s, b = a + segments+1;
                // At each pole one half of the quad collapses to a point.
                // Keep only the real cap triangle for a well-defined UV frame.
                if (r != 0) {
                    mesh.face_sides.push_back(3);
                    mesh.face_indices.insert(mesh.face_indices.end(), {a,a+1,b+1});
                }
                if (r + 1 != rings) {
                    mesh.face_sides.push_back(3);
                    mesh.face_indices.insert(mesh.face_indices.end(), {a,b+1,b});
                }
            }
    }
    return mesh;
}

// mesh_t has no tangent attribute. Use the standard glTF importer for material
// samples so normal maps receive an explicit, UV-derived tangent frame too.
static QByteArray materialMesh(const f3d::mesh_t& mesh, const QString& albedo) {
    const size_t count=mesh.points.size()/3;
    std::vector<QVector3D> ts(count), bs(count);
    auto point=[&](unsigned i) { return QVector3D(mesh.points[i*3],mesh.points[i*3+1],mesh.points[i*3+2]); };
    for (size_t i=0;i<mesh.face_indices.size();i+=3) {
        const unsigned a=mesh.face_indices[i],b=mesh.face_indices[i+1],c=mesh.face_indices[i+2];
        const auto e1=point(b)-point(a),e2=point(c)-point(a);
        const float u1=mesh.texture_coordinates[b*2]-mesh.texture_coordinates[a*2];
        const float v1=mesh.texture_coordinates[b*2+1]-mesh.texture_coordinates[a*2+1];
        const float u2=mesh.texture_coordinates[c*2]-mesh.texture_coordinates[a*2];
        const float v2=mesh.texture_coordinates[c*2+1]-mesh.texture_coordinates[a*2+1];
        const float det=u1*v2-u2*v1;
        if (std::abs(det)<1e-12f) throw std::runtime_error("Collapsed material UV triangle");
        const auto t=(e1*v2-e2*v1)/det,bit=(e2*u1-e1*u2)/det;
        for (unsigned vertex:{a,b,c}) { ts[vertex]+=t; bs[vertex]+=bit; }
    }
    std::vector<float> tangents;
    for (size_t i=0;i<count;++i) {
        const QVector3D n(mesh.normals[i*3],mesh.normals[i*3+1],mesh.normals[i*3+2]);
        auto t=ts[i]-n*QVector3D::dotProduct(n,ts[i]);
        // Unreferenced duplicated pole vertices still need finite attributes.
        if (t.lengthSquared()<1e-12f) t=QVector3D::crossProduct(n,std::abs(n.y())<.9f?QVector3D(0,1,0):QVector3D(1,0,0));
        t.normalize();
        const float sign=QVector3D::dotProduct(QVector3D::crossProduct(n,t),bs[i])<0?-1.f:1.f;
        tangents.insert(tangents.end(),{t.x(),t.y(),t.z(),sign});
    }
    QByteArray bin;
    QJsonArray views,accessors;
    auto append=[&](const auto& values,int component,const QString& type,int elements) {
        const int bytes=int(values.size()*sizeof(values[0])),offset=int(bin.size());
        bin.append(reinterpret_cast<const char*>(values.data()),bytes);
        views.append(QJsonObject{{"buffer",0},{"byteOffset",offset},{"byteLength",bytes}});
        QJsonObject accessor{{"bufferView",views.size()-1},{"componentType",component},{"count",int(values.size())/elements},{"type",type}};
        if (accessors.isEmpty()) {
            QJsonArray lo,hi;
            for (int axis=0;axis<3;++axis) {
                float lower=mesh.points[axis],upper=lower;
                for(size_t i=axis;i<mesh.points.size();i+=3) { lower=std::min(lower,mesh.points[i]); upper=std::max(upper,mesh.points[i]); }
                lo.append(lower);hi.append(upper);
            }
            accessor["min"]=lo;accessor["max"]=hi;
        }
        accessors.append(accessor);
    };
    append(mesh.points,5126,"VEC3",3);append(mesh.normals,5126,"VEC3",3);
    append(mesh.texture_coordinates,5126,"VEC2",2);append(tangents,5126,"VEC4",4);
    append(mesh.face_indices,5125,"SCALAR",1);
    const QJsonObject primitive{{"attributes",QJsonObject{{"POSITION",0},{"NORMAL",1},{"TEXCOORD_0",2},{"TANGENT",3}}},{"indices",4},{"material",0}};
    QJsonObject doc{{"asset",QJsonObject{{"version","2.0"}}},{"scene",0},
        {"scenes",QJsonArray{QJsonObject{{"nodes",QJsonArray{0}}}}},
        {"nodes",QJsonArray{QJsonObject{{"mesh",0}}}},
        {"meshes",QJsonArray{QJsonObject{{"primitives",QJsonArray{primitive}}}}},
        {"buffers",QJsonArray{QJsonObject{{"byteLength",bin.size()}}}},{"bufferViews",views},{"accessors",accessors},
        // An RGBA override makes F3D force translucency even when alpha is 255.
        // An authored opaque material keeps depth writes and rear faces occluded.
        {"images",QJsonArray{QJsonObject{{"uri",QString::fromLatin1(QUrl::toPercentEncoding(
            QDir(QDir::tempPath()).relativeFilePath(albedo),"/"))}}}},
        {"textures",QJsonArray{QJsonObject{{"source",0}}}},
        {"materials",QJsonArray{QJsonObject{{"alphaMode","OPAQUE"},{"doubleSided",true},
            {"pbrMetallicRoughness",QJsonObject{{"baseColorTexture",QJsonObject{{"index",0}}}}}}}}};
    QByteArray json=QJsonDocument(doc).toJson(QJsonDocument::Compact);
    while(json.size()%4) json.append(' ');
    QByteArray glb;
    auto word=[&](quint32 value) { for(int shift=0;shift<32;shift+=8) glb.append(char(value>>shift)); };
    word(0x46546c67);word(2);word(12+8+json.size()+8+bin.size());
    word(json.size());word(0x4e4f534a);glb.append(json);
    word(bin.size());word(0x004e4942);glb.append(bin);
    return glb;
}

ModelScene::ModelScene(QJsonObject cfg) : config(std::move(cfg)) {
    animationTick.setSingleShot(true); animationTick.setTimerType(Qt::PreciseTimer); animationTick.setInterval(33);
    connect(&animationTick,&QTimer::timeout,this,[this] {
        if (engine && !animation.paused && isExposed()) update();
    });
}
ModelScene::~ModelScene() {
    cleanup();
}
void ModelScene::cleanup() {
    animationTick.stop(); animationClock.invalidate();
    engine.reset();
}
ModelScene::Animation ModelScene::prepareAnimation(f3d::engine& target,double position) {
    Animation next;
    if (config["kind"]!="model") return next;
    auto& scene=target.getScene();
    const auto count=std::min(256u,preview::f3dCompat::animationCount(scene));
    for (unsigned i=0;i<count;++i) {
        auto name=QString::fromStdString(preview::f3dCompat::animationName(scene,i)).left(128);
        next.clips.append(name.isEmpty()?QString("Clip %1").arg(i+1):name);
    }
    if (!count) return next;
    next.index=std::clamp(config["settings"].toObject()["animation"].toInt(),0,int(count)-1);
    target.getOptions().scene.animation.indices={next.index};
    preview::f3dCompat::loadTime(scene,0);
    const auto range=preview::f3dCompat::timeRange(scene);
    if (std::isfinite(range.first) && std::isfinite(range.second) && std::isfinite(range.second-range.first) && range.second>range.first) {
        next.start=range.first; next.duration=range.second-range.first;
    }
    next.position=std::clamp(position,0.0,next.duration);
    next.paused=config["settings"].toObject()["paused"].toBool(true) || next.duration<=0;
    const auto settings=config["settings"].toObject();
    next.sample=preview::animationSample(next.position,next.duration,settings["animation_stepped"].toBool(),settings["animation_fps"].toInt(12));
    preview::f3dCompat::loadTime(scene,next.start+next.sample);
    return next;
}
void ModelScene::restartAnimationClock() {
    animationTick.stop(); animationAnchor=animation.position; animationClockAtPose=0;
    if (!animation.paused && animation.duration>0 && isExposed()) animationClock.start();
    else animationClock.invalidate();
}
void ModelScene::scheduleAnimation() {
    if (!engine || animation.paused || animation.duration<=0 || !isExposed() || !animationClock.isValid()) return;
    const auto settings=config["settings"].toObject();
    int delay=33;
    if (settings["animation_stepped"].toBool()) {
        const double speed=settings["animation_speed"].toDouble(1);
        const double absolute=animationAnchor+animationClock.nsecsElapsed()/1e9*speed;
        // Derive the deadline from the evaluated pose, then subtract render time.
        // If rendering crossed a boundary, wake promptly to catch up in one step.
        const int fps=settings["animation_fps"].toInt(12);
        const double boundary=std::min(animation.duration,(std::floor(preview::animationFrame(animation.position,fps))+1)/fps);
        const double renderElapsed=absolute-(animationAnchor+animationClockAtPose);
        delay=int(std::clamp(std::ceil((boundary-animation.position-renderElapsed)/speed*1000),1.0,10000.0));
    }
    animationTick.start(delay);
}
void ModelScene::notifyPlayback() {
    notificationClock.start();
    if (playbackChanged) playbackChanged();
}
void ModelScene::advanceAnimation() {
    if (!engine || animation.clips.isEmpty() || animation.paused || !animationClock.isValid()) return;
    const auto settings=config["settings"].toObject();
    const double at=animationAnchor+animationClock.nsecsElapsed()/1e9*settings["animation_speed"].toDouble(1);
    animationClockAtPose=at-animationAnchor;
    animation.position=at;
    if (at>=animation.duration) {
        if (settings["loop"].toBool(true)) animation.position=std::fmod(at,animation.duration);
        else {
            animation.position=animation.duration; animation.paused=true; animation.eof=true;
            animationClock.invalidate(); animationTick.stop();
            QTimer::singleShot(0,this,[this] {
                if (animation.eof && animation.paused && playbackSettingsRequested) playbackSettingsRequested({{"paused",true}});
            });
        }
    }
    const double sample=preview::animationSample(animation.position,animation.duration,settings["animation_stepped"].toBool(),settings["animation_fps"].toInt(12));
    if (sample!=animation.sample) {
        preview::f3dCompat::loadTime(engine->getScene(),animation.start+sample);
        animation.sample=sample;
    }
}
void ModelScene::exposed(bool value) {
    visible=value; restartAnimationClock();
    if (visible) update();
}
void ModelScene::viewport(QSize size,double scale) {
    extent=size; ratio=scale;
    if (engine) engine->getWindow().setSize(std::max(1,int(width()*ratio)),std::max(1,int(height()*ratio)));
    update();
}
void ModelScene::update() {
    if (framePending || !visible) return;
    framePending=true;
    QTimer::singleShot(0,this,[this] {
        if (!framePending) return;
        framePending=false;
        if (visible && engine)try { paintGL(); }
        catch(const std::exception& error) {
            animationTick.stop(); animation.paused=true; animationClock.invalidate();
            if(loaded)loaded(false,QString::fromUtf8(error.what()));
        }
    });
}
bool ModelScene::seek(double seconds,QString& error) {
    if (!engine || animation.clips.isEmpty() || animation.duration<=0) { error="No seekable animation in this model"; return false; }
    if (!std::isfinite(seconds) || seconds<0) { error="Seek requires finite nonnegative seconds"; return false; }
    try {
        const double position=std::min(seconds,animation.duration);
        const auto settings=config["settings"].toObject();
        const double sample=preview::animationSample(position,animation.duration,settings["animation_stepped"].toBool(),settings["animation_fps"].toInt(12));
        preview::f3dCompat::loadTime(engine->getScene(),animation.start+sample);
        animation.sample=sample; animation.position=position; animation.eof=false;
    } catch (const std::exception& exception) { error=QString::fromUtf8(exception.what()); return false; }
    restartAnimationClock(); update(); notifyPlayback(); return true;
}
void ModelScene::applyOptions(f3d::engine& e) {
    auto& opt = e.getOptions();
    const auto s = config["settings"].toObject();
    opt.render.background.color = s["background"].toString() == "light"
        ? f3d::color_t{0.8,0.8,0.8} : f3d::color_t{0.075,0.09,0.115};
    if(config["graph_sample"].toBool())opt.render.background.color=f3d::color_t{0.1,0.1,0.1};
    opt.render.grid.enable = s["grid"].toBool(true);
    opt.render.grid.color = f3d::color_t{0.35,0.40,0.45};
    opt.render.show_edges = s["edges"].toBool();
    opt.ui.axis = s["axes"].toBool(false);
    opt.render.light.intensity = s["light"].toDouble(1.0);
    opt.render.hdri.ambient = s["lighting"].toString("studio") == "studio";
    opt.render.effect.tone_mapping = true;
    preview::f3dCompat::antialias(opt);
    opt.render.raytracing.enable = false;
    opt.scene.animation.autoplay = false;
    opt.scene.camera.orthographic = s["orthographic"].toBool();
    opt.scene.up_direction = s["up_axis"].toString("y") == "z"
        ? f3d::direction_t{0,0,1} : f3d::direction_t{0,1,0};
    // Authored properties are restored by a scene reload when a display mode
    // changes: F3D overrides mutate its imported actor properties.
    opt.model = f3d::options{}.model;
#ifdef ASSET_PREVIEW_F3D4
    if(isOVoxel(config["path"].toString())) {
        opt.render.backface_type="visible";
        if(s["materials"].toBool(true))opt.model.coloring="direct";
        const auto mode=s["ovoxel_mode"].toString("surface");
        if(mode=="voxels" || mode=="vertices") {
            opt.model.point_sprites.type="sphere";
            opt.model.point_sprites.absolute_size=true;
            opt.model.point_sprites.size=1;
        }
    }
#endif
    if (config["kind"].toString() == "material") {
        opt.model.color.rgb = f3d::color_t{1,1,1};
        // Albedo is authored into the sample GLB to preserve opaque depth writes.
        const auto maps = config["maps"].toObject();
        if (!maps["normal"].toString().isEmpty()) opt.model.normal.texture = maps["normal"].toString().toStdString();
        if (!maps["orm"].toString().isEmpty()) opt.model.material.texture = maps["orm"].toString().toStdString();
        // ORM contains actual multipliers, not default scaled factors.
        opt.model.material.roughness = maps["orm"].toString().isEmpty() ? s["roughness"].toDouble(0.5) : 1.0;
        opt.model.material.metallic = maps["orm"].toString().isEmpty() ? s["metallic"].toDouble(0.0) : 1.0;
    }
    if (!s["textures"].toBool(true) || !s["materials"].toBool(true)) {
        // A present, empty path explicitly clears a texture in F3D 3.5. An
        // unset optional retains the authored texture instead.
        opt.model.color.texture = std::filesystem::path{};
        opt.model.normal.texture = std::filesystem::path{};
        opt.model.material.texture = std::filesystem::path{};
        opt.model.emissive.texture = std::filesystem::path{};
        opt.model.matcap.texture = std::filesystem::path{};
    }
    if (!s["materials"].toBool(true)) {
        opt.model.color.rgb = f3d::color_t{0.65,0.67,0.70};
        opt.model.color.opacity = 1.0;
        opt.model.material.roughness = 0.8;
        opt.model.material.metallic = 0.0;
        opt.model.emissive.factor = f3d::color_t{0,0,0};
        opt.model.unlit = false;
    }
}
bool ModelScene::load(QString& error,bool retainAnimation) {
    if (!QFileInfo::exists(config["path"].toString())) { error = "Waiting for file"; return false; }
    if (!QFileInfo(config["path"].toString()).isFile()) { error="Waiting for a regular model file"; return false; }
    if (QFileInfo(config["path"].toString()).size()>256*1024*1024) { error="File exceeds 256 MiB"; return false; }
    try {
        QElapsedTimer clock; clock.start();
        QJsonObject timing;
        preview::resetGltfImportMetrics();
        qint64 previous=0;
        auto phase=[&](const char* name) {
            const auto now=clock.nsecsElapsed();
            timing[name]=double(now-previous)/1e6; previous=now;
        };
        // Construct a replacement first: malformed partial saves keep the last good scene.
        auto makeEngine=[] {
#if defined(ASSET_PREVIEW_F3D4) && defined(Q_OS_LINUX)
            // EGL owns no hidden X11 window. A hidden GLX window can still
            // affect compositor input/focus when scenes are reopened.
            try { return std::make_unique<f3d::engine>(f3d::engine::createEGL()); }
            catch(const std::exception& error) { qWarning("EGL preview context unavailable: %s",error.what()); }
#endif
            return std::make_unique<f3d::engine>(f3d::engine::create(true));
        };
        auto next = makeEngine();
        applyOptions(*next);
        phase("engine_options_ms");
        std::optional<qint64> nextTriangles;
        const auto displaySettings=config["settings"].toObject();
        QJsonObject previewMetrics{{"requested",displaySettings["texture_quality"].toString("original")},{"status","original"}};
        if(previewMetrics["requested"]=="preview")
            previewMetrics["reason"]=config["kind"]=="material"?"Material samples use original maps":"Textures are disabled";
        if (config["kind"].toString() == "material") {
            for (const auto& key : {"normal", "orm"}) {
                const QString path = config["maps"].toObject()[key].toString();
                if (!path.isEmpty() && !QFileInfo(path).isFile()) { error = "Waiting for " + path; return false; }
            }
            const auto model=config["sample_model"].toString();
            const auto modelSource=model.isEmpty()?preview::SourceFingerprint{}:preview::sourceFingerprint(model);
            const auto mesh=model.isEmpty()?primitive(config["settings"].toObject()["shape"].toString("sphere")):f3d::mesh_t{};
            const auto glb=model.isEmpty()?materialMesh(mesh,config["path"].toString()):preview::materialOnModel(model,config["path"].toString(),config["maps"].toObject());
            // File-based import also supports the packaged VTK 9.4 backend,
            // which cannot autodetect a glTF memory stream. Removed after load.
            QTemporaryFile sample(QDir::tempPath()+"/asset-preview-material-XXXXXX.glb");
            if (!sample.open() || sample.write(glb)!=glb.size() || !sample.flush())
                throw std::runtime_error("Cannot prepare material sample");
            sample.close();
            phase("preparation_ms");
            next->getScene().add(std::filesystem::path(sample.fileName().toStdString()));
            phase("import_ms");
            if(!model.isEmpty() && preview::sourceFingerprint(model)!=modelSource){error="Model changed during material preview import";return false;}
            if (!thumbnailOnly) nextTriangles=model.isEmpty()?std::optional<qint64>(qint64(mesh.face_sides.size())):triangleMetadata.count(model,modelSource);
            phase("metadata_ms");
        } else {
            const auto source=config["path"].toString();
            const std::filesystem::path path(source.toStdString());
            loadTrellisReader(source,config["settings"].toObject()["ovoxel_mode"].toString("surface"));
            if (!preview::f3dCompat::supports(next->getScene(),path)) { error = "Unsupported model format"; return false; }
            const auto sourceBefore=preview::sourceFingerprint(source);
            const auto settings=config["settings"].toObject();
            preview::TexturePreview derivative;
            if(settings["texture_quality"].toString("original")=="preview" && settings["textures"].toBool(true) && settings["materials"].toBool(true)) {
                derivative=preview::texturePreview(source,cache); previewMetrics=derivative.metrics;
            }
            phase("preparation_ms");
            if(derivative.path.isEmpty() || derivative.path==source) next->getScene().add(path);
            else {
                try { next->getScene().add(std::filesystem::path(derivative.path.toStdString())); }
                catch(const std::exception&) {
                    // Failed cache imports cannot replace the last good view
                    // or prevent the ordinary source loader from recovering.
                    next=makeEngine();
                    applyOptions(*next); next->getScene().add(path);
                    previewMetrics["status"]="original"; previewMetrics["reason"]="Preview import failed; loaded original";
                }
            }
            phase("import_ms");
            if(preview::sourceFingerprint(source)!=sourceBefore) { error="Source changed during model import"; return false; }
            if (!thumbnailOnly) nextTriangles=triangleMetadata.count(source,sourceBefore);
            phase("metadata_ms");
        }
        auto nextAnimation=prepareAnimation(*next,retainAnimation?animation.position:0);
        phase("animation_ms");
        auto& window = next->getWindow();
        const auto saved=resetCameraFrame?QJsonObject{}:(engine?cameraState():config["camera"].toObject());
        // A square fit gives comparison a stable scale independent of each
        // card's viewport aspect ratio and when it was first opened.
        window.setSize(1024,1024);
        if (config["settings"].toObject().contains("front_axis")) {
            const preview::ModelFrame frame(config["settings"].toObject());
            auto state=window.getCamera().getState();
            const auto direction=frame.direction("isometric");
            for(int i=0;i<3;++i) { state.position[i]=state.focalPoint[i]+direction[i]; state.viewUp[i]=frame.up[i]; }
            window.getCamera().setState(state).resetToBounds();
        } else window.getCamera().azimuth(30).elevation(20).resetToBounds();
        referenceCamera=window.getCamera().getState();
        window.setSize(std::max(1, int(width()*devicePixelRatioF())), std::max(1, int(height()*devicePixelRatioF())));
        window.getCamera().resetToBounds();
        defaultCamera=window.getCamera().getState();
        engine = std::move(next);
        animation=std::move(nextAnimation); restartAnimationClock();
        if (!saved.isEmpty()) restoreCamera(saved);
        triangles=nextTriangles;
        texturePreviewMetrics=previewMetrics;
        gltfImport=preview::gltfImportMetrics();
        publishedMaterialModel=config["kind"]=="material"?config["sample_model"].toString():QString();
        if (saved.isEmpty() && config["settings"].toObject()["lock_horizon"].toBool(true)) orbit(0,0);
        resetCameraFrame=false;
        ++loads;
        phase("camera_publish_ms");
        timing["total_ms"]=double(clock.nsecsElapsed())/1e6;
        loadTiming=timing; loadClock=clock; firstFramePending=true;
        return true;
    } catch (const std::exception& e) { error = QString::fromUtf8(e.what()); return false; }
}
bool ModelScene::reload(const QJsonObject& cfg, QString& error) {
    pendingLoadNotice=false;
    resetCameraFrame=preview::frameChanged(config["settings"].toObject(),cfg["settings"].toObject());
    config = cfg;
    try { loadModelPlugins(); }
    catch (const std::exception& failure) { error=QString::fromUtf8(failure.what()); return false; }
    const bool ok=load(error); update(); return ok;
}
void ModelScene::settings(const QJsonObject& cfg) {
    const auto old=config["settings"].toObject(), next=cfg["settings"].toObject();
    resetCameraFrame=preview::frameChanged(old,next);
    const bool clipChanged=next["animation"].toInt()!=old["animation"].toInt() ||
        (engine && !animation.clips.isEmpty() && std::clamp(next["animation"].toInt(),0,int(animation.clips.size())-1)!=animation.index);
    // The installed glTF importer disables old clips without resetting their
    // unkeyed transforms. A fresh candidate restores the authored rest state.
    const bool reloadScene = clipChanged || resetCameraFrame || next["shape"] != old["shape"] || next["up_axis"] != old["up_axis"] ||
        next["materials"].toBool(true) != old["materials"].toBool(true) ||
        next["textures"].toBool(true) != old["textures"].toBool(true) ||
        next["texture_quality"].toString("original") != old["texture_quality"].toString("original") ||
        next["ovoxel_mode"].toString("surface") != old["ovoxel_mode"].toString("surface");
    if (engine) { advanceAnimation(); }
    config = cfg;
    auto oldRendering=old, nextRendering=next;
    for (const auto* key:{"triangles","paused","loop","animation","animation_speed","animation_stepped","animation_fps"}) {
        oldRendering.remove(key); nextRendering.remove(key);
    }
    const bool transportChanged=old["paused"]!=next["paused"] || old["loop"]!=next["loop"] ||
        old["animation"]!=next["animation"] || old["animation_speed"]!=next["animation_speed"] ||
        old["animation_stepped"]!=next["animation_stepped"] || old["animation_fps"]!=next["animation_fps"] ||
        (animation.eof && !next["paused"].toBool(true));
    if (oldRendering==nextRendering && !transportChanged && !clipChanged) return;
    if (engine) {
        if (reloadScene) {
            QString error; const bool ok=load(error,!clipChanged);
            if(ok)pendingLoadNotice=true; else if(loaded)loaded(false,error);
        }
        else {
            if (oldRendering!=nextRendering) applyOptions(*engine);
            if (transportChanged) {
                const bool reset=old["animation"]!=next["animation"] || (animation.eof && !next["paused"].toBool(true));
                const bool eof=animation.eof;
                animation=prepareAnimation(*engine,reset?0:animation.position);
                if (!reset) animation.eof=eof;
                restartAnimationClock();
            }
        }
        if (next["lock_horizon"].toBool(true) && (old["lock_horizon"]!=next["lock_horizon"] || old["up_axis"]!=next["up_axis"])) orbit(0,0);
        update();
        if (oldRendering!=nextRendering) rememberCamera();
        notifyPlayback();
    }
}
void ModelScene::fit() {
    if (engine) { engine->getWindow().getCamera().resetToBounds(); update(); }
    rememberCamera();
}
double ModelScene::parallelSpan() const {
    auto& window=engine->getWindow();
    auto display=window.getDisplayFromWorld(window.getCamera().getFocalPoint());
    display[1]=0; const auto bottom=window.getWorldFromDisplay(display);
    display[1]=height()*devicePixelRatioF(); const auto top=window.getWorldFromDisplay(display);
    return std::sqrt(std::pow(top[0]-bottom[0],2)+std::pow(top[1]-bottom[1],2)+std::pow(top[2]-bottom[2],2));
}
QJsonObject ModelScene::cameraState() const {
    if (!engine) return {};
    const auto c=engine->getWindow().getCamera().getState();
    return {{"position",QJsonArray{c.position[0],c.position[1],c.position[2]}},
        {"focal",QJsonArray{c.focalPoint[0],c.focalPoint[1],c.focalPoint[2]}},
        {"up",QJsonArray{c.viewUp[0],c.viewUp[1],c.viewUp[2]}},
        {"angle",c.viewAngle},{"parallel_span",parallelSpan()}};
}
void ModelScene::restoreCamera(const QJsonObject& saved) {
    auto& camera=engine->getWindow().getCamera();
    auto state=camera.getState();
    const auto p=saved["position"].toArray(), f=saved["focal"].toArray(), u=saved["up"].toArray();
    for (int i=0;i<3;++i) { state.position[i]=p[i].toDouble(); state.focalPoint[i]=f[i].toDouble(); state.viewUp[i]=u[i].toDouble(); }
    state.viewAngle=saved["angle"].toDouble(30); camera.setState(state);
    const double span=saved["parallel_span"].toDouble();
    if (config["settings"].toObject()["orthographic"].toBool() && span>0) camera.zoom(parallelSpan()/span);
}
QJsonObject ModelScene::cameraHome() const {
    const auto& c=referenceCamera;
    return {{"position",QJsonArray{c.position[0],c.position[1],c.position[2]}},
        {"focal",QJsonArray{c.focalPoint[0],c.focalPoint[1],c.focalPoint[2]}}};
}
void ModelScene::setCameraState(const QJsonObject& state) {
    config["camera"]=state;
    if (!engine) return;
    restoreCamera(state); update();
}
void ModelScene::cameraView(const QString& view) {
    if (!engine) return;
    auto& camera=engine->getWindow().getCamera();
    auto state=camera.getState();
    double distance=0; for (int i=0;i<3;++i) distance+=std::pow(state.position[i]-state.focalPoint[i],2);
    distance=std::sqrt(distance);
    const preview::ModelFrame frame(config["settings"].toObject());
    const auto direction=frame.direction(view);
    const auto up=(view=="top" || view=="bottom")?-frame.front:frame.up;
    for(int i=0;i<3;++i) state.viewUp[i]=up[i];
    for (int i=0;i<3;++i) state.position[i]=state.focalPoint[i]+direction[i]*distance;
    camera.setState(state); update(); rememberCamera();
}
void ModelScene::rememberCamera(bool persist) {
    const auto saved=cameraState();
    if (!saved.isEmpty()) { config["camera"]=saved; if (inspectionChanged) inspectionChanged(saved,persist); }
}
void ModelScene::resetView() {
    if (!engine) return;
    auto& camera=engine->getWindow().getCamera();
    camera.setState(defaultCamera).resetToBounds();
    if (config["settings"].toObject()["lock_horizon"].toBool(true)) orbit(0,0);
    update(); rememberCamera();
}
QJsonObject ModelScene::metrics() const {
    QJsonObject result{{"loads", loads}, {"renders", renders}, {"engine", bool(engine)}, {"renderer", renderer},{"render_pending",framePending}};
    result["triangles"]=engine && triangles ? QJsonValue(*triangles) : QJsonValue(QJsonValue::Null);
    if(config["kind"]=="material")result["material_model"]=publishedMaterialModel;
    if (engine) {
        result["texture_preview"]=texturePreviewMetrics;
        result["load_timing"]=loadTiming;
        result["gltf_import"]=gltfImport;
        result["animations"]=QJsonArray::fromStringList(animation.clips);
        if (!animation.clips.isEmpty()) {
            result["animation"]=animation.index; result["duration"]=animation.duration;
            result["position"]=animation.position; result["paused"]=animation.paused;
            result["sample_position"]=animation.sample;
            result["animation_stepped"]=config["settings"].toObject()["animation_stepped"].toBool();
            result["animation_fps"]=config["settings"].toObject()["animation_fps"].toInt(12);
            result["eof"]=animation.eof; result["seekable"]=animation.duration>0;
            result["animation_timer"]=animationTick.isActive();
        }
        result["camera"]=cameraState();
        result["lighting"]=config["settings"].toObject()["lighting"].toString("studio");
        result["lights"]=engine->getScene().getLightCount();
    }
    return result;
}
f3d::vector3_t ModelScene::worldUp() const {
    return config["settings"].toObject()["up_axis"].toString("y") == "z"
        ? f3d::vector3_t{0,0,1} : f3d::vector3_t{0,1,0};
}
void ModelScene::orbit(double yawDelta, double pitchDelta) {
    auto& camera=engine->getWindow().getCamera();
    auto state=camera.getState();
    const bool zUp=config["settings"].toObject()["up_axis"].toString("y") == "z";
    const int horizontal=zUp?1:2, vertical=zUp?2:1;
    double d[3]; for (int i=0;i<3;++i) d[i]=state.position[i]-state.focalPoint[i];
    const double radius=std::sqrt(d[0]*d[0]+d[1]*d[1]+d[2]*d[2]);
    if (radius<1e-9) return;
    double yaw=std::atan2(d[0],d[horizontal])+yawDelta;
    double pitch=std::clamp(std::asin(std::clamp(d[vertical]/radius,-1.0,1.0))+pitchDelta,
        -std::numbers::pi*0.49,std::numbers::pi*0.49);
    d[0]=radius*std::cos(pitch)*std::sin(yaw);
    d[horizontal]=radius*std::cos(pitch)*std::cos(yaw);
    d[vertical]=radius*std::sin(pitch);
    for (int i=0;i<3;++i) state.position[i]=state.focalPoint[i]+d[i];
    state.viewUp=worldUp(); camera.setState(state);
}
void ModelScene::paintGL() {
    QElapsedTimer frameClock; frameClock.start();
    ++renders;
    try { advanceAnimation(); }
    catch (const std::exception& error) {
        animation.paused=true; animationTick.stop(); animationClock.invalidate();
        qWarning("Animation playback stopped: %s",error.what());
    }
    QImage pixels;
    if (engine) {
        const auto frame=engine->getWindow().renderToImage();
        const auto channels=frame.getChannelCount();
        if (frame.getChannelType()==f3d::image::ChannelType::BYTE && (channels==3 || channels==4))
        {
            const QImage view(static_cast<const uchar*>(frame.getContent()),frame.getWidth(),frame.getHeight(),
                frame.getWidth()*channels,channels==4?QImage::Format_RGBA8888:QImage::Format_RGB888);
#if QT_VERSION >= QT_VERSION_CHECK(6,9,0)
            pixels=view.flipped(Qt::Vertical);
#else
            pixels=view.mirrored();
#endif
        }
        using GetString=const unsigned char* (*)(unsigned int);
        static const auto getString=reinterpret_cast<GetString>(QLibrary::resolve("libGL.so.1","glGetString"));
        const auto* name=getString?getString(0x1F01):nullptr;
        renderer=name?QString::fromUtf8(reinterpret_cast<const char*>(name)):QString("unknown");
    }
    if(engine && firstFramePending) {
        loadTiming["first_render_ms"]=double(frameClock.nsecsElapsed())/1e6;
        loadTiming["load_to_first_render_ms"]=double(loadClock.nsecsElapsed())/1e6;
        firstFramePending=false;
    }
    if (frameReady && !pixels.isNull()) frameReady(std::move(pixels));
    if(pendingLoadNotice) { pendingLoadNotice=false; if(loaded)loaded(true,{}); }
    scheduleAnimation();
    if (!animation.clips.isEmpty() && (!notificationClock.isValid() || notificationClock.elapsed()>=100 || animation.eof))
        notifyPlayback();
}
QImage ModelScene::snapshot() {
    QImage image;
    auto callback=std::move(frameReady);
    const auto restore=qScopeGuard([&] { frameReady=std::move(callback); });
    frameReady=[&image](QImage frame) { image=std::move(frame); };
    framePending=false;
    if (engine) paintGL();
    return image;
}
QImage ModelScene::thumbnail(QJsonObject config,QString& error,QString& renderer) {
    loadModelPlugins();
    auto settings=config["settings"].toObject(); settings["paused"]=true; config["settings"]=settings;
    ModelScene model(std::move(config)); model.thumbnailOnly=true; model.viewport({256,160},1);
    if (!model.load(error)) return {};
    const auto frame=model.engine->getWindow().renderToImage();
    using GetString=const unsigned char* (*)(unsigned int);
    const auto getString=reinterpret_cast<GetString>(QLibrary::resolve("libGL.so.1","glGetString"));
    const auto* name=getString?getString(0x1F01):nullptr;
    renderer=name?QString::fromUtf8(reinterpret_cast<const char*>(name)):QString("unknown");
    const auto png=frame.saveBuffer();
    return QImage::fromData(png.data(),int(png.size()),"PNG");
}
void ModelScene::scroll(double delta) {
    if (engine) {
        auto& camera=engine->getWindow().getCamera();
        const double factor=std::pow(1.0015,delta);
        if (config["settings"].toObject()["orthographic"].toBool()) camera.zoom(factor);
        else camera.dolly(factor);
        update();
    }
    rememberCamera();
}
void ModelScene::drag(QPointF d,bool pan) {
    if (!engine) return;
    auto& c=engine->getWindow().getCamera();
    if (pan) {
        const auto p=c.getPosition(), f=c.getFocalPoint();
        double distance=std::sqrt(std::pow(p[0]-f[0],2)+std::pow(p[1]-f[1],2)+std::pow(p[2]-f[2],2));
        c.pan(-d.x()*distance/height(),d.y()*distance/height());
    } else if (config["settings"].toObject()["lock_horizon"].toBool(true)) {
        orbit(-d.x()*.4*std::numbers::pi/180,d.y()*.4*std::numbers::pi/180);
    } else { c.azimuth(-d.x()*.4).elevation(d.y()*.4); }
    update(); rememberCamera(false);
}
extern "C" Q_DECL_EXPORT ModelWorker* asset_preview_create_model_worker(QJsonObject config) {
    return new ModelScene(std::move(config));
}
extern "C" Q_DECL_EXPORT QImage asset_preview_model_thumbnail(QJsonObject config,QString& error,QString& renderer) {
    return ModelScene::thumbnail(std::move(config),error,renderer);
}
