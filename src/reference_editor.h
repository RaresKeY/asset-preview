#pragma once
#include <QWidget>
#include <QImage>
#include <QPainterPath>
#include <functional>

QString screenshotDirectory();
QString saveReferencePng(const QImage& image, const QString& label, QString& error);

class ReferenceCanvas final : public QWidget {
public:
    explicit ReferenceCanvas(QImage image, QWidget* parent=nullptr);
    enum Tool { Pen, Ellipse, Highlight, Number, Text };
    Tool tool=Ellipse;
    QImage rendered() const;
    void undo();
    int revision() const { return changes; }
protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
private:
    struct Mark { Tool tool; QPainterPath path; QPointF start,end; QString text; int number=0; };
    QImage image;
    QList<Mark> marks;
    bool drawing=false;
    int changes=0;
    QRectF imageRect() const;
    QPointF imagePoint(const QPointF&) const;
    void drawMarks(QPainter&) const;
};
void editReference(QImage image, QString label, QString context,
                   std::function<bool(const QString&,const QString&)> attach, QWidget* parent);
