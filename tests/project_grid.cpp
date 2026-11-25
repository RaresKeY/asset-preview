#include "project_grid.h"
#include <QApplication>
#include <QCursor>
#include <QEvent>
#include <QJsonObject>
#include <QPointer>
#include <QVBoxLayout>
#include <iostream>
#include <stdexcept>

static void require(bool value,const char* message) { if(!value)throw std::runtime_error(message); }
class Card final : public QWidget {
public:
    using QWidget::QWidget;
    int parentChanges=0;
protected:
    bool event(QEvent* event) override {
        if(event->type()==QEvent::ParentChange)++parentChanges;
        return QWidget::event(event);
    }
};
static QJsonArray project(const QString& current) {
    return {QJsonObject{{"id","project"},{"label","Project"},{"current",current},
        {"entries",QJsonArray{QJsonObject{{"id","first"},{"label","First"}},
                              QJsonObject{{"id","second"},{"label","Second"}}}}}};
}
int main(int argc,char** argv) {
    QApplication app(argc,argv);
    try {
        QWidget host; QVBoxLayout layout(&host); ProjectGrid grid;
        layout.addWidget(&grid); host.resize(640,480); host.show();
        grid.setProjects(project("first"),2); QApplication::processEvents();
        QPointer<QWidget> pane=grid.cardParent("first");
        require(bool(pane),"Project has no card parent");
        QPointer<Card> first=new Card(pane); first->setMinimumSize(100,100);
        grid.attach("first",first); QApplication::processEvents();
        require(first->isVisible(),"Initial card is hidden");
        const int initialChanges=first->parentChanges;

        grid.setProjects(project("second"),2); QApplication::processEvents();
        require(first && first->isHidden(),"Changing current destroyed or displayed the departing card");
        require(first->parentWidget()==pane && first->parentChanges==initialChanges,
                "A member switch unnecessarily reparented the departing card");
        require(grid.describe(&host)["panes"].toArray().first().toObject()["loading"].toBool(),
                "Unadmitted replacement has no loading placeholder");
        delete first.data();
        QPointer<Card> second=new Card(grid.cardParent("second")); second->setMinimumSize(100,100);
        grid.attach("second",second); QApplication::processEvents();
        require(second->isVisible() && !grid.describe(&host)["panes"].toArray().first().toObject()["loading"].toBool(),
                "Replacement card did not publish visibly");

        // Removing a pane must hand its card back before QObject child teardown.
        grid.setProjects({},2); QApplication::processEvents();
        require(!pane && second && second->isHidden(),"Pane removal destroyed a service-owned card");
        require(second->parentWidget()==&host,"Removed pane retained the card parent");
        grid.setProjects(project("second"),2); grid.attach("second",second); QApplication::processEvents();
        require(second->isVisible() && second->parentWidget()==grid.cardParent("second"),
                "Detached card could not reenter a new pane");
        for(bool native:{false,true}) {
            if(native)grid.viewport()->setAttribute(Qt::WA_NativeWindow);
            QApplication::processEvents();
            const auto panes=grid.describe(&host)["panes"].toArray();
            const auto bounds=panes.first().toObject();
            QCursor::setPos(host.mapToGlobal(QPoint{bounds["x"].toInt()+30,bounds["y"].toInt()+100}));
            require(grid.adjacentHovered(-1)=="first","Pointer inside the pane did not target its previous member");
            QCursor::setPos(host.mapToGlobal(QPoint{1,1}));
            require(grid.adjacentHovered(-1).isEmpty(),"Pointer outside the viewport targeted a project");
        }
        delete second.data();
        std::cout<<"Project member retirement, pane ownership and reentry checks passed\n";
    } catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
