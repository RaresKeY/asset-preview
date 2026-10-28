#include "video_view.h"
#include <QAbstractButton>
#include <QFileInfo>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QTimer>
#include <QtGui/qguiapplication_platform.h>
#include <mpv/client.h>
#include <mpv/render_gl.h>
#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "mpv_player.h"
#include "gl_framebuffer.h"
using namespace preview::media;

struct VideoView::Player {
    mpv_handle* client=nullptr;
    mpv_render_context* render=nullptr;
    QJsonObject properties;
    bool fileLoaded=false, frameDue=false;
    ~Player() {
        if (render) { mpv_render_context_set_update_callback(render,nullptr,nullptr); mpv_render_context_free(render); }
        if (client) { mpv_set_wakeup_callback(client,nullptr,nullptr); mpv_terminate_destroy(client); }
    }
};

VideoView::VideoView(QJsonObject cfg, LoadedCallback done, SettingsCallback patch)
    : config(std::move(cfg)),loaded(std::move(done)),changed(std::move(patch)) { setMinimumSize({100,100}); }
VideoView::~VideoView() { closing=true; if (context()) disconnect(context(),nullptr,this,nullptr); cleanup(); }
void VideoView::cleanup() {
    if (context()) makeCurrent();
    candidate.reset(); current.reset(); initialized=false; ++generation;
    if (context()) doneCurrent();
}
void VideoView::wakeup(void* pointer) {
    auto* self=static_cast<VideoView*>(pointer);
    if (self->closing || self->queued.exchange(true)) return;
    QMetaObject::invokeMethod(self,[self] {
        self->queued=false;
        if (!self->closing) { self->drain(); if (self->candidate) self->update(); }
    },Qt::QueuedConnection);
}
void VideoView::redraw(void* pointer) {
    auto* self=static_cast<VideoView*>(pointer);
    if (self->closing || self->frameQueued.exchange(true)) return;
    QMetaObject::invokeMethod(self,[self] {
        self->frameQueued=false; if (!self->closing) self->update();
    },Qt::QueuedConnection);
}
void VideoView::initializeGL() {
    initialized=true;
    connect(context(),&QOpenGLContext::aboutToBeDestroyed,this,[this] { cleanup(); },Qt::DirectConnection);
    const auto* name=context()->functions()->glGetString(GL_RENDERER);
    renderer=name?QString::fromUtf8(reinterpret_cast<const char*>(name)):"unknown";
    qInfo("Asset Preview video OpenGL renderer: %s",qPrintable(renderer));
    QString error; if (!reload(config,error) && loaded) loaded(false,error);
}
bool VideoView::loading() const { return !initialized || bool(candidate); }
bool VideoView::reload(const QJsonObject& cfg, QString& error) {
    config=cfg;
    if (!QFileInfo(config["path"].toString()).isFile()) { error="Waiting for video file"; return false; }
    if (!initialized) return true;
    makeCurrent();
    try {
        candidate.reset();
        auto next=std::make_unique<Player>();
        next->client=mpv_create(); if (!next->client) throw std::runtime_error("Cannot create libmpv player");
        configure(next->client,false);
        checked(mpv_set_option_string(next->client,"pause",config["settings"].toObject()["paused"].toBool()?"yes":"no"));
        checked(mpv_initialize(next->client));
        mpv_opengl_init_params init{[](void* ctx,const char* name)->void* {
            return reinterpret_cast<void*>(static_cast<QOpenGLContext*>(ctx)->getProcAddress(name));
        },context()};
        mpv_render_param params[]={{MPV_RENDER_PARAM_API_TYPE,const_cast<char*>(MPV_RENDER_API_TYPE_OPENGL)},
            {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS,&init},{MPV_RENDER_PARAM_INVALID,nullptr},{MPV_RENDER_PARAM_INVALID,nullptr}};
#if QT_CONFIG(xcb)
        if (auto* native=qGuiApp->nativeInterface<QNativeInterface::QX11Application>())
            params[2]={MPV_RENDER_PARAM_X11_DISPLAY,native->display()};
#endif
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
#if QT_CONFIG(wayland)
        if (auto* native=qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>())
            params[2]={MPV_RENDER_PARAM_WL_DISPLAY,native->display()};
#endif
#endif
        checked(mpv_render_context_create(&next->render,next->client,params));
        const std::pair<const char*,mpv_format> observed[]={
            {"video-params/w",MPV_FORMAT_INT64},{"video-params/h",MPV_FORMAT_INT64},
            {"duration",MPV_FORMAT_DOUBLE},{"time-pos",MPV_FORMAT_DOUBLE},
            {"pause",MPV_FORMAT_FLAG},{"mute",MPV_FORMAT_FLAG},{"eof-reached",MPV_FORMAT_FLAG},{"seekable",MPV_FORMAT_FLAG},
            {"hwdec-current",MPV_FORMAT_STRING},{"video-codec",MPV_FORMAT_STRING}};
        for (const auto& [key,format]:observed) checked(mpv_observe_property(next->client,0,key,format));
        mpv_set_wakeup_callback(next->client,&VideoView::wakeup,this);
        mpv_render_context_set_update_callback(next->render,&VideoView::redraw,this);
        const auto path=config["path"].toString().toUtf8(); command(next->client,"loadfile",path);
        candidate=std::move(next);
        const int token=++generation;
        QTimer::singleShot(15000,this,[this,token] { if (token==generation && candidate) failed("Video load timed out"); });
        doneCurrent(); update(); return true;
    } catch (const std::exception& e) { error=QString::fromUtf8(e.what()); doneCurrent(); return false; }
}
void VideoView::failed(const QString& error) {
    makeCurrent(); candidate.reset(); doneCurrent(); ++generation;
    if (loaded) loaded(false,error);
}
void VideoView::drain() {
    QString error;
    bool notify=false;
    for (auto* p:{current.get(),candidate.get()}) {
        if (!p) continue;
        while (auto* event=mpv_wait_event(p->client,0)) {
            if (event->event_id==MPV_EVENT_NONE) break;
            if (event->event_id==MPV_EVENT_PROPERTY_CHANGE) {
                const auto* prop=static_cast<mpv_event_property*>(event->data);
                notify|=p==current.get();
                updateProperty(p->properties,*prop);
                if (p==current.get() && QByteArray(prop->name)=="eof-reached" && p->properties[prop->name].toBool() &&
                    !config["settings"].toObject()["loop"].toBool(true)) change({{"paused",true}});
            } else if (event->event_id==MPV_EVENT_FILE_LOADED) p->fileLoaded=true;
            else if (p==candidate.get() && event->event_id==MPV_EVENT_END_FILE) {
                const auto* end=static_cast<mpv_event_end_file*>(event->data);
                error=end->error<0?QString::fromUtf8(mpv_error_string(end->error)):"File has no playable video track";
            }
        }
    }
    if (!error.isEmpty()) failed(error);
    if (notify && playbackChanged) playbackChanged();
}
void VideoView::apply(Player& p) {
    const auto s=config["settings"].toObject();
    flag(p.client,"pause",s["paused"].toBool()); flag(p.client,"mute",s["muted"].toBool(true));
    string(p.client,"aid",s["muted"].toBool(true)?"no":"auto");
    string(p.client,"loop-file",s["loop"].toBool(true)?"inf":"no");
    string(p.client,"background-color",s["background"].toString()=="light"?"#e0e0e0":"#13171d");
}
void VideoView::settings(const QJsonObject& cfg) {
    config=cfg;
    try {
        if (current) {
            if (current->properties["eof-reached"].toBool() && !config["settings"].toObject()["paused"].toBool())
                command(current->client,"seek","0","absolute+exact");
            apply(*current);
        }
        if (candidate) flag(candidate->client,"pause",config["settings"].toObject()["paused"].toBool());
    }
    catch (const std::exception& e) { if (loaded) loaded(false,QString::fromUtf8(e.what())); }
}
void VideoView::change(QJsonObject patch) { if (changed) changed(patch); }
bool VideoView::seek(double position, QString& error) {
    if (!current) { error="Video is not loaded"; return false; }
    try { command(current->client,"seek",QByteArray::number(position,'f',3),"absolute+exact"); return true; }
    catch (const std::exception& e) { error=QString::fromUtf8(e.what()); return false; }
}
void VideoView::fit() { /* Video is always fitted with preserved aspect ratio. */ }
QJsonObject VideoView::metrics() const {
    QJsonObject result{{"loads",loads},{"renders",renders},{"engine",bool(current)},{"backend","libmpv"},{"renderer",renderer},{"loading",bool(candidate)}};
    if (current) {
        const auto& p=current->properties;
        result["width"]=p["video-params/w"]; result["height"]=p["video-params/h"];
        result["duration"]=p["duration"]; result["position"]=p["time-pos"];
        result["paused"]=p["pause"]; result["muted"]=p["mute"]; result["eof"]=p["eof-reached"]; result["seekable"]=p["seekable"];
        result["hwdec"]=p["hwdec-current"]; result["codec"]=p["video-codec"];
    }
    return result;
}
void VideoView::paintGL() { paintFrame(defaultFramebufferObject()); }
void VideoView::paintFrame(GLuint framebuffer) {
    ++renders;
    auto* gl=context()->functions();
    gl->glBindFramebuffer(GL_FRAMEBUFFER,framebuffer);
    gl->glViewport(0,0,int(width()*devicePixelRatio()),int(height()*devicePixelRatio()));
    bool replace=false;
    if (candidate) {
        candidate->frameDue|=bool(mpv_render_context_update(candidate->render)&MPV_RENDER_UPDATE_FRAME);
        replace=candidate->frameDue && candidate->fileLoaded && candidate->properties["video-params/w"].toInt()>0;
    }
    Player* p=replace?candidate.get():current.get();
    if (p) {
        if (!replace) mpv_render_context_update(p->render);
        mpv_opengl_fbo fbo{int(framebuffer),int(width()*devicePixelRatio()),int(height()*devicePixelRatio()),0};
        int flip=1,block=0;
        mpv_render_param params[]={{MPV_RENDER_PARAM_OPENGL_FBO,&fbo},{MPV_RENDER_PARAM_FLIP_Y,&flip},
            {MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME,&block},{MPV_RENDER_PARAM_INVALID,nullptr}};
        const int code=mpv_render_context_render(p->render,params);
        if (code<0) { if (replace) QTimer::singleShot(0,this,[this,code] { failed(QString::fromUtf8(mpv_error_string(code))); }); return; }
        if (replace) {
            current=std::move(candidate); ++loads; ++generation;
            apply(*current);
            QTimer::singleShot(0,this,[this] { if (loaded) loaded(true,{}); });
        }
    } else {
        gl->glClearColor(.075f,.09f,.115f,1); gl->glClear(GL_COLOR_BUFFER_BIT);
    }
}
QImage VideoView::snapshot() {
    return preview::captureFramebuffer(*this,[this](GLuint framebuffer) { paintFrame(framebuffer); });
}
void VideoView::mousePressEvent(QMouseEvent* e) { if (activated) activated(); if (focusSurface) focusSurface(); requestActivate(); e->accept(); }
bool VideoView::eventFilter(QObject* watched, QEvent* e) {
    // After programmatic focus, Qt can send keys to the widget container
    // instead of the native child. Forward only this video's playback keys.
    // Focused playback buttons retain native Space activation.
    if ((e->type()==QEvent::ShortcutOverride || e->type()==QEvent::KeyPress) &&
        static_cast<QKeyEvent*>(e)->key()==Qt::Key_Space && qobject_cast<QAbstractButton*>(watched))
        return QOpenGLWindow::eventFilter(watched,e);
    if (e->type()==QEvent::ShortcutOverride && playbackKey(static_cast<QKeyEvent*>(e))) { e->accept(); return true; }
    if (e->type()==QEvent::KeyPress && playbackKey(static_cast<QKeyEvent*>(e))) {
        keyPressEvent(static_cast<QKeyEvent*>(e)); return true;
    }
    return QOpenGLWindow::eventFilter(watched,e);
}
void VideoView::keyPressEvent(QKeyEvent* e) {
    if (!playbackKey(e)) { QOpenGLWindow::keyPressEvent(e); return; }
    const auto s=config["settings"].toObject();
    if (e->key()==Qt::Key_Space) change({{"paused",!s["paused"].toBool()}});
    else if (e->key()==Qt::Key_M) change({{"muted",!s["muted"].toBool(true)}});
    else {
        QString error; const double at=current?current->properties["time-pos"].toDouble():0;
        seek(e->key()==Qt::Key_Home?0:std::max(0.0,at+(e->key()==Qt::Key_Left?-5:5)),error);
    }
    e->accept();
}

extern "C" Q_DECL_EXPORT ModelHandle asset_preview_create_video(QJsonObject config,QWidget* parent,LoadedCallback loaded,SettingsCallback changed) {
    auto* video=new VideoView(std::move(config),std::move(loaded),std::move(changed));
    auto* surface=QWidget::createWindowContainer(video,parent); surface->setFocusPolicy(Qt::StrongFocus);
    surface->installEventFilter(video);
    video->focusSurface=[surface] { surface->setFocus(Qt::MouseFocusReason); };
    return {surface,video};
}
