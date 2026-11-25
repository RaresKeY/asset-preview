#pragma once
#include "preview_cache.h"
#include <QAbstractScrollArea>
#include <QElapsedTimer>
#include <QSet>
#include <QMap>
#include <QTimer>
#include <functional>
#include <memory>
class ThumbnailQueue;
class QToolButton;

class OverviewGrid final : public QAbstractScrollArea {
public:
    struct Item { QString id,label,path,kind,project,projectLabel,parent={},parentLabel={}; };
    OverviewGrid(preview::PreviewCache*,const QString& jobDirectory,QWidget* parent=nullptr,bool selectionOnly=false);
    ~OverviewGrid() override;
    void setItems(QList<Item>,const QString& selected,bool grouped=false);
    void select(const QString&,bool reveal=false);
    void setMarked(const QSet<QString>& ids);
    void setCompact(bool);
    QStringList visibleIds() const;
    QJsonObject describe(QWidget* window) const;
    void invalidate();
    void refresh();
    void warmItems(QList<Item>,QStringList nearby,QSet<QString> active,bool ready,bool enabled);
    QStringList workingIds() const;
    bool backgroundAllowed() const;
    QString selectedId() const { return current; }
    void selectionDemand(QStringList ids,bool open);
    std::function<QString(const QString&)> keyFor;
    std::function<QJsonObject(const QString&)> configFor;
    std::function<void(const QString&)> activated,selected;
    std::function<void(const QString&)> galleryRequested,closeGroupRequested;
    std::function<void(const QString&,const QPoint&)> organizeRequested;
    std::function<void(const QString&,Qt::KeyboardModifiers)> marked;
    std::function<void()> viewportChanged;
    std::function<void(const QString&)> thumbnailChanged;
protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void showEvent(QShowEvent*) override;
    void hideEvent(QHideEvent*) override;
    void scrollContentsBy(int dx,int dy) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void contextMenuEvent(QContextMenuEvent*) override;
    bool viewportEvent(QEvent*) override;
private:
    int columns() const;
    QRect tile(int index) const;
    QRect paintArea(int index) const;
    QRectF spinnerRect(int index) const;
    QRect checkbox(int index) const;
    struct ScrollAnchor { QString id; int top=0; };
    ScrollAnchor scrollAnchor(const QSet<QString>& surviving={}) const;
    void reflow(const ScrollAnchor&);
    void syncHeadingButtons();
    void pruneKeys();
    void updateTile(const QString&);
    void updateHover(QPoint);
    void syncDemand();
    void thumbnailState();
    preview::PreviewCache* cache;
    QList<Item> items;
    QString current;
    bool grouped=false,compact=false;
    bool selectionOnly=false,selectionOpen=false;
    QStringList selectionIds;
    QList<QRect> tiles;
    struct Heading { QString id,label; QRect rect; bool nested=false; };
    QList<Heading> headings;
    struct HeadingButtons { QToolButton* gallery=nullptr; QToolButton* close=nullptr; };
    QMap<QString,HeadingButtons> headingButtons;
    int layoutColumns=1;
    bool reflowing=false;
    QSet<QString> checked;
    int hover=-1;
    QPoint hoverPosition{-1,-1};
    QMap<QString,QString> keys;
    QTimer spinner;
    QElapsedTimer spinClock;
    QSet<QString> loading;
    ThumbnailQueue* queue;
    bool hasWarmItems=false,warmReady=false,warmEnabled=false;
    QStringList nearby;
    QSet<QString> active;
};
