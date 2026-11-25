#include "viewer.h"
#include "backend_loader.h"
#include "preview_cache.h"
#include <QFileInfo>
#include <QImageReader>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QCoreApplication>
#include <QLibrary>
#include <QMouseEvent>
#include <QPainter>
#include <QtOpenGLWidgets/QOpenGLWidget>
#include <QSet>
#include <QWheelEvent>
#include <QPointer>
#include <QRunnable>
#include <QThreadPool>
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace {
class ImageDisplay final : public QOpenGLWidget {
public:
    using QOpenGLWidget::QOpenGLWidget;
    std::function<void(QPainter&)> paint;
protected:
    void paintGL() override { QPainter painter(this); if(paint)paint(painter); }
};
}

ImageView::ImageView(QWidget* parent) : QWidget(parent) {
    // The image and checker cover the framebuffer; avoid ancestor repainting.
    setAttribute(Qt::WA_OpaquePaintEvent);
    setMinimumSize(100, 100);
    setFocusPolicy(Qt::StrongFocus);
    setCursor(Qt::OpenHandCursor);
    if(QGuiApplication::platformName()=="wayland" || QGuiApplication::platformName()=="xcb") {
        auto* surface=new ImageDisplay(this);
        surface->setAttribute(Qt::WA_TransparentForMouseEvents);
        surface->setAttribute(Qt::WA_OpaquePaintEvent);
        surface->setFocusPolicy(Qt::NoFocus);
        surface->paint=[this](QPainter& painter) { paintImage(painter,true); };
        display=surface;
        // Showing an empty GL child initializes its context and paints a blank
        // framebuffer during card admission. Wait for the first valid pixels.
        surface->hide();
    }
}
ImageView::~ImageView() {
    if (cancelled) *cancelled=true;
    if (cache && !cachedKey.isEmpty() && cachedKey==preview::sourceCacheKey("image-v1-4096",cachedPath))
        cache->put(cachedKey,{std::move(image),originalSize,{}});
}
bool ImageView::reload(const QJsonObject& cfg, QString& error) {
    if (cancelled) *cancelled=true;
    pending=false; settings(cfg);
    // Restore before starting async work. Input while decoding owns the view
    // from this point on and must not be overwritten by a captured old config.
    if (!restored) {
        setImageView(cfg["image_view"].toObject());
    }
    const auto path=cfg["path"].toString();
    if(!QFileInfo(path).isFile()) { error="Waiting for a regular image file"; return false; }
    QImageReader reader(path);
    reader.setAutoTransform(true);
    const bool vectorImage=reader.format().toLower().startsWith("svg");
    static const QSet<QByteArray> standalone{"png","jpeg","webp","bmp","tiff","exr","hdr"};
    const auto key=standalone.contains(reader.format().toLower())?preview::sourceCacheKey("image-v1-4096",path):QString();
    if (cache && !key.isEmpty()) cache->identify(key,"image",path);
    preview::CachedPayload retained;
    if (cache && reuseCachedPayload && !key.isEmpty()) retained=cache->take(key);
    cacheHit=!retained.image.isNull();
    // Budget the output, since indexed/monochrome sources expand for display.
    const auto displayBytes=qint64(retained.image.width())*retained.image.height()*4;
    if (cacheHit && (!display || displayBytes<=1024*1024)) {
        previewImage={};
        loadTiming={{"cache_hit",true},{"decode_ms",0},{"queue_ms",0}};
        auto prepared=display?retained.image.convertToFormat(QImage::Format_ARGB32_Premultiplied):QImage{};
        publish(std::move(retained.image),retained.originalSize,path,key,std::move(prepared),vectorImage);
        return true;
    }
    // At most two foreground decode/preparation jobs across all visible cards.
    // Superseded requests check cancellation before opening their source.
    static QThreadPool pool;
    pool.setMaxThreadCount(2); pool.setExpiryTimeout(1000);
    cancelled=std::make_shared<std::atomic_bool>(false);
    const auto token=cancelled;
    const auto source=preview::sourceCacheKey("image-source-v1",path);
    pending=true; repaintImage();
    QElapsedTimer queued; queued.start();
    QPointer<ImageView> guard(this);
    const bool reuse=reuseCachedPayload;
    const bool accelerated=display!=nullptr;
    const bool fromCache=cacheHit;
    pool.start(QRunnable::create([guard,token,path,key,source,reuse,queued,accelerated,fromCache,vectorImage,retained=std::move(retained)]() mutable {
        if (*token) return;
        const double queueMs=queued.nsecsElapsed()/1e6;
        QSize size=retained.originalSize;
        auto next=std::move(retained.image);
        QString failure;
        qint64 elapsedUs=0;
        if(!fromCache) {
            QImageReader reader(path); reader.setAutoTransform(true);
            size=reader.size();
            // Bound decoded residency; preserve aspect ratio and expose original size.
            if (size.width()>4096 || size.height()>4096)
                reader.setScaledSize(size.scaled(4096,4096,Qt::KeepAspectRatio));
            QElapsedTimer decode; decode.start();
            next=reader.read(); elapsedUs=decode.nsecsElapsed()/1000;
            if(next.isNull())failure=reader.errorString();
        }
        if(*token)return;
        QElapsedTimer prepare;prepare.start();
        // Keep small source pixels intact; the gallery enlarges them with nearest sampling.
        auto thumb=next;
        if(thumb.width()>preview::PreviewCache::ThumbnailWidth || thumb.height()>preview::PreviewCache::ThumbnailHeight)
            thumb=thumb.scaled(preview::PreviewCache::ThumbnailWidth,preview::PreviewCache::ThumbnailHeight,Qt::KeepAspectRatio,Qt::SmoothTransformation);
        thumb=thumb.convertToFormat(QImage::Format_RGBA8888);
        // Large premultiplication and cache-hit thumbnail work must not block
        // pointer delivery while another image is being dragged.
        auto prepared=accelerated?next.convertToFormat(QImage::Format_ARGB32_Premultiplied):QImage{};
        const double prepareMs=prepare.nsecsElapsed()/1e6;
        if (source!=preview::sourceCacheKey("image-source-v1",path)) failure="Image changed during decode";
        if (*token) return;
        QMetaObject::invokeMethod(qApp,[guard,token,path,key,source,reuse,fromCache,vectorImage,queueMs,elapsedUs,prepareMs,size,failure,next=std::move(next),thumb=std::move(thumb),prepared=std::move(prepared)]() mutable {
            if (!guard || *token) return;
            guard->pending=false;
            guard->loadTiming={{"cache_hit",fromCache},{"queue_ms",queueMs},{"decode_ms",elapsedUs/1000.0},{"prepare_ms",prepareMs}};
            if (guard->cache && !fromCache) guard->cache->observe("image","decode",key,next.sizeInBytes(),reuse?"cache_miss":"forced_reload",elapsedUs,{{"success",failure.isEmpty()}});
            if (source!=preview::sourceCacheKey("image-source-v1",path)) failure="Image changed during decode";
            if (failure.isEmpty()) { guard->previewImage=std::move(thumb); guard->publish(std::move(next),size,path,key,std::move(prepared),vectorImage); }
            if (guard->loaded) guard->loaded(failure.isEmpty(),failure);
        },Qt::QueuedConnection);
    }));
    return true;
}
void ImageView::publish(QImage next,QSize size,const QString& path,const QString& key,QImage prepared,bool vectorImage) {
    image = std::move(next);
    // Premultiply before GPU interpolation so transparent texels cannot add
    // dark fringes. Keep the decoded pixels intact for source exports/cache.
    displayPixels=std::move(prepared);
    originalSize = size;
    // Classify the source, not its viewport or bounded display copy. Vector
    // artwork stays smooth even when its nominal dimensions are small.
    const auto sourceSize=size.isValid()?size:image.size();
    automaticNearest=!vectorImage && sourceSize.isValid() && sourceSize.width()<=256 && sourceSize.height()<=256;
    nearest=nearestMode.toBool(automaticNearest);
    cachedPath=path; cachedKey=key;
    if (display) display->show();
    ++loads;
    repaintImage();
}
void ImageView::fit() { zoom = 1; pan = {}; repaintImage(); remember(); }
QJsonObject ImageView::inspectionState() const { return {{"zoom",zoom},{"pan",QJsonArray{pan.x(),pan.y()}}}; }
void ImageView::setImageView(const QJsonObject& state) {
    zoom=state["zoom"].toDouble(1);
    const auto offset=state["pan"].toArray();
    pan={offset[0].toDouble(),offset[1].toDouble()};
    // Comparison may set framing before the first decode; reload must retain it.
    restored=true;
    repaintImage();
}
void ImageView::remember(bool persist) { if (inspectionChanged) inspectionChanged(inspectionState(),persist); }
void ImageView::settings(const QJsonObject& cfg) {
    const auto s = cfg["settings"].toObject();
    background = s["background"].toString("checker");
    nearestMode = s.value("nearest");
    nearest = nearestMode.toBool(automaticNearest);
    repaintImage();
}
QJsonObject ImageView::metrics() const {
    return {{"loads", loads}, {"renders", renders}, {"image_bytes", double(image.sizeInBytes())},{"cache_hit",cacheHit},
            {"paint_backend",display?"opengl":"raster"},
            {"nearest",nearest},{"filtering_auto",!nearestMode.isBool()},
            {"loading",pending},{"load_timing",loadTiming},
            {"width", originalSize.width()}, {"height", originalSize.height()},
            {"decoded_width", image.width()}, {"decoded_height", image.height()},{"image_view",inspectionState()}};
}
void ImageView::repaintImage() { if(display && display->isVisible())display->update(); else update(); }
QImage ImageView::snapshot() {
    return display && display->isVisible() && display->isValid()?display->grabFramebuffer():grab().toImage();
}
void ImageView::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    if(display)display->setGeometry(rect());
}
void ImageView::paintEvent(QPaintEvent*) {
    QPainter painter(this); paintImage(painter);
}
void ImageView::paintImage(QPainter& p,bool accelerated) {
    ++renders;
    if (background == "checker") {
        static const QPixmap checker = [] {
            QPixmap tile(40, 40);
            tile.fill(QColor("#171c22"));
            QPainter tp(&tile);
            tp.fillRect(0, 0, 20, 20, QColor("#252c34"));
            tp.fillRect(20, 20, 20, 20, QColor("#252c34"));
            return tile;
        }();
        p.drawTiledPixmap(rect(), checker);
    } else {
        p.fillRect(rect(), background == "light" ? QColor("#e0e0e0") : QColor("#171c22"));
    }
    if (image.isNull()) return;
    p.setRenderHint(QPainter::SmoothPixmapTransform, !nearest);
    const double scale = std::min(double(width()) / image.width(), double(height()) / image.height()) * zoom;
    QSizeF size(image.width() * scale, image.height() * scale);
    p.drawImage(QRectF(QPointF((width() - size.width()) / 2, (height() - size.height()) / 2) + QPointF(pan.x()*width(),pan.y()*height()), size), accelerated?displayPixels:image);
}
void ImageView::wheelEvent(QWheelEvent* e) {
    const auto delta=e->angleDelta().y()?e->angleDelta().y():e->pixelDelta().y();
    zoomBy(std::pow(1.0015,delta),e->position());e->accept();
}
void ImageView::zoomBy(double factor,QPointF anchor) {
    if(!std::isfinite(factor) || factor<=0 || width()<=0 || height()<=0)return;
    const double next = std::clamp(zoom * factor, 0.05, 32.0);
    const QPointF offset = anchor - QPointF(width()/2.0, height()/2.0);
    const auto moved=offset - (offset - QPointF(pan.x()*width(),pan.y()*height())) * (next / zoom);
    pan={moved.x()/width(),moved.y()/height()};
    zoom = next;
    repaintImage(); remember();
}
void ImageView::panBy(QPointF viewportDelta) {
    if(!std::isfinite(viewportDelta.x()) || !std::isfinite(viewportDelta.y()))return;
    pan+=viewportDelta;repaintImage();remember();
}
void ImageView::mousePressEvent(QMouseEvent* e) { last = e->position(); if (activated) activated(); setFocus(); }
void ImageView::mouseReleaseEvent(QMouseEvent* e) { remember(); e->accept(); }
void ImageView::mouseMoveEvent(QMouseEvent* e) {
    if (e->buttons() != Qt::NoButton) { const auto delta=e->position()-last; pan+=QPointF(delta.x()/width(),delta.y()/height()); repaintImage(); remember(false); }
    last = e->position();
}
void ImageView::mouseDoubleClickEvent(QMouseEvent*) { fit(); }

ModelHandle createMedia(QJsonObject config, QWidget* parent, LoadedCallback loaded, SettingsCallback changed) {
    static QLibrary library(QCoreApplication::applicationDirPath()+"/asset-preview-mpv.so");
    library.setLoadHints(QLibrary::PreventUnloadHint);
    using Factory = ModelHandle (*)(QJsonObject, QWidget*, LoadedCallback, SettingsCallback);
    auto factory=reinterpret_cast<Factory>(preview::resolveBackend(library,config["kind"]=="audio"?"asset_preview_create_audio":"asset_preview_create_video"));
    return factory(std::move(config),parent,std::move(loaded),std::move(changed));
}

ModelHandle createInspection(QJsonObject config, QWidget* parent, LoadedCallback loaded, InspectionCallback changed, InspectionEventCallback event) {
    if (!ASSET_PREVIEW_HAS_INSPECTION) throw std::runtime_error("Component inspection was disabled in this build");
    static QLibrary library(QCoreApplication::applicationDirPath()+"/asset-preview-inspection.so");
    library.setLoadHints(QLibrary::PreventUnloadHint);
    using Factory = ModelHandle (*)(QJsonObject, QWidget*, LoadedCallback, InspectionCallback, InspectionEventCallback);
    auto factory=reinterpret_cast<Factory>(preview::resolveBackend(library,"asset_preview_create_inspection"));
    return factory(std::move(config),parent,std::move(loaded),std::move(changed),std::move(event));
}
