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
#include <QSet>
#include <QWheelEvent>
#include <QPointer>
#include <QRunnable>
#include <QThreadPool>
#include <algorithm>
#include <cmath>
#include <stdexcept>

ImageView::ImageView(QWidget* parent) : QWidget(parent) {
    setMinimumSize(100, 100);
    setFocusPolicy(Qt::StrongFocus);
    setCursor(Qt::OpenHandCursor);
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
        const auto saved=cfg["image_view"].toObject();
        zoom=saved["zoom"].toDouble(1);
        const auto offset=saved["pan"].toArray();
        if (offset.size()==2) pan={offset[0].toDouble(),offset[1].toDouble()};
        restored=true;
    }
    const auto path=cfg["path"].toString();
    if(!QFileInfo(path).isFile()) { error="Waiting for a regular image file"; return false; }
    QImageReader reader(path);
    reader.setAutoTransform(true);
    static const QSet<QByteArray> standalone{"png","jpeg","webp","bmp","tiff","exr","hdr"};
    const auto key=standalone.contains(reader.format().toLower())?preview::sourceCacheKey("image-v1-4096",path):QString();
    if (cache && !key.isEmpty()) cache->identify(key,"image",path);
    preview::CachedPayload retained;
    if (cache && reuseCachedPayload && !key.isEmpty()) retained=cache->take(key);
    cacheHit=!retained.image.isNull();
    if (cacheHit) {
        previewImage={};
        loadTiming={{"cache_hit",true},{"decode_ms",0},{"queue_ms",0}};
        publish(std::move(retained.image),retained.originalSize,path,key);
        return true;
    }
    // At most two foreground decoders, shared across all visible cards. Pending
    // and superseded requests check cancellation before opening their source.
    static QThreadPool pool;
    pool.setMaxThreadCount(2); pool.setExpiryTimeout(1000);
    cancelled=std::make_shared<std::atomic_bool>(false);
    const auto token=cancelled;
    const auto source=preview::sourceCacheKey("image-source-v1",path);
    pending=true; update();
    QElapsedTimer queued; queued.start();
    QPointer<ImageView> guard(this);
    const bool reuse=reuseCachedPayload;
    pool.start(QRunnable::create([guard,token,path,key,source,reuse,queued] {
        if (*token) return;
        const double queueMs=queued.nsecsElapsed()/1e6;
        QImageReader reader(path); reader.setAutoTransform(true);
        const auto size=reader.size();
        // Bound decoded residency; preserve aspect ratio and expose original size.
        if (size.width()>4096 || size.height()>4096)
            reader.setScaledSize(size.scaled(4096,4096,Qt::KeepAspectRatio));
        QElapsedTimer decode; decode.start();
        auto next=reader.read();
        auto thumb=next.isNull()?QImage{}:next.scaled(preview::PreviewCache::ThumbnailWidth,preview::PreviewCache::ThumbnailHeight,Qt::KeepAspectRatio,Qt::SmoothTransformation).convertToFormat(QImage::Format_RGBA8888);
        const auto elapsedUs=decode.nsecsElapsed()/1000;
        auto failure=next.isNull()?reader.errorString():QString();
        if (source!=preview::sourceCacheKey("image-source-v1",path)) failure="Image changed during decode";
        if (*token) return;
        QMetaObject::invokeMethod(qApp,[guard,token,path,key,source,reuse,queueMs,elapsedUs,size,failure,next=std::move(next),thumb=std::move(thumb)]() mutable {
            if (!guard || *token) return;
            guard->pending=false;
            guard->loadTiming={{"cache_hit",false},{"queue_ms",queueMs},{"decode_ms",elapsedUs/1000.0}};
            if (guard->cache) guard->cache->observe("image","decode",key,next.sizeInBytes(),reuse?"cache_miss":"forced_reload",elapsedUs,{{"success",failure.isEmpty()}});
            if (source!=preview::sourceCacheKey("image-source-v1",path)) failure="Image changed during decode";
            if (failure.isEmpty()) { guard->previewImage=std::move(thumb); guard->publish(std::move(next),size,path,key); }
            if (guard->loaded) guard->loaded(failure.isEmpty(),failure);
        },Qt::QueuedConnection);
    }));
    return true;
}
void ImageView::publish(QImage next,QSize size,const QString& path,const QString& key) {
    image = std::move(next);
    originalSize = size;
    cachedPath=path; cachedKey=key;
    ++loads;
    update();
}
void ImageView::fit() { zoom = 1; pan = {}; update(); remember(); }
QJsonObject ImageView::inspectionState() const { return {{"zoom",zoom},{"pan",QJsonArray{pan.x(),pan.y()}}}; }
void ImageView::remember(bool persist) { if (inspectionChanged) inspectionChanged(inspectionState(),persist); }
void ImageView::settings(const QJsonObject& cfg) {
    const auto s = cfg["settings"].toObject();
    background = s["background"].toString("checker");
    nearest = s["nearest"].toBool();
    update();
}
QJsonObject ImageView::metrics() const {
    return {{"loads", loads}, {"renders", renders}, {"image_bytes", double(image.sizeInBytes())},{"cache_hit",cacheHit},
            {"loading",pending},{"load_timing",loadTiming},
            {"width", originalSize.width()}, {"height", originalSize.height()},
            {"decoded_width", image.width()}, {"decoded_height", image.height()},{"image_view",inspectionState()}};
}
void ImageView::paintEvent(QPaintEvent*) {
    ++renders;
    QPainter p(this);
    p.fillRect(rect(), background == "light" ? QColor("#e0e0e0") : QColor("#171c22"));
    if (background == "checker") {
        for (int y = 0; y < height(); y += 20)
            for (int x = 0; x < width(); x += 20)
                if ((x / 20 + y / 20) % 2 == 0) p.fillRect(x, y, 20, 20, QColor("#252c34"));
    }
    if (image.isNull()) return;
    p.setRenderHint(QPainter::SmoothPixmapTransform, !nearest);
    const double scale = std::min(double(width()) / image.width(), double(height()) / image.height()) * zoom;
    QSizeF size(image.width() * scale, image.height() * scale);
    p.drawImage(QRectF(QPointF((width() - size.width()) / 2, (height() - size.height()) / 2) + QPointF(pan.x()*width(),pan.y()*height()), size), image);
}
void ImageView::wheelEvent(QWheelEvent* e) {
    const double next = std::clamp(zoom * std::pow(1.0015, e->angleDelta().y()), 0.05, 32.0);
    const QPointF offset = e->position() - QPointF(width()/2.0, height()/2.0);
    const auto moved=offset - (offset - QPointF(pan.x()*width(),pan.y()*height())) * (next / zoom);
    pan={moved.x()/width(),moved.y()/height()};
    zoom = next;
    update(); remember(); e->accept();
}
void ImageView::mousePressEvent(QMouseEvent* e) { last = e->position(); if (activated) activated(); setFocus(); }
void ImageView::mouseReleaseEvent(QMouseEvent* e) { remember(); e->accept(); }
void ImageView::mouseMoveEvent(QMouseEvent* e) {
    if (e->buttons() != Qt::NoButton) { const auto delta=e->position()-last; pan+=QPointF(delta.x()/width(),delta.y()/height()); update(); remember(false); }
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
