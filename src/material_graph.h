#pragma once
#include "viewer.h"
#include <QJsonArray>
#include <QMap>
#include <QSet>
#include <QStringList>
#include <QFileSystemWatcher>
#include <QTimer>

class QGraphicsScene;
class QGraphicsView;
class QLabel;
class QPlainTextEdit;
class QComboBox;
class QPushButton;
class QVBoxLayout;
class QSplitter;

namespace MaterialGraph {
struct Node {
    QString id, label, kind, preview, description, state;
    QJsonObject parameters;
};
struct Edge { QString from, to, label; int fromPort=0, toPort=0; };
struct Document {
    QString path, title, materialId, status;
    QList<Node> nodes;
    QList<Edge> edges;
    QMap<QString,QString> maps;
    QJsonArray operations;
    QStringList dependencies;
    QMap<QString,int> levels;
};
QString sidecar(const QString& path);
bool read(const QString& path, Document& result, QString& error);
QStringList dependencies(const QString& path);
}

// The same presentation is embedded in Asset Preview and the Atlas workbench.
// Sources and bakes remain owned by the material-authoring project.
class MaterialGraphView final : public QWidget, public PreviewView {
public:
    explicit MaterialGraphView(QWidget* parent=nullptr);
    ~MaterialGraphView() override;
    bool reload(const QJsonObject&, QString&) override;
    void fit() override;
    void settings(const QJsonObject&) override;
    QJsonObject metrics() const override;
    QImage snapshot() override;
    const MaterialGraph::Document& document() const { return doc; }
    std::function<void()> documentChanged;
    void selectNode(const QString&);
    void expandNode(const QString&);
    bool previewOnModel(const QString& path);
    QString previewModelPath() const { return modelPath; }
    QJsonObject previewState() const;
    void restorePreviewState(const QJsonObject&);
protected:
    void resizeEvent(QResizeEvent*) override;
private:
    void drawGraph(bool autoFit);
    void inspector();
    void updateMaterial();
    void watchModel();
    MaterialGraph::Document doc;
    QGraphicsScene* scene;
    QGraphicsView* graph;
    QLabel *heading, *summary, *selectionTitle, *image, *previewStatus;
    QPlainTextEdit* details;
    QComboBox* channels;
    QComboBox* targets;
    QSplitter *contentSplit,*inspectorSplit;
    QWidget* materialHost;
    QWidget* inspectorPanel=nullptr;
    QPushButton* fitButton=nullptr;
    QVBoxLayout* materialLayout;
    ModelHandle material{};
    QJsonObject materialConfig;
    QJsonObject requestedPreviewSettings;
    QString materialStamp;
    QString selected;
    QString modelPath;
    QFileSystemWatcher modelWatcher;
    QTimer modelDebounce;
    QSet<QString> expanded;
    QMap<QString,QString> imageStamps;
    QMap<QString,QImage> images;
    int revisions=0;
};
