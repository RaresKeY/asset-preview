#pragma once
#include "preview_cache.h"
#include <QAbstractScrollArea>
#include <QSet>
#include <QMap>
#include <QTimer>
#include <functional>
#include <memory>
class ThumbnailQueue;

class OverviewGrid final : public QAbstractScrollArea {
public:
    struct Item { QString id,label,path,kind,project,projectLabel; };
    OverviewGrid(preview::PreviewCache*,const QString& jobDirectory,QWidget* parent=nullptr);
    ~OverviewGrid() override;
    void setItems(QList<Item>,const QString& selected,bool grouped=false);
    void select(const QString&,bool reveal=false);
    void setMarked(const QSet<QString>& ids);
    QStringList visibleIds() const;
    QJsonObject describe(QWidget* window) const;
    void invalidate();
    void refresh();
    void warmItems(QList<Item>,QStringList nearby,QSet<QString> active,bool ready,bool enabled);
    QStringList workingIds() const;
    bool backgroundAllowed() const;
    std::function<QString(const QString&)> keyFor;
    std::function<QJsonObject(const QString&)> configFor;
    std::function<void(const QString&)> activated,selected;
    std::function<void(const QString&,Qt::KeyboardModifiers)> marked;
    std::function<void()> viewportChanged;
protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void showEvent(QShowEvent*) override;
    void hideEvent(QHideEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    bool viewportEvent(QEvent*) override;
private:
    int columns() const;
    QRect tile(int index) const;
    QRect checkbox(int index) const;
    void reflow();
    void pruneKeys();
    void updateTile(const QString&);
    void syncDemand();
    void thumbnailState();
    preview::PreviewCache* cache;
    QList<Item> items;
    QString current;
    bool grouped=false;
    QList<QRect> tiles;
    struct Heading { QString id,label; QRect rect; };
    QList<Heading> headings;
    QSet<QString> checked;
    int hover=-1;
    QMap<QString,QString> keys;
    QTimer spinner;
    QSet<QString> loading;
    int spinAngle=0;
    ThumbnailQueue* queue;
    bool hasWarmItems=false,warmReady=false,warmEnabled=false;
    QStringList nearby;
    QSet<QString> active;
};
