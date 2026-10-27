#include "inspection/panel.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QGroupBox>
#include <QJsonArray>
#include <QKeyEvent>
#include <QPushButton>
#include <QSlider>
#include <QTreeWidget>
#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>
#include <cmath>
#include <cstdio>
int main(int argc,char** argv) {
    QApplication app(argc,argv);
    const QString sha(64,'a'), dependency(64,'b');
    int cutsSent=0;
    bool rejectCuts=false;
    QJsonObject lastCut;
    QStringList actions;
    QJsonObject snapshot{{"loaded",true},{"revision",sha},{"dependency_revision",dependency},
        {"review_path","/tmp/panel-review.json"},{"document_revision",1},{"review_digest",QString(64,'c')},
        {"bounds",QJsonArray{-1,1,-1,1,-1,1}},{"cuts",QJsonArray{}},{"hidden",QJsonArray{}}};
    const QJsonObject note{{"id","note-one"},{"text","A note without dependency identity"},{"created_against",sha},
        {"binding","unresolved"},{"status","open"}};
    InspectionPanel panel([&](const QJsonObject& request) -> QJsonObject {
        const auto method=request["method"].toString(),action=request["action"].toString();
        actions.append(action);
        if(method=="annotations") return {{"ok",true},{"items",QJsonArray{note}},{"total",1},
            {"has_more",false},{"document_revision",1},{"review_digest",QString(64,'c')}};
        if(action=="parts") return {{"ok",true},{"items",QJsonArray{}},{"total",0},{"has_more",false}};
        if(action=="cut") {
            ++cutsSent;
            lastCut=request;
            if(rejectCuts) return {{"ok",false},{"error","Cut failed"}};
        }
        auto reply=snapshot; reply["ok"]=true; return reply;
    });
    panel.setState("model",snapshot);
    auto* partsGroup=panel.findChild<QGroupBox*>("inspectionPartsGroup");
    auto* cutsGroup=panel.findChild<QGroupBox*>("inspectionCutsGroup");
    if(!partsGroup || !cutsGroup || partsGroup->isHidden() || cutsGroup->isHidden()) return 5;
    panel.setToolMode("cuts");
    if(!partsGroup->isHidden() || cutsGroup->isHidden()) return 6;
    panel.setToolMode("parts");
    if(partsGroup->isHidden() || cutsGroup->isHidden()) return 7;
    QCoreApplication::processEvents();
    auto* tree=panel.findChild<QTreeWidget*>("inspectionNotes");
    auto* restore=panel.findChild<QPushButton*>("inspectionRestoreNote");
    if(!tree || !restore || tree->topLevelItemCount()!=1) return 3;
    tree->setCurrentItem(tree->topLevelItem(0));
    std::printf("initial_note=%s restore_enabled=%d\n",qPrintable(tree->topLevelItem(0)->text(0)),restore->isEnabled());
    auto unloaded=snapshot; unloaded["loaded"]=false; unloaded["revision"]=""; unloaded.remove("dependency_revision");
    panel.setState("model",unloaded);
    panel.setState("model",snapshot);
    QCoreApplication::processEvents();
    std::printf("after_reentry_note=%s restore_enabled=%d\n",qPrintable(tree->topLevelItem(0)->text(0)),restore->isEnabled());
    if(restore->isEnabled() || !tree->topLevelItem(0)->text(0).startsWith("[unresolved]")) return 1;
    // Each axis traverses the exported bounds, publishes an enabled plane and
    // keeps numeric precision independently of the slider's discrete steps.
    auto bounded=snapshot;
    const std::array<double,6> bounds{-4,12,2,10,-2,2};
    bounded["bounds"]=QJsonArray{-4,12,2,10,-2,2};
    const auto defaultBounds=snapshot.value("bounds");
    snapshot["bounds"]=bounded["bounds"];
    panel.setState("model",bounded);
    for(int index=0;index<3;++index) {
        const QString axis(QChar('X'+index));
        auto* slider=panel.findChild<QSlider*>("inspectionCut"+axis+"Slider");
        auto* number=panel.findChild<QDoubleSpinBox*>("inspectionCut"+axis+"Position");
        auto* enabled=panel.findChild<QCheckBox*>("inspectionCut"+axis+"Enabled");
        auto* keep=panel.findChild<QComboBox*>("inspectionCut"+axis+"Keep");
        if(!slider || !number || !enabled || !keep) return 20;
        QKeyEvent arrow(QEvent::ShortcutOverride,Qt::Key_Left,Qt::NoModifier);
        arrow.ignore(); QApplication::sendEvent(slider,&arrow);
        if(!arrow.isAccepted()) return 29;
        for(const auto endpoint:{slider->minimum(),slider->maximum()}) {
            slider->setValue(endpoint);
            const auto expected=bounds[index*2+(endpoint==slider->maximum()?1:0)];
            if(!enabled->isChecked() || number->value()!=expected || !panel.commitPendingCuts()) return 21;
            if(lastCut["axis"]!=axis.toLower() || lastCut["position"]!=expected ||
                !lastCut["enabled"].toBool() || lastCut["keep"]!="positive") return 22;
        }
        enabled->setChecked(false);
        const double precise=bounds[index*2]+.123456;
        number->setValue(precise);
        keep->setCurrentIndex(1);
        if(enabled->isChecked() || std::abs(number->value()-precise)>1e-6 || !panel.commitPendingCuts()) return 23;
        if(lastCut["enabled"].toBool() || lastCut["keep"]!="negative" ||
            std::abs(lastCut["position"].toDouble()-precise)>1e-6) return 24;
        slider->setValue(slider->maximum()/2);
        if(!panel.commitPendingCuts() || !lastCut["enabled"].toBool() || lastCut["keep"]!="negative") return 25;
    }
    panel.findChild<QPushButton*>("inspectionResetCuts")->click();
    if(!panel.commitPendingCuts()) return 26;
    for(const auto& axis:{QString("X"),QString("Y"),QString("Z")}) {
        if(panel.findChild<QCheckBox*>("inspectionCut"+axis+"Enabled")->isChecked() ||
            panel.findChild<QSlider*>("inspectionCut"+axis+"Slider")->value()!=500) return 27;
    }
    auto restored=bounded;
    restored["cuts"]=QJsonArray{QJsonObject{{"axis","x"},{"position",8},{"keep","negative"},{"enabled",false}}};
    const auto beforeRestore=cutsSent;
    panel.setState("model",restored);
    if(cutsSent!=beforeRestore || panel.findChild<QSlider*>("inspectionCutXSlider")->value()!=750 ||
        panel.findChild<QCheckBox*>("inspectionCutXEnabled")->isChecked()) return 28;
    std::printf("cut_sliders=3 bounds=native automatic_enable=ok numeric_precision=ok reset=ok restore=ok\n");
    snapshot["bounds"]=defaultBounds;
    panel.setState("model",snapshot);
    cutsSent=0;
    auto* position=panel.findChild<QDoubleSpinBox*>("inspectionCutXPosition");
    if(!position) return 4;
    panel.setToolMode("cuts");
    position->setValue(.25);
    panel.setToolMode("parts");
    if(cutsSent!=1) return 8;
    panel.setToolMode("cuts");
    position->setValue(.4);
    actions.clear();
    panel.findChild<QPushButton*>("inspectionReturn")->click();
    if(actions!=QStringList{"cut","close"}) return 9;
    position->setValue(.6);
    rejectCuts=true;
    if(panel.setToolMode("parts") || !partsGroup->isHidden() || cutsGroup->isHidden()) return 10;
    position->setValue(.7);
    actions.clear();
    panel.findChild<QPushButton*>("inspectionReturn")->click();
    if(actions!=QStringList{"cut"}) return 11;
    rejectCuts=false;
    cutsSent=0;
    position->setValue(.5);
    snapshot["dependency_revision"]=QString(64,'d');
    panel.setState("model",snapshot);
    QEventLoop loop; QTimer::singleShot(35,&loop,&QEventLoop::quit); loop.exec();
    std::printf("cuts_sent_after_resource_revision_change=%d cut_position=%.1f\n",cutsSent,position->value());
    return cutsSent==0 && position->value()==0?0:2;
}
