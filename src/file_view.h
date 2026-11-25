#pragma once
#include "viewer.h"
#include "file_document.h"
#include <array>
#include <QFont>
class QComboBox;
class QCheckBox;
class QLineEdit;
class QMenu;
class QLabel;
class QTreeWidget;
class QStackedWidget;
class QTextBrowser;
class QSplitter;
class QHBoxLayout;
class CodePane;
namespace FilePreview { QImage thumbnail(const QJsonObject&,QString& error); }

class FileView final : public QWidget, public PreviewView {
public:
    explicit FileView(QWidget* parent=nullptr);
    bool reload(const QJsonObject&,QString&) override;
    void fit() override;
    void settings(const QJsonObject&) override;
    QJsonObject metrics() const override;
    QImage snapshot() override { return grab().toImage(); }
    void setCardWidgets(QWidget* actions,QWidget* status);
    void addNavigationActions(QMenu* menu);
protected:
    void resizeEvent(QResizeEvent*) override;
    void changeEvent(QEvent*) override;
private:
    void render();
    void updateSummary();
    void find(bool backwards=false);
    void hunk(int direction);
    void patch(QJsonObject);
    void alignPair();
    void scheduleAlignment();
    FilePreview::Document doc;
    QJsonObject config;
    QString path;
    QComboBox *mode,*filePicker;
    QCheckBox *wrap,*context;
    QLineEdit* search;
    QLabel *summary,*searchStatus;
    QTreeWidget* files;
    QStackedWidget* pages;
    QTextBrowser* rendered;
    CodePane *source=nullptr,*inlineDiff=nullptr,*left=nullptr,*right=nullptr;
    QSplitter *body,*pair;
    QHBoxLayout* toolbar;
    QWidget* searchTools;
    QWidget* cardActions=nullptr;
    QHBoxLayout* searchToolbar;
    struct ContentStamp {
        int load=-1,file=-1;
        bool context=false;
        bool operator==(const ContentStamp&) const=default;
    };
    std::array<ContentStamp,4> prepared;
    ContentStamp alignedContent;
    QSize alignedWidths;
    std::array<QFont,2> alignedFonts;
    bool alignedWrapping=false;
    int selected=0,loads=0,lineCount=1;
    bool refreshing=false,searchBefore=false;
    bool alignmentPending=false,aligning=false;
};
