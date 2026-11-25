#include "selection_document.h"
#include <QDir>
#include "file_document.h"
#include "inspection/paths.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <sys/stat.h>

namespace Selection {
namespace {
QString hash(const QJsonObject& object) {
    return QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(object).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
}
bool identifier(const QString& id) {
    static const QRegularExpression valid("^[A-Za-z0-9_-]{1,96}$");
    return valid.match(id).hasMatch();
}
bool text(const QJsonObject& obj, const QString& key, int bound, bool required=false) {
    return (!obj.contains(key) && !required) || (obj[key].isString() && obj[key].toString().size()<=bound && (!required || !obj[key].toString().trimmed().isEmpty()));
}
bool mediaPath(const QJsonObject& item,const QString& key,const QString& manifest,QString& path) {
    if (!item.contains(key)) return true;
    if (!text(item,key,4096,true)) return false;
    const auto value=item[key].toString();
    for (const auto c:value) if (c.category()==QChar::Other_Control || c==':') return false;
    path=QDir::cleanPath(QFileInfo(manifest).absoluteDir().absoluteFilePath(value));
    static const QSet<QString> models{"glb","gltf","obj","stl","ply","fbx"};
    return isImagePath(path) || (key=="asset" && models.contains(QFileInfo(path).suffix().toLower()));
}
bool safeOutput(const QString& path, const Document& doc, QString& error) {
    if (!QFileInfo(path).isAbsolute() || path.size()>4096 ||
        preview::inspection::pathIdentity(path)==preview::inspection::pathIdentity(doc.path)) {
        error="Decisions need a separate absolute file path"; return false;
    }
    struct stat info{};
    if (::lstat(QFile::encodeName(path).constData(),&info)==0 && (!S_ISREG(info.st_mode) || info.st_nlink!=1)) {
        error="Decisions must be a regular file, without symlinks or hard links"; return false;
    }
    if (!QFileInfo(QFileInfo(path).absolutePath()).isDir()) { error="Decisions directory does not exist"; return false; }
    return true;
}
}
QString defaultDecisionsPath(const QString& path) {
    return path.endsWith(".selection.json", Qt::CaseInsensitive) ? path.left(path.size()-15)+".decisions.json" : path+".decisions.json";
}
bool isImagePath(const QString& path) {
    static const QSet<QString> images{"png","jpg","jpeg","webp","bmp"};
    return images.contains(QFileInfo(path).suffix().toLower());
}
bool load(const QString& path, Document& output, QString& error) {
    QString source;
    if (!FilePreview::readText(path, source, error)) return false;
    QJsonParseError parse;
    auto json=QJsonDocument::fromJson(source.toUtf8(),&parse);
    if (parse.error!=QJsonParseError::NoError || !json.isObject()) { error="Selection must be a JSON object"; return false; }
    const auto root=json.object();
    if (root["schema"]!="asset-preview-selection/v1" || !identifier(root["id"].toString()) ||
        !text(root,"title",256,true) || !text(root,"markdown",32768)) {
        error="Selection needs schema asset-preview-selection/v1, a stable id and title"; return false;
    }
    if (root.contains("items") && root.contains("sections")) { error="Use items or sections, not both"; return false; }
    const auto presentation=root.value("presentation");
    if (!presentation.isUndefined() && presentation!="list" && presentation!="variant-rows") {
        error="Selection presentation must be list or variant-rows"; return false;
    }
    QJsonArray sections;
    if (root.contains("items")) {
        if (!root["items"].isArray()) { error="Selection items must be an array"; return false; }
        sections.append(QJsonObject{{"id","options"},{"title","Options"},{"items",root["items"]}});
    } else {
        if (!root["sections"].isArray()) { error="Selection needs items or sections"; return false; }
        sections=root["sections"].toArray();
    }
    if (sections.size()>32) { error="Selection supports at most 32 sections"; return false; }
    Document doc; doc.id=root["id"].toString(); doc.title=root["title"].toString();
    doc.markdown=root["markdown"].toString(); doc.path=path; doc.digest=hash(root);
    doc.variantRows=presentation=="variant-rows";
    QSet<QString> ids,sectionIds;
    for (const auto& s:sections) {
        if (!s.isObject()) { error="Each section must be an object"; return false; }
        const auto section=s.toObject(); const auto sectionId=section["id"].toString();
        if (!identifier(sectionId) || sectionIds.contains(sectionId) || !text(section,"title",256,true) ||
            !text(section,"markdown",32768) || !section["items"].isArray()) { error="Invalid or duplicate selection section"; return false; }
        sectionIds.insert(sectionId);
        for (const auto& v:section["items"].toArray()) {
            if (!v.isObject()) { error="Each option must be an object"; return false; }
            const auto item=v.toObject(); const auto id=item["id"].toString();
            if (!identifier(id) || ids.contains(id) || !text(item,"title",256,true) ||
                !text(item,"markdown",32768) || !text(item,"recommendation",256)) { error="Invalid or duplicate option: "+id; return false; }
            QString image,asset;
            if (!mediaPath(item,"image",path,image) || !mediaPath(item,"asset",path,asset)) {
                error="Invalid local preview path for option: "+id; return false;
            }
            ids.insert(id);
            // Bind a decision to its option and surrounding explanation, while
            // unrelated options can be revised without erasing the user's work.
            auto content=item;
            content.remove("image"); content.remove("asset");
            const auto digest=hash({{"id",doc.id},{"title",doc.title},{"markdown",doc.markdown},
                {"section_id",sectionId},{"section_title",section["title"]},{"section_markdown",section["markdown"]},{"item",content}});
            doc.items.append({id,item["title"].toString(),item["markdown"].toString(),item["recommendation"].toString(),digest,
                sectionId,section["title"].toString(),section["markdown"].toString(),image,asset});
            if (doc.items.size()>256) { error="Selection supports at most 256 options"; return false; }
        }
    }
    output=std::move(doc); return true;
}
bool readDecisions(const QString& path, const Document& doc, QJsonObject& output, QString& error) {
    if (!safeOutput(path,doc,error)) return false;
    if (!QFileInfo::exists(path)) {
        output={{"schema","asset-preview-decisions/v1"},{"selection_id",doc.id},{"decisions",QJsonObject{}}}; return true;
    }
    QString source;
    if (!FilePreview::readText(path,source,error)) return false;
    QJsonParseError parse;
    const auto json=QJsonDocument::fromJson(source.toUtf8(),&parse);
    const auto root=json.object();
    if (parse.error!=QJsonParseError::NoError || !json.isObject() || root["schema"]!="asset-preview-decisions/v1" ||
        root["selection_id"]!=doc.id || !root["decisions"].isObject() || root["decisions"].toObject().size()>2048) {
        error="Invalid decisions file or selection identity; existing results were preserved"; return false;
    }
    const auto rows=root["decisions"].toObject();
    static const QRegularExpression digest("^[a-f0-9]{64}$");
    for (auto it=rows.begin();it!=rows.end();++it) {
        const auto row=it.value().toObject(); const auto value=row["decision"].toString();
        if (!identifier(it.key()) || !it.value().isObject() || !digest.match(row["digest"].toString()).hasMatch() ||
            (value!="accepted" && value!="denied" && value!="pending")) { error="Invalid decision record; existing results were preserved"; return false; }
    }
    output=root; return true;
}
QString decision(const QJsonObject& results, const Item& item) {
    const auto row=results["decisions"].toObject()[item.id].toObject();
    return row["digest"]==item.digest ? row["decision"].toString("pending") : "pending";
}
bool writeDecision(const QString& path, const Document& doc, const QString& id, const QString& value, QJsonObject& output, QString& error) {
    if (value!="accepted" && value!="denied" && value!="pending") { error="Invalid decision"; return false; }
    const Item* item=nullptr;
    for (const auto& current:doc.items) if (current.id==id) { item=&current; break; }
    if (!item) { error="Unknown option"; return false; }
    if (!safeOutput(path,doc,error)) return false;
    QLockFile lock(path+".lock");
    if (!lock.tryLock(0)) { error="Decisions are being saved by another viewer; try again"; return false; }
    Document current;
    if (!load(doc.path,current,error)) return false;
    if (current.digest!=doc.digest) { error="Options changed; reload before deciding"; return false; }
    QJsonObject result;
    if (!readDecisions(path,doc,result,error)) return false;
    auto rows=result["decisions"].toObject();
    rows[id]=QJsonObject{{"digest",item->digest},{"decision",value},{"updated_at",QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},{"actor","user"}};
    result["decisions"]=rows; result["manifest_digest"]=doc.digest;
    QSaveFile file(path); file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)) { error=file.errorString(); return false; }
    file.setPermissions(QFileDevice::ReadOwner|QFileDevice::WriteOwner);
    const auto data=QJsonDocument(result).toJson();
    if (data.size()>FilePreview::MaxBytes || file.write(data)!=data.size() || !file.commit()) { error="Could not save decisions; prior choices were preserved"; return false; }
    output=std::move(result); return true;
}
}
