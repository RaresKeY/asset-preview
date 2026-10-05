#include "viewer.h"
#include <QFileInfo>
#include <QImageReader>
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
    settings(cfg);
    update();
    return true;
}
void ImageView::fit() { zoom = 1; pan = {}; update(); }
void ImageView::settings(const QJsonObject& cfg) {
    const auto s = cfg["settings"].toObject();
    background = s["background"].toString("checker");
    nearest = s["nearest"].toBool();
    update();
}
QJsonObject ImageView::metrics() const {
    return {{"loads", loads}, {"renders", renders}, {"image_bytes", double(image.sizeInBytes())},
            {"width", originalSize.width()}, {"height", originalSize.height()},
            {"decoded_width", image.width()}, {"decoded_height", image.height()}};
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
    p.drawImage(QRectF(QPointF((width() - size.width()) / 2, (height() - size.height()) / 2) + pan, size), image);
}
void ImageView::wheelEvent(QWheelEvent* e) {
    zoom = std::clamp(zoom * std::pow(1.0015, e->angleDelta().y()), 0.05, 32.0);
    update(); e->accept();
}
void ImageView::mousePressEvent(QMouseEvent* e) { last = e->position(); setFocus(); }
void ImageView::mouseMoveEvent(QMouseEvent* e) {
    if (e->buttons() != Qt::NoButton) { pan += e->position() - last; update(); }
    last = e->position();
}
void ImageView::mouseDoubleClickEvent(QMouseEvent*) { fit(); }

ModelHandle createModel(QJsonObject config, QWidget* parent, LoadedCallback loaded) {
    // Keep the module resident after first use: VTK/driver global registrations
    // are not safe to unload while Qt has graphics infrastructure in the process.
    static QLibrary library(QCoreApplication::applicationDirPath()+"/asset-preview-f3d.so");
    library.setLoadHints(QLibrary::PreventUnloadHint);
    using Factory = ModelHandle (*)(QJsonObject, QWidget*, LoadedCallback);
    auto factory=reinterpret_cast<Factory>(library.resolve("asset_preview_create_model"));
    if (!factory) throw std::runtime_error(library.errorString().toStdString());
    return factory(std::move(config),parent,std::move(loaded));
}
