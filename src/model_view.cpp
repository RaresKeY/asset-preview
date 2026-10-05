#include "model_view.h"
#include <QFileInfo>
#include <QImageReader>
#include <QJsonArray>
#include <QMouseEvent>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QPainter>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <numbers>

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
                quad(a, a+1, b+1, b);
            }
    }
    return mesh;
}

ModelView::ModelView(QJsonObject cfg) : QOpenGLWindow(), config(std::move(cfg)) {
    setMinimumSize(QSize(100,100));
    setCursor(Qt::OpenHandCursor);
}
ModelView::~ModelView() {
    if (context()) disconnect(context(),nullptr,this,nullptr);
    cleanup();
}
void ModelView::cleanup() {
    if (context() && engine) { makeCurrent(); engine.reset(); doneCurrent(); }
    initialized = false;
}
void ModelView::initializeGL() {
    initialized = true;
    connect(context(), &QOpenGLContext::aboutToBeDestroyed, this, [this] { cleanup(); }, Qt::DirectConnection);
    const auto* name = context()->functions()->glGetString(GL_RENDERER);
    renderer = name ? QString::fromUtf8(reinterpret_cast<const char*>(name)) : "unknown";
    qInfo("Asset Preview OpenGL renderer: %s", qPrintable(renderer));
    QString error;
    bool ok = false;
    try {
        static const bool plugins=[] { f3d::engine::loadPlugin("native"); f3d::engine::loadPlugin("assimp"); return true; }();
        (void)plugins;
        ok = load(error);
    } catch (const std::exception& e) { error=QString::fromUtf8(e.what()); }
    if (loaded) loaded(ok, error);
}
void ModelView::applyOptions(f3d::engine& e) {
    auto& opt = e.getOptions();
    const auto s = config["settings"].toObject();
    opt.render.background.color = s["background"].toString() == "light"
        ? f3d::color_t{0.8,0.8,0.8} : f3d::color_t{0.075,0.09,0.115};
    opt.render.grid.enable = s["grid"].toBool(true);
    opt.render.grid.color = f3d::color_t{0.35,0.40,0.45};
    opt.render.show_edges = s["edges"].toBool();
    opt.ui.axis = s["axes"].toBool(false);
    opt.render.light.intensity = s["light"].toDouble(1.0);
    opt.render.hdri.ambient = s["lighting"].toString("studio") == "studio";
    opt.render.effect.tone_mapping = true;
    opt.render.effect.antialiasing.enable = true;
    opt.render.raytracing.enable = false;
    opt.scene.camera.orthographic = s["orthographic"].toBool();
    opt.scene.up_direction = s["up_axis"].toString("y") == "z"
        ? f3d::direction_t{0,0,1} : f3d::direction_t{0,1,0};
    // Authored properties are restored by a scene reload when a display mode
    // changes: F3D overrides mutate its imported actor properties.
    opt.model = f3d::options{}.model;
    if (config["kind"].toString() == "material") {
        opt.model.color.rgb = f3d::color_t{1,1,1};
        opt.model.color.texture = config["path"].toString().toStdString();
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
bool ModelView::load(QString& error) {
    if (!QFileInfo::exists(config["path"].toString())) { error = "Waiting for file"; return false; }
    if (QFileInfo(config["path"].toString()).size()>256*1024*1024) { error="File exceeds 256 MiB"; return false; }
    try {
        // Construct a replacement first: malformed partial saves keep the last good scene.
        auto next = std::make_unique<f3d::engine>(f3d::engine::createExternal(
            [this](const char* name) { return context()->getProcAddress(name); }));
        applyOptions(*next);
        if (config["kind"].toString() == "material") {
            for (const auto& key : {"normal", "orm"}) {
                const QString path = config["maps"].toObject()[key].toString();
                if (!path.isEmpty() && !QFileInfo::exists(path)) { error = "Waiting for " + path; return false; }
            }
            next->getScene().add(primitive(config["settings"].toObject()["shape"].toString("sphere")));
        } else {
            const std::filesystem::path path(config["path"].toString().toStdString());
            if (!next->getScene().supports(path)) { error = "Unsupported model format"; return false; }
            next->getScene().add(path);
        }
        auto& window = next->getWindow();
        window.setSize(std::max(1, int(width()*devicePixelRatioF())), std::max(1, int(height()*devicePixelRatioF())));
        if (engine) window.getCamera().setState(engine->getWindow().getCamera().getState());
        else { window.getCamera().azimuth(30).elevation(20).resetToBounds(); }
        engine = std::move(next);
        if (config["settings"].toObject()["lock_horizon"].toBool(true)) orbit(0,0);
        ++loads;
        return true;
    } catch (const std::exception& e) { error = QString::fromUtf8(e.what()); return false; }
}
bool ModelView::reload(const QJsonObject& cfg, QString& error) {
    config = cfg;
    if (!initialized) return true;
    makeCurrent(); bool ok = load(error); doneCurrent(); update(); return ok;
}
void ModelView::settings(const QJsonObject& cfg) {
    const auto old=config["settings"].toObject(), next=cfg["settings"].toObject();
    const bool reloadScene = next["shape"] != old["shape"] || next["up_axis"] != old["up_axis"] ||
        next["materials"].toBool(true) != old["materials"].toBool(true) ||
        next["textures"].toBool(true) != old["textures"].toBool(true);
    config = cfg;
    if (engine) {
        makeCurrent();
        if (reloadScene) { QString error; const bool ok = load(error); if (loaded) loaded(ok, error); }
        else applyOptions(*engine);
        if (next["lock_horizon"].toBool(true)) orbit(0,0);
        doneCurrent(); update();
    }
}
void ModelView::fit() {
    if (engine) { makeCurrent(); engine->getWindow().getCamera().resetToBounds(); if (config["settings"].toObject()["lock_horizon"].toBool(true)) orbit(0,0); doneCurrent(); update(); }
}
QJsonObject ModelView::metrics() const {
    QJsonObject result{{"loads", loads}, {"renders", renders}, {"engine", bool(engine)}, {"renderer", renderer}};
    if (engine) {
        const auto c=engine->getWindow().getCamera().getState();
        result["camera"]=QJsonObject{{"position",QJsonArray{c.position[0],c.position[1],c.position[2]}},
            {"focal",QJsonArray{c.focalPoint[0],c.focalPoint[1],c.focalPoint[2]}},
            {"up",QJsonArray{c.viewUp[0],c.viewUp[1],c.viewUp[2]}}};
        result["lighting"]=config["settings"].toObject()["lighting"].toString("studio");
        result["lights"]=engine->getScene().getLightCount();
    }
    return result;
}
f3d::vector3_t ModelView::worldUp() const {
    return config["settings"].toObject()["up_axis"].toString("y") == "z"
        ? f3d::vector3_t{0,0,1} : f3d::vector3_t{0,1,0};
}
void ModelView::orbit(double yawDelta, double pitchDelta) {
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
void ModelView::resizeGL(int w, int h) {
    if (engine) engine->getWindow().setSize(std::max(1,int(w*devicePixelRatioF())), std::max(1,int(h*devicePixelRatioF())));
}
void ModelView::paintGL() {
    ++renders;
    // F3D's external window blits the caller's color/depth before rendering.
    // Qt does not initialize that content for us. A stale depth or alpha buffer
    // causes missing faces and transparent grids even when engine.render succeeds.
    auto* f=context()->functions();
    const bool light=config["settings"].toObject()["background"].toString()=="light";
    f->glDisable(GL_SCISSOR_TEST);
    f->glEnable(GL_DEPTH_TEST); f->glDepthFunc(GL_LEQUAL);
    f->glDisable(GL_CULL_FACE);
    f->glEnable(GL_BLEND);
    f->glBlendFuncSeparate(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA,GL_ONE,GL_ONE_MINUS_SRC_ALPHA);
    f->glBindFramebuffer(GL_FRAMEBUFFER,defaultFramebufferObject());
    f->glViewport(0,0,int(width()*devicePixelRatioF()),int(height()*devicePixelRatioF()));
    f->glDepthMask(GL_TRUE); f->glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
    f->glClearDepthf(1.0f); f->glClearStencil(0);
    f->glClearColor(light?.8f:.075f,light?.8f:.09f,light?.8f:.115f,1);
    f->glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT|GL_STENCIL_BUFFER_BIT);
    if (engine) engine->getWindow().render();
}
QImage ModelView::snapshot() {
    makeCurrent();
    // Render into the back buffer before readback; after a native-window swap,
    // the back buffer can still contain the previous preview revision.
    paintGL();
    QImage image=grabFramebuffer();
    doneCurrent();
    return image;
}
void ModelView::wheelEvent(QWheelEvent* e) {
    if (engine) { makeCurrent(); engine->getWindow().getCamera().dolly(std::pow(1.0015,e->angleDelta().y())); doneCurrent(); update(); }
    e->accept();
}
void ModelView::mousePressEvent(QMouseEvent* e) { last=e->position(); requestActivate(); }
void ModelView::mouseMoveEvent(QMouseEvent* e) {
    if (!engine || e->buttons() == Qt::NoButton) { last=e->position(); return; }
    const QPointF d=e->position()-last; last=e->position();
    makeCurrent(); auto& c=engine->getWindow().getCamera();
    if (e->buttons().testFlag(Qt::MiddleButton) || e->buttons().testFlag(Qt::RightButton) || e->modifiers().testFlag(Qt::ShiftModifier)) {
        const auto p=c.getPosition(), f=c.getFocalPoint();
        double distance=std::sqrt(std::pow(p[0]-f[0],2)+std::pow(p[1]-f[1],2)+std::pow(p[2]-f[2],2));
        c.pan(-d.x()*distance/height(),d.y()*distance/height());
    } else if (config["settings"].toObject()["lock_horizon"].toBool(true)) {
        orbit(-d.x()*.4*std::numbers::pi/180,-d.y()*.4*std::numbers::pi/180);
    } else { c.azimuth(-d.x()*.4).elevation(-d.y()*.4); }
    doneCurrent(); update();
}
void ModelView::mouseDoubleClickEvent(QMouseEvent*) { fit(); }

extern "C" Q_DECL_EXPORT ModelHandle asset_preview_create_model(QJsonObject config, QWidget* parent, LoadedCallback loaded) {
    auto* model=new ModelView(std::move(config));
    model->loaded=std::move(loaded);
    auto* surface=QWidget::createWindowContainer(model,parent);
    surface->setFocusPolicy(Qt::StrongFocus);
    return {surface,model};
}
