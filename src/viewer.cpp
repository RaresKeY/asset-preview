#include "viewer.h"
#include <QFileInfo>
#include <QImageReader>
#include <QJsonArray>
#include <QCoreApplication>
#include <QLibrary>
#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <stdexcept>

ImageView::ImageView(QWidget* parent) : QWidget(parent) {
    QImageReader::setAllocationLimit(64);
    setMinimumSize(100, 100);
    setFocusPolicy(Qt::StrongFocus);
    setCursor(Qt::OpenHandCursor);
}
bool ImageView::reload(const QJsonObject& cfg, QString& error) {
    QImageReader reader(cfg["path"].toString());
    reader.setAutoTransform(true);
    const QSize size = reader.size();
    // Bound decoded residency; preserve aspect ratio and expose original size.
    if (size.width() > 4096 || size.height() > 4096)
        reader.setScaledSize(size.scaled(4096, 4096, Qt::KeepAspectRatio));
    QImage next = reader.read();
    if (next.isNull()) { error = reader.errorString(); return false; }
    image = std::move(next);
    originalSize = size;
    ++loads;
    if (!restored) {
        const auto saved=cfg["image_view"].toObject();
        zoom=saved["zoom"].toDouble(1);
        const auto offset=saved["pan"].toArray();
        if (offset.size()==2) pan={offset[0].toDouble(),offset[1].toDouble()};
        restored=true;
    }
    settings(cfg);
    update();
    return true;
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
    return {{"loads", loads}, {"renders", renders}, {"image_bytes", double(image.sizeInBytes())},
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

ModelHandle createModel(QJsonObject config, QWidget* parent, LoadedCallback loaded, InspectionCallback changed) {
    // Keep the module resident after first use: VTK/driver global registrations
    // are not safe to unload while Qt has graphics infrastructure in the process.
    static QLibrary library(QCoreApplication::applicationDirPath()+"/asset-preview-f3d.so");
    library.setLoadHints(QLibrary::PreventUnloadHint);
    using Factory = ModelHandle (*)(QJsonObject, QWidget*, LoadedCallback, InspectionCallback);
    auto factory=reinterpret_cast<Factory>(library.resolve("asset_preview_create_model"));
    if (!factory) throw std::runtime_error(library.errorString().toStdString());
    return factory(std::move(config),parent,std::move(loaded),std::move(changed));
}

ModelHandle createVideo(QJsonObject config, QWidget* parent, LoadedCallback loaded, SettingsCallback changed) {
    static QLibrary library(QCoreApplication::applicationDirPath()+"/asset-preview-mpv.so");
    library.setLoadHints(QLibrary::PreventUnloadHint);
    using Factory = ModelHandle (*)(QJsonObject, QWidget*, LoadedCallback, SettingsCallback);
    auto factory=reinterpret_cast<Factory>(library.resolve("asset_preview_create_video"));
    if (!factory) throw std::runtime_error(library.errorString().toStdString());
    return factory(std::move(config),parent,std::move(loaded),std::move(changed));
}
