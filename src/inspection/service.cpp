#include "../service.h"
#include "document.h"
#include "panel.h"
#include "paths.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QDockWidget>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLabel>
#include <QSet>
#include <QTimer>
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <stdexcept>
#include <sys/stat.h>

namespace {
QJsonObject okay(QJsonObject data={}) { data["ok"]=true; return data; }
QJsonObject failed(const QString& message) { return {{"ok",false},{"error",message}}; }
bool enabled(const Entry& entry) { return entry.config.value("inspection").toObject()["enabled"].toBool(); }
QString reviewPath(const Entry& entry) { return entry.config.value("inspection").toObject()["review"].toString(); }
QString fingerprint(const QString& path) {
    struct stat value{};
    if (::lstat(QFile::encodeName(path).constData(),&value)) {
        if (errno==ENOENT || errno==ENOTDIR) return "missing";
        throw std::runtime_error("Cannot inspect the review file; last valid review retained");
    }
    if (S_ISLNK(value.st_mode)) throw std::runtime_error("Review file symlinks are not supported; last valid review retained");
    return QString("%1:%2:%3:%4:%5:%6:%7:%8").arg(value.st_dev).arg(value.st_ino).arg(value.st_size)
        .arg(value.st_mtim.tv_sec).arg(value.st_mtim.tv_nsec)
        .arg(value.st_ctim.tv_sec).arg(value.st_ctim.tv_nsec).arg(value.st_mode);
}
void checkReviewIdentity(const Entry& entry,const QJsonObject& document) {
    const auto asset=document["asset"].toObject(), options=entry.config.value("inspection").toObject();
    if (asset["id"]!=options["asset_id"] || asset["variant"]!=options["variant"] || asset["lod"].toInteger()!=0 ||
        QDir::cleanPath(asset["model"].toString())!=entry.config["path"].toString())
        throw std::runtime_error("Review asset identity does not match this model, variant and LOD");
}
QJsonObject viewState(PreviewView* view,const QJsonObject& state) {
    QJsonObject result;
    for (const auto& key:{"cuts","hidden","selected","selection","revision","dependency_revision","orthographic"}) if (state.contains(key)) result[key]=state[key];
    if (view) result["camera"]=view->cameraState();
    return result;
}
QString binding(const QJsonObject& note,const QString& revision,const QString& dependencies={}) {
    if (revision.isEmpty()) return "unresolved";
    if (note["created_against"].toString()!=revision) return "stale";
    if (!note.contains("dependency_revision")) return "unresolved";
    if (note.contains("dependency_revision") && note["dependency_revision"].toString()!=dependencies) return "stale";
    return "exact";
}
}

void Service::cacheReview(Entry& entry) {
    const auto path=reviewPath(entry);
    const auto current=path.isEmpty()?QString("missing"):fingerprint(path);
    if (current==entry.reviewStamp) {
        if (!entry.reviewDocument.isEmpty()) checkReviewIdentity(entry,entry.reviewDocument);
        return;
    }
    // One visible review plus one explicit metadata-only query may be cached.
    // Walking inactive registrations must not accumulate sixteen MiB per asset.
    for (const auto& other:entries) if (other.get()!=&entry && other->config["id"].toString()!=inspectionId) {
        other->reviewDocument={}; other->reviewStamp.clear();
    }
    if (current=="missing") {
        if (entry.reviewEstablished) throw std::runtime_error("Review file is missing; last valid revision retained");
        entry.reviewDocument={}; entry.reviewDigest.clear(); entry.reviewStamp=current; return;
    }
    // The cached pair comes only from validated native reads/publications. A
    // changed fingerprint still requires a bounded reread and exact hash check;
    // unchanged bytes need no repeat parse or full schema validation.
    const preview::inspection::DocumentSnapshot validated{entry.reviewDocument,entry.reviewDigest};
    const auto snapshot=preview::inspection::readDocumentSnapshot(path,
        entry.reviewDocument.isEmpty()?nullptr:&validated);
    checkReviewIdentity(entry,snapshot.document);
    // Keep the PRE-read fingerprint. A later pathname stat could describe an
    // external replacement rather than the bytes in this snapshot.
    entry.reviewDocument=snapshot.document; entry.reviewDigest=snapshot.digest; entry.reviewStamp=current;
    entry.reviewEstablished=true; entry.reviewRevision=snapshot.document["document_revision"].toInteger();
}

QJsonObject Service::inspectionState(const QString& id) {
    const auto entry=entries.value(id);
    if (!entry || entry->config["kind"]!="model") return failed("Inspection requires a registered model");
    QJsonObject result{{"supported",true},{"enabled",enabled(*entry)},{"loaded",false},{"selected",""},
        {"part_count",0},{"cuts",QJsonArray{}},{"hidden",QJsonArray{}},{"revision",""}};
    if (enabled(*entry) && entry->active && entry->view && inspectionId==id) {
        QString error;
        const auto state=entry->view->inspect({{"action","state"}},error);
        if (error.isEmpty()) for (auto it=state.begin();it!=state.end();++it) result[it.key()]=it.value();
        else result["backend_error"]=error;
    }
    result["review_path"]=reviewPath(*entry);
    result["asset_id"]=entry->config.value("inspection").toObject()["asset_id"];
    result["variant"]=entry->config.value("inspection").toObject()["variant"];
    try {
        cacheReview(*entry);
        result["document_revision"]=entry->reviewRevision;
        result["review_digest"]=entry->reviewDigest;
        result["note_count"]=entry->reviewDocument.value("annotations").toArray().size();
    } catch (const std::exception& error) {
        result["review_error"]=QString::fromUtf8(error.what());
        result["document_revision"]=entry->reviewRevision; result["review_digest"]=entry->reviewDigest;
    }
    if (QJsonDocument(result).toJson(QJsonDocument::Compact).size()>250*1024)
        return failed("Inspection state exceeds the response limit; restore hidden parts before continuing");
    return okay(result);
}

void Service::replaceInspectionView(const QString& id) {
    const auto entry=entries.value(id);
    if (!entry || !entry->active || !window) return;
    if (entry->view) {
        const auto camera=entry->view->cameraState();
        if (!camera.isEmpty()) { entry->config["camera"]=camera; entry->inspectionView["camera"]=camera; }
    }
    ++entry->token;
    window->removeCard(*entry);
    entry->view=nullptr; entry->surface=nullptr; entry->card=nullptr;
    entry->statusLabel=nullptr; entry->optionsButton=nullptr;
    entry->markerRevision.clear(); entry->markerDigest.clear();
    window->createCard(*entry);
    reload(id);
}

QJsonObject Service::inspectionRequest(const QJsonObject& request) {
    const auto id=request["id"].toString(), action=request["action"].toString();
    const auto entry=entries.value(id);
    if (!entry || entry->config["kind"]!="model") return failed("Inspection requires a registered model");
    if (action=="state") return inspectionState(id);
    if (action=="open") {
        const auto suffix=QFileInfo(entry->config["path"].toString()).suffix().toLower();
        if (suffix!="glb" && suffix!="gltf") return failed("Component inspection currently supports GLB and glTF models");
        if (!ASSET_PREVIEW_HAS_INSPECTION || !QFileInfo::exists(QCoreApplication::applicationDirPath()+"/asset-preview-inspection.so"))
            return failed("The optional inspection backend is unavailable; build with VTK inspection support");
        if (window && !window->commitInspectionControls()) return failed("Could not apply the pending cut adjustment");
        auto options=entry->config.value("inspection").toObject();
        for (const auto& key:{"review","asset_id","variant"}) {
            if (request.contains(key)) {
                const int limit=QString(key)=="review"?4096:QString(key)=="variant"?128:256;
                if (!request[key].isString() || request[key].toString().isEmpty() || request[key].toString().toUtf8().size()>limit)
                    return failed(QString("Invalid inspection %1").arg(key));
                options[key]=request[key];
            }
        }
        if (options["review"].toString().isEmpty()) options["review"]=statePath+"/reviews/"+id+".json";
        if (!QFileInfo(options["review"].toString()).isAbsolute() || QFileInfo(options["review"].toString()).isDir())
            return failed("Review requires an absolute JSON file path");
        options["review"]=QDir::cleanPath(options["review"].toString());
        const auto resolvedReview=preview::inspection::pathIdentity(options["review"].toString());
        if (resolvedReview==preview::inspection::pathIdentity(entry->config["path"].toString())) return failed("Review must be separate from the model output");
        for (const auto& path:entry->config["watch"].toArray())
            if (preview::inspection::pathIdentity(path.toString())==resolvedReview) return failed("Review files must not be generator inputs");
        if (QFileInfo::exists(options["review"].toString())) {
            const auto document=preview::inspection::readDocument(options["review"].toString());
            const auto asset=document["asset"].toObject();
            if (QDir::cleanPath(asset["model"].toString())!=entry->config["path"].toString() || asset["lod"].toInteger()!=0)
                return failed("This review belongs to a different model path or LOD");
            for (const auto& field:{"asset_id","variant"}) {
                const auto key=QString(field)=="asset_id"?QString("id"):QString("variant");
                if (request.contains(field) && request[field]!=asset[key]) return failed("Review identity differs from the requested inspection identity");
                options[field]=asset[key];
            }
        }
        if (options["asset_id"].toString().isEmpty()) options["asset_id"]=id;
        if (options["variant"].toString().isEmpty()) options["variant"]="default";
        options["enabled"]=true;
        const auto oldConfig=entry->config;
        const auto oldComparison=comparison;
        const auto oldId=inspectionId, oldSelected=selected;
        const auto previous=entries.value(oldId);
        const auto previousConfig=previous?previous->config:QJsonObject{};
        if (previous && oldId!=id) { auto prior=previous->config.value("inspection").toObject(); prior["enabled"]=false; previous->config["inspection"]=prior; }
        entry->config["inspection"]=options; inspectionId=id; selected=id; comparison.clear();
        const auto invalid=validate(entry->config);
        if (!invalid.isEmpty()) { entry->config=oldConfig; inspectionId=oldId; selected=oldSelected; comparison=oldComparison; if(previous)previous->config=previousConfig; return failed(invalid); }
        try { save(); } catch (...) { entry->config=oldConfig; inspectionId=oldId; selected=oldSelected; comparison=oldComparison; if(previous)previous->config=previousConfig; throw; }
        const bool switching=oldId!=id || !oldConfig["inspection"].toObject()["enabled"].toBool();
        if (oldConfig["inspection"].toObject()["review"]!=options["review"]) {
            entry->reviewStamp.clear(); entry->reviewDocument={}; entry->reviewDigest.clear();
            entry->reviewEstablished=false; entry->reviewRevision=0;
        }
        if (switching && entry->active) replaceInspectionView(id);
        reconcile(); inspectionEvent(id);
        return inspectionState(id);
    }
    if (action=="close") {
        if (!enabled(*entry)) return inspectionState(id);
        if (window && !window->commitInspectionControls()) return failed("Could not apply the pending cut adjustment");
        const auto old=entry->config;
        const auto oldId=inspectionId;
        if (entry->view && entry->active && oldId==id) {
            QString error; const auto state=entry->view->inspect({{"action","state"}},error);
            if (error.isEmpty()) entry->inspectionView=viewState(entry->view,state);
        }
        auto options=entry->config.value("inspection").toObject(); options["enabled"]=false; entry->config["inspection"]=options;
        if (inspectionId==id) inspectionId.clear();
        try { save(); } catch (...) { entry->config=old; inspectionId=oldId; throw; }
        replaceInspectionView(id); reconcile();
        return inspectionState(id);
    }
    if (!enabled(*entry) || inspectionId!=id || !entry->active || !entry->view)
        return failed("Open this model in a visible Inspect 3D view first");
    const QSet<QString> actions{"parts","select","pick","hide","show","isolate","restore","cut","apply"};
    if (!actions.contains(action)) return failed("Unknown inspection action");
    QString error;
    const auto previousConfig=entry->config;
    QJsonObject previousView;
    if (action=="apply") previousView=viewState(entry->view,entry->view->inspect({{"action","state"}},error));
    if (!error.isEmpty()) return failed(error);
    auto operation=request;
    if (action=="apply") operation["record_undo"]=false;
    const auto result=entry->view->inspect(operation,error);
    if (!error.isEmpty()) return failed(error);
    if (action=="parts") return okay(result);
    if (action=="apply") {
        const auto view=request["view"].toObject();
        if (view.contains("orthographic")) {
            auto settings=entry->config["settings"].toObject(); settings["orthographic"]=view["orthographic"];
            entry->config["settings"]=settings;
        }
        entry->config["camera"]=entry->view->cameraState();
        const auto rollback=[&] {
            entry->config=previousConfig;
            QString ignored; entry->view->inspect({{"action","apply"},{"view",previousView},{"record_undo",false}},ignored);
        };
        const auto invalid=validate(entry->config,true);
        if (!invalid.isEmpty()) { rollback(); return failed(invalid); }
        try { if (entry->config!=previousConfig) save(); } catch (...) {
            rollback();
            throw;
        }
        entry->view->inspect({{"action","commit_hidden"},{"hidden",previousView["hidden"]}},error);
        if (!error.isEmpty()) return failed(error);
    }
    inspectionEvent(id);
    return inspectionState(id);
}

QJsonObject Service::annotationRequest(const QJsonObject& request) {
    const auto id=request["id"].toString(), action=request["action"].toString();
    const auto entry=entries.value(id);
    if (!entry || entry->config["kind"]!="model" || reviewPath(*entry).isEmpty()) return failed("Open a model review before accessing annotations");
    cacheReview(*entry);
    const auto state=inspectionState(id);
    const auto revision=state["revision"].toString();
    if (action=="list") {
        const auto offset=request["offset"].toInt(0), limit=request["limit"].toInt(100);
        if (offset<0 || limit<1 || limit>100 || (request.contains("offset") && request["offset"].toDouble()!=offset) ||
            (request.contains("limit") && request["limit"].toDouble()!=limit)) return failed("Invalid annotation pagination");
        const auto status=request["status"].toString();
        if (!status.isEmpty() && !QSet<QString>{"open","in_progress","resolved","obsolete"}.contains(status)) return failed("Unknown annotation status");
        const auto annotations=entry->reviewDocument.value("annotations").toArray();
        QJsonArray page; qsizetype bytes=0;
        const auto dependencies=state["dependency_revision"].toString();
        const auto append=[&](QJsonObject note) {
            note["binding"]=binding(note,revision,dependencies);
            const auto cost=QJsonDocument(note).toJson(QJsonDocument::Compact).size()+1;
            if (bytes+cost>250*1024) return false;
            page.append(note); bytes+=cost; return true;
        };
        qsizetype total=annotations.size();
        if (status.isEmpty()) {
            for (qsizetype i=offset;i<total && page.size()<limit;++i)
                if (!append(annotations[i].toObject())) break;
        } else {
            total=0;
            bool full=false;
            for (const auto& value:annotations) {
                const auto note=value.toObject();
                if (note["status"]!=status) continue;
                const auto index=total++;
                if (index<offset || full) continue;
                if (page.size()==limit || !append(note)) full=true;
            }
        }
        if (page.isEmpty() && offset<total) return failed("Annotation is too large for a paginated response");
        return okay({{"items",page},{"total",total},{"offset",offset},{"has_more",offset+page.size()<total},
            {"document_revision",entry->reviewDocument.value("document_revision").toInteger()},{"review_digest",entry->reviewDigest}});
    }
    if (action=="restore") {
        if (!state["loaded"].toBool() || !entry->view) return failed("Restoring a comment requires a visible inspection view");
        for (const auto& value:entry->reviewDocument.value("annotations").toArray()) {
            const auto note=value.toObject(); if (note["id"]!=request["note_id"]) continue;
            if (binding(note,revision,state["dependency_revision"].toString())!="exact") return failed("This comment belongs to a different model revision; inspect the current model and create a replacement comment");
            return inspectionRequest({{"id",id},{"action","apply"},{"view",note["view"]}});
        }
        return failed("Unknown annotation");
    }
    if (!QSet<QString>{"add","update","remove"}.contains(action)) return failed("Unknown annotation action");
    if (!request.contains("expected_revision")) return failed("Annotation mutations require expected_revision");
    auto mutation=request; mutation["id"]=request["note_id"];
    if (!entry->reviewDocument.isEmpty()) mutation["asset"]=entry->reviewDocument.value("asset");
    auto note=request["note"].toObject();
    if (action=="add") {
        if (!state["loaded"].toBool() || revision.isEmpty()) return failed("Adding a comment requires a loaded, visible inspection model");
        if (!request["note"].isObject()) return failed("note must be an object");
        if (!note.contains("targets")) {
            const auto target=state["selection"].toObject();
            if (!target.isEmpty() && target["id"].toString().isEmpty()) return failed("This surface has no exact source identity; choose a model comment instead");
            note["targets"]=target.isEmpty()?QJsonArray{}:QJsonArray{target};
        } else if (!note["targets"].isArray() || !note["targets"].toArray().isEmpty())
            return failed("Select a component or supply empty targets for a model comment");
        note["created_against"]=revision;
        note["dependency_revision"]=state["dependency_revision"];
        note["author"]=QJsonObject{{"kind",request["author_kind"].toString("human")},{"name",request["author_name"].toString("local user")}};
        auto view=viewState(entry->view,state);
        view["orthographic"]=entry->config["settings"].toObject()["orthographic"].toBool();
        note["view"]=view;
        const auto options=entry->config.value("inspection").toObject(), settings=entry->config["settings"].toObject();
        const bool zUp=settings["up_axis"].toString("y")=="z";
        const QJsonObject frame{{"up",zUp?"+Z":"+Y"},{"front",settings["front_axis"].toString(zUp?"-y":"+z").toUpper()},{"units","model_units"}};
        mutation["asset"]=QJsonObject{{"id",options["asset_id"]},{"variant",options["variant"]},{"lod",0},
            {"model",entry->config["path"]},{"model_sha256",revision},{"dependency_manifest_sha256",state["dependency_revision"]},{"frame",frame}};
    }
    mutation["note"]=note;
    if (action=="add" && !QFileInfo::exists(reviewPath(*entry)) && !QDir().mkpath(QFileInfo(reviewPath(*entry)).absolutePath()))
        return failed("Cannot create the review directory");
    const auto snapshot=preview::inspection::mutateDocumentSnapshot(reviewPath(*entry),mutation);
    const auto& document=snapshot.document;
    entry->reviewDocument=document; entry->reviewStamp.clear(); entry->reviewDigest=snapshot.digest;
    entry->reviewEstablished=true; entry->reviewRevision=document["document_revision"].toInteger();
    try { cacheReview(*entry); } catch (const std::exception& error) { if (window) window->reportError(QString::fromUtf8(error.what())); }
    updateInspectionMarkers(id);
    if (window) window->refreshInspection();
    auto result=inspectionState(id);
    result["document_revision"]=document["document_revision"];
    if (action=="add") {
        result["note"]=document["annotations"].toArray().last();
        if (QJsonDocument(result).toJson(QJsonDocument::Compact).size()>250*1024) {
            result.remove("note"); result["note_id"]=document["annotations"].toArray().last().toObject()["id"];
        }
    }
    return result;
}

void Service::updateInspectionMarkers(const QString& id) {
    const auto entry=entries.value(id);
    if (!entry || !entry->view || !entry->active || inspectionId!=id) return;
    QString error; const auto state=entry->view->inspect({{"action","state"}},error);
    if (!error.isEmpty() || !state["loaded"].toBool()) return;
    try { cacheReview(*entry); } catch (const std::exception& error) { if(window)window->reportError(QString::fromUtf8(error.what())); return; }
    const auto revision=state["revision"].toString(), dependencies=state["dependency_revision"].toString();
    if (entry->markerRevision==revision+dependencies && entry->markerDigest==entry->reviewDigest) return;
    QJsonArray markers;
    int anchors=0;
    for (const auto& value:entry->reviewDocument.value("annotations").toArray()) {
        const auto note=value.toObject();
        if (binding(note,revision,dependencies)!="exact" || note["status"]=="resolved" || note["status"]=="obsolete") continue;
        QJsonArray targets;
        for (const auto& target:note["targets"].toArray()) {
            if (anchors>=200) break;
            const auto identity=target.toObject();
            if (identity["id"].toString().isEmpty() || identity["anchor"].toObject()["position_local"].toArray().size()!=3) continue;
            targets.append(target); ++anchors;
        }
        if (!targets.isEmpty()) markers.append(QJsonObject{{"id",note["id"]},{"targets",targets}});
        if (anchors>=200) break;
    }
    entry->view->inspect({{"action","markers"},{"items",markers}},error);
    entry->markerRevision=revision+dependencies; entry->markerDigest=entry->reviewDigest;
}

void Service::inspectionEvent(const QString& id) {
    const auto entry=entries.value(id);
    if (!entry || entry->inspectionNotificationPending) return;
    entry->inspectionNotificationPending=true;
    QTimer::singleShot(0,this,[this,id] {
        const auto entry=entries.value(id); if (!entry) return;
        entry->inspectionNotificationPending=false;
        if (!entry->active || !entry->view || inspectionId!=id) return;
        QString error; const auto state=entry->view->inspect({{"action","state"}},error);
        if (error.isEmpty()) entry->inspectionView=viewState(entry->view,state);
        updateInspectionMarkers(id);
        if (window) window->refreshInspection();
    });
}

bool PreviewWindow::commitInspectionControls() {
    return service->inspectionId.isEmpty() || !inspectionPanel || inspectionPanel->commitPendingCuts();
}

void PreviewWindow::refreshInspection() {
    const auto id=service->inspectionId;
    const bool visible=!id.isEmpty() && service->entries.contains(id);
    if (!visible) { if (inspectionDock) inspectionDock->hide(); return; }
    if (!inspectionDock) {
        inspectionDock=new QDockWidget("Inspect 3D",this);
        inspectionDock->setObjectName("inspectionDock");
        inspectionDock->setAllowedAreas(Qt::LeftDockWidgetArea|Qt::RightDockWidgetArea);
        inspectionDock->setFeatures(QDockWidget::DockWidgetMovable);
        inspectionPanel=new InspectionPanel([this](const QJsonObject& request) { return service->request(request); },inspectionDock);
        inspectionPanel->reportError=[this](const QString& error) { reportError(error); };
        inspectionDock->setWidget(inspectionPanel);
        addDockWidget(Qt::RightDockWidgetArea,inspectionDock);
        inspectionDock->setMinimumWidth(280);
        resizeDocks({inspectionDock},{310},Qt::Horizontal);
    }
    inspectionPanel->setState(id,service->inspectionState(id));
    inspectionPanel->setToolMode(service->entries[id]->inspectionTool);
    inspectionDock->setWindowTitle(service->entries[id]->inspectionTool=="cuts"?"Cut mode":"Part mode");
    inspectionDock->show();
}
