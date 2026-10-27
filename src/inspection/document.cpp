#include "document.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QLockFile>
#include <QSaveFile>
#include <QSet>
#include <QUuid>

#include <cmath>
#include <limits>
#include <stdexcept>

namespace preview::inspection {
namespace {

constexpr int MaximumDepth = 32;
constexpr int MaximumStringBytes = 64 * 1024;
constexpr int MaximumArrayItems = 65536;
constexpr int MaximumObjectKeys = 1024;
constexpr int MaximumJsonValues = 1000000;
constexpr int MaximumVisibleTargets = 8192;
constexpr qint64 MaximumRevision = 9007199254740991LL;
const QString Schema = QStringLiteral("asset-preview-review/v1");

[[noreturn]] void fail(const QString& message) {
    throw std::runtime_error(message.toStdString());
}

QString stringValue(const QJsonValue& value, const QString& field, int bytes,
                    bool allowEmpty = false) {
    if (!value.isString()) fail(field + " must be a string");
    const QString text = value.toString();
    if (text.size() > bytes || text.toUtf8().size() > bytes)
        fail(field + " exceeds its UTF-8 byte limit");
    if (text.contains(QChar(u'\0'))) fail(field + " contains a null character");
    if (!allowEmpty && text.trimmed().isEmpty()) fail(field + " must not be empty");
    return text;
}

qint64 integerValue(const QJsonValue& value, const QString& field, qint64 minimum,
                    qint64 maximum = MaximumRevision) {
    if (!value.isDouble()) fail(field + " must be an integer");
    const double number = value.toDouble();
    if (!std::isfinite(number) || std::floor(number) != number ||
        number < static_cast<double>(minimum) || number > static_cast<double>(maximum))
        fail(field + " is outside its integer range");
    return static_cast<qint64>(number);
}

QJsonObject objectValue(const QJsonValue& value, const QString& field) {
    if (!value.isObject()) fail(field + " must be an object");
    return value.toObject();
}

QJsonArray arrayValue(const QJsonValue& value, const QString& field, qsizetype maximum) {
    if (!value.isArray()) fail(field + " must be an array");
    const auto array = value.toArray();
    if (array.size() > maximum) fail(field + " has too many items");
    return array;
}

QString digestValue(const QJsonValue& value, const QString& field) {
    const auto digest = stringValue(value, field, 64);
    if (digest.size() != 64) fail(field + " must be a SHA-256 digest");
    for (const auto character : digest) {
        const auto c = character.unicode();
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F')))
            fail(field + " must be a SHA-256 digest");
    }
    return digest;
}

void boundedValue(const QJsonValue& value, int depth, int& remaining) {
    if (depth > MaximumDepth) fail("Review JSON exceeds the nesting limit");
    if (--remaining < 0) fail("Review JSON has too many values");
    if (value.isObject()) {
        const auto object = value.toObject();
        if (object.size() > MaximumObjectKeys) fail("Review JSON object has too many keys");
        for (auto it = object.begin(); it != object.end(); ++it) {
            stringValue(it.key(), "JSON key", 256);
            boundedValue(it.value(), depth + 1, remaining);
        }
    } else if (value.isArray()) {
        const auto array = arrayValue(value, "JSON array", MaximumArrayItems);
        for (const auto& item : array) boundedValue(item, depth + 1, remaining);
    } else if (value.isString()) {
        stringValue(value, "JSON string", MaximumStringBytes, true);
    } else if (value.isDouble() && !std::isfinite(value.toDouble())) {
        fail("Review JSON numbers must be finite");
    } else if (value.isUndefined()) {
        fail("Review JSON must not contain undefined values");
    }
}

void boundedJson(const QJsonObject& object) {
    int remaining = MaximumJsonValues;
    boundedValue(object, 0, remaining);
}

// Check structural depth before the Qt parser allocates nested JSON containers.
void preflightJson(const QByteArray& bytes) {
    int depth = 0;
    bool quoted = false;
    bool escaped = false;
    for (const char c : bytes) {
        if (quoted) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') quoted = false;
        } else if (c == '"') {
            quoted = true;
        } else if (c == '{' || c == '[') {
            if (++depth > MaximumDepth + 1) fail("Review JSON exceeds the nesting limit");
        } else if (c == '}' || c == ']') {
            --depth;
        }
    }
}

QString documentPath(const QString& requested) {
    if (requested.isEmpty() || requested.contains(QChar(u'\0')) ||
        requested.toUtf8().size() > 4096 || !QDir::isAbsolutePath(requested))
        fail("Review path must be an explicit absolute file path");
    const QFileInfo info(QDir::cleanPath(requested));
    if (info.isSymLink()) fail("Review path must not be a symbolic link");
    const auto parent = QFileInfo(info.absolutePath()).canonicalFilePath();
    if (parent.isEmpty() || !QFileInfo(parent).isDir())
        fail("Review parent directory does not exist");
    const auto path = QDir(parent).filePath(info.fileName());
    const QFileInfo target(path);
    if (target.isSymLink() || (target.exists() && !target.isFile()))
        fail("Review path must name a regular file");
    return path;
}

QByteArray readBytes(const QString& path) {
    const QFileInfo info(path);
    if (!info.exists() || !info.isFile() || info.isSymLink())
        fail("Review document is missing or is not a regular file");
    if (info.size() > MaximumDocumentBytes) fail("Review document exceeds 16 MiB");
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) fail("Cannot read review: " + file.errorString());
    const auto bytes = file.read(MaximumDocumentBytes + 1);
    if (file.error() != QFileDevice::NoError) fail("Cannot read review: " + file.errorString());
    if (bytes.size() > MaximumDocumentBytes || !file.atEnd())
        fail("Review document exceeds 16 MiB");
    return bytes;
}

QString sha256(const QByteArray& bytes) {
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

void vectorValue(const QJsonValue& value, const QString& field, bool nonzero = false) {
    const auto vector = arrayValue(value, field, 3);
    if (vector.size() != 3) fail(field + " must contain three numbers");
    double length = 0;
    for (const auto& element : vector) {
        if (!element.isDouble() || !std::isfinite(element.toDouble()) ||
            std::abs(element.toDouble()) > 1e12)
            fail(field + " contains an invalid coordinate");
        length += element.toDouble() * element.toDouble();
    }
    if (nonzero && length < 1e-18) fail(field + " must not be zero");
}

void validateAsset(const QJsonObject& asset) {
    stringValue(asset["id"], "asset.id", 512);
    stringValue(asset["variant"], "asset.variant", 128);
    integerValue(asset["lod"], "asset.lod", 0, 255);
    stringValue(asset["model"], "asset.model", 4096);
    digestValue(asset["model_sha256"], "asset.model_sha256");
    if (asset.contains("dependency_manifest_sha256"))
        digestValue(asset["dependency_manifest_sha256"], "asset.dependency_manifest_sha256");
    if (asset.contains("dependency_manifest") && !asset["dependency_manifest"].isObject())
        stringValue(asset["dependency_manifest"], "asset.dependency_manifest", 4096);
    const auto frame = objectValue(asset["frame"], "asset.frame");
    const auto up = stringValue(frame["up"], "asset.frame.up", 2).toUpper();
    const auto front = stringValue(frame["front"], "asset.frame.front", 2).toUpper();
    const QSet<QString> axes = {"+X", "-X", "+Y", "-Y", "+Z", "-Z"};
    if (!axes.contains(up) || !axes.contains(front) || up[1] == front[1])
        fail("asset.frame axes must be valid and perpendicular");
    stringValue(frame["units"], "asset.frame.units", 64);
}

void validateTarget(const QJsonObject& target) {
    bool hasIdentity = false;
    for (const auto* key : {"id", "path", "instance_selector"}) {
        if (target.contains(key)) {
            stringValue(target[key], QString("target.") + key, 4096);
            hasIdentity = true;
        }
    }
    if (!hasIdentity) fail("Target requires an exact id or instance path");
    for (const auto* key : {"name", "exported_name", "semantic_id"}) {
        if (target.contains(key))
            stringValue(target[key], QString("target.") + key, 2048, true);
    }
    if (target.contains("node_index"))
        integerValue(target["node_index"], "target.node_index", 0, std::numeric_limits<int>::max());
    if (target.contains("anchor")) {
        const auto anchor = objectValue(target["anchor"], "target.anchor");
        const auto kind = stringValue(anchor["kind"], "target.anchor.kind", 64);
        if (anchor.contains("position_local"))
            vectorValue(anchor["position_local"], "target.anchor.position_local");
        if (anchor.contains("normal_local"))
            vectorValue(anchor["normal_local"], "target.anchor.normal_local", true);
        if (kind == "surface_point" && !anchor.contains("position_local"))
            fail("Surface-point anchor requires position_local");
    }
}

void targetReferences(const QJsonValue& value, const QString& field, int maximum) {
    for (const auto& item : arrayValue(value, field, maximum)) {
        if (item.isString()) stringValue(item, field + " target", 4096);
        else validateTarget(objectValue(item, field + " target"));
    }
}

void validateView(const QJsonObject& view) {
    if (view.contains("camera")) {
        const auto camera = objectValue(view["camera"], "view.camera");
        for (const auto* key : {"position", "focal", "up"})
            vectorValue(camera[key], QString("view.camera.") + key, QString(key) == "up");
        const auto angle = camera["angle"];
        const auto span = camera["parallel_span"];
        if (!angle.isDouble() || !std::isfinite(angle.toDouble()) || angle.toDouble() <= 0 ||
            angle.toDouble() >= 180 || !span.isDouble() || !std::isfinite(span.toDouble()) ||
            span.toDouble() <= 0 || span.toDouble() > 1e12)
            fail("Invalid view.camera scale");
        double distance = 0;
        for (int i = 0; i < 3; ++i) {
            const double d = camera["position"].toArray()[i].toDouble() -
                             camera["focal"].toArray()[i].toDouble();
            distance += d * d;
        }
        if (distance < 1e-18) fail("Invalid view.camera position");
    }
    if (view.contains("orthographic") && !view["orthographic"].isBool())
        fail("view.orthographic must be boolean");
    if (view.contains("cuts")) {
        for (const auto& item : arrayValue(view["cuts"], "view.cuts", 6)) {
            const auto cut = objectValue(item, "view cut");
            if (cut.contains("enabled") && !cut["enabled"].isBool())
                fail("Cut enabled must be boolean");
            if (cut.contains("axis")) {
                const auto axis = stringValue(cut["axis"], "cut.axis", 1).toLower();
                if (axis != "x" && axis != "y" && axis != "z") fail("Invalid cut axis");
                const auto position = cut["position"];
                if (!position.isDouble() || !std::isfinite(position.toDouble()) ||
                    std::abs(position.toDouble()) > 1e12) fail("Invalid cut position");
            } else {
                vectorValue(cut["normal"], "cut.normal", true);
                vectorValue(cut["origin"], "cut.origin");
            }
            if (cut.contains("keep")) {
                const auto keep = stringValue(cut["keep"], "cut.keep", 16);
                if (keep != "positive" && keep != "negative") fail("Invalid cut keep side");
            }
        }
    }
    for (const auto* key : {"hidden", "hidden_targets"}) {
        if (view.contains(key)) targetReferences(view[key], QString("view.") + key, MaximumVisibleTargets);
    }
    if (view.contains("selected")) {
        if (view["selected"].isString())
            stringValue(view["selected"], "view.selected", 4096, true);
        else if (view["selected"].isArray())
            targetReferences(view["selected"], "view.selected", MaximumAnnotationTargets);
        else validateTarget(objectValue(view["selected"], "view.selected"));
    }
}

QDateTime utcTime(const QJsonValue& value, const QString& field) {
    const auto string = stringValue(value, field, 40);
    const auto time = QDateTime::fromString(string, Qt::ISODateWithMs);
    if (!string.endsWith('Z') || !time.isValid() || time.offsetFromUtc() != 0)
        fail(field + " must be an ISO 8601 UTC timestamp");
    return time;
}

void validateNote(const QJsonObject& note) {
    auto id = stringValue(note["id"], "note.id", 64);
    if (id.startsWith("note-")) id.remove(0, 5);
    if (QUuid::fromString(id).isNull()) fail("note.id must contain a nonzero UUID");
    const auto author = objectValue(note["author"], "note.author");
    const auto kind = stringValue(author["kind"], "note.author.kind", 16);
    if (kind != "human" && kind != "agent") fail("Unknown note author kind");
    stringValue(author["name"], "note.author.name", 256);
    const auto created = utcTime(note["created_at"], "note.created_at");
    const auto updated = utcTime(note["updated_at"], "note.updated_at");
    if (updated < created) fail("note.updated_at precedes creation");
    const auto status = stringValue(note["status"], "note.status", 32);
    if (status != "open" && status != "in_progress" && status != "resolved" && status != "obsolete")
        fail("Unknown note workflow status");
    stringValue(note["text"], "note.text", MaximumAnnotationTextBytes);
    digestValue(note["created_against"], "note.created_against");
    if (note.contains("dependency_revision"))
        digestValue(note["dependency_revision"], "note.dependency_revision");
    if (note.contains("tags")) {
        for (const auto& tag : arrayValue(note["tags"], "note.tags", 32))
            stringValue(tag, "note tag", 128);
    }
    for (const auto& target : arrayValue(note["targets"], "note.targets", MaximumAnnotationTargets))
        validateTarget(objectValue(target, "note target"));
    validateView(objectValue(note["view"], "note.view"));
    if (note.contains("resolution") && !note["resolution"].isNull()) {
        const auto resolution = objectValue(note["resolution"], "note.resolution");
        if (resolution.contains("resolved_against"))
            digestValue(resolution["resolved_against"], "resolution.resolved_against");
        if (resolution.contains("evidence")) {
            const auto evidence = resolution["evidence"];
            if (evidence.isString()) stringValue(evidence, "resolution.evidence", MaximumAnnotationTextBytes);
            else {
                for (const auto& item : arrayValue(evidence, "resolution.evidence", 32)) {
                    if (item.isString()) stringValue(item, "resolution evidence", 4096);
                    else objectValue(item, "resolution evidence");
                }
            }
        }
    }
}

void validateLayers(const QJsonValue& value) {
    for (const auto& item : arrayValue(value, "layer_presets", 128)) {
        const auto layer = objectValue(item, "layer preset");
        stringValue(layer["name"], "layer preset name", 256);
        if (layer.contains("targets")) targetReferences(layer["targets"], "layer preset targets", MaximumVisibleTargets);
        else if (layer.contains("members")) targetReferences(layer["members"], "layer preset members", MaximumVisibleTargets);
        else fail("Layer preset requires explicit targets or members");
    }
}

void validateDocument(const QJsonObject& document) {
    if (document["schema"] != Schema) fail("Unsupported review document schema");
    integerValue(document["document_revision"], "document_revision", 1);
    const auto notes = arrayValue(document["annotations"], "annotations", MaximumAnnotations);
    if (document.contains("_digest")) fail("Transient _digest must not be persisted");
    boundedJson(document);
    validateAsset(objectValue(document["asset"], "asset"));
    QSet<QString> ids;
    for (const auto& item : notes) {
        const auto note = objectValue(item, "annotation");
        validateNote(note);
        const auto id = note["id"].toString();
        if (ids.contains(id)) fail("Review document contains duplicate note IDs");
        ids.insert(id);
    }
    if (document.contains("layer_presets")) validateLayers(document["layer_presets"]);
}

QJsonObject parseDocument(const QByteArray& bytes) {
    preflightJson(bytes);
    QJsonParseError error;
    const auto json = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || !json.isObject())
        fail("Invalid review JSON: " + error.errorString());
    const auto document = json.object();
    validateDocument(document);
    return document;
}

void checkAssetIdentity(const QJsonObject& original, const QJsonObject& supplied) {
    validateAsset(supplied);
    for (const auto* key : {"id", "variant", "lod", "model"}) {
        if (original[key] != supplied[key]) fail("Review asset identity conflict: " + QString(key));
    }
}

QString escapedMarkdown(QString text) {
    text.replace('&', "&amp;");
    text.replace('<', "&lt;");
    text.replace('>', "&gt;");
    const QString punctuation = "\\`*_{}[]()#+-.!|";
    QString result;
    result.reserve(text.size());
    for (const auto character : text) {
        if (character.unicode() < 32 || character.unicode() == 127) {
            result += QString("\\\\u%1").arg(static_cast<uint>(character.unicode()), 4, 16, QChar('0'));
            continue;
        }
        if (punctuation.contains(character)) result += '\\';
        result += character;
    }
    return result;
}

QString jsonBlock(const QJsonObject& object) {
    auto json = QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Indented));
    json.replace(QChar(127), "\\u007f");
    return "```json\n" + json + "```\n\n";
}

} // namespace

DocumentSnapshot readDocumentSnapshot(const QString& path, const DocumentSnapshot* validated) {
    const auto bytes = readBytes(documentPath(path));
    const auto digest = sha256(bytes);
    if (validated && validated->digest == digest) return *validated;
    return {parseDocument(bytes), digest};
}

QJsonObject readDocument(const QString& path) {
    return readDocumentSnapshot(path).document;
}

DocumentSnapshot mutateDocumentSnapshot(const QString& requestedPath, const QJsonObject& request) {
    boundedJson(request);
    if (QJsonDocument(request).toJson(QJsonDocument::Compact).size() > MaximumDocumentBytes)
        fail("Review mutation exceeds 16 MiB");
    const auto action = stringValue(request["action"], "action", 32);
    if (action != "add" && action != "update" && action != "remove" && action != "layers")
        fail("Unknown review mutation action");
    const auto expected = integerValue(request["expected_revision"], "expected_revision", 0);
    QString expectedDigest;
    if (request.contains("expected_digest")) {
        const auto supplied = request["expected_digest"];
        if (!(expected == 0 && supplied.isString() && supplied.toString().isEmpty()))
            expectedDigest = digestValue(supplied, "expected_digest").toLower();
    }
    const auto path = documentPath(requestedPath);
    QLockFile lock(path + ".lock");
    lock.setStaleLockTime(0);
    if (!lock.tryLock(1000)) fail("Review document lock conflict or inaccessible lock file");

    const bool existed = QFileInfo::exists(path);
    QByteArray previous;
    QJsonObject document;
    if (existed) {
        previous = readBytes(path);
        document = parseDocument(previous);
        if (integerValue(document["document_revision"], "document_revision", 1) != expected)
            fail("Review document revision conflict");
        if (!expectedDigest.isEmpty() && sha256(previous) != expectedDigest)
            fail("Review document digest conflict");
        if (request.contains("asset"))
            checkAssetIdentity(document["asset"].toObject(), objectValue(request["asset"], "asset"));
    } else {
        if (action != "add" || expected != 0 || !expectedDigest.isEmpty())
            fail("Missing review requires add with expected_revision zero and no expected_digest");
        const auto asset = objectValue(request["asset"], "asset");
        validateAsset(asset);
        document = {{"schema", Schema}, {"asset", asset}, {"annotations", QJsonArray{}},
                    {"layer_presets", QJsonArray{}}};
    }
    if (expected == MaximumRevision) fail("Review document revision limit reached");

    auto annotations = document["annotations"].toArray();
    if (action == "add") {
        if (annotations.size() >= MaximumAnnotations) fail("Review annotation limit reached");
        auto note = objectValue(request["note"], "note");
        for (const auto* key : {"id", "created_at", "updated_at"}) {
            if (note.contains(key)) fail("New note " + QString(key) + " is generated by the document store");
        }
        note["id"] = "note-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
        const auto now = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
        note["created_at"] = now;
        note["updated_at"] = now;
        if (!note.contains("status")) note["status"] = "open";
        if (!note.contains("targets")) note["targets"] = QJsonArray{};
        if (!note.contains("view")) note["view"] = QJsonObject{};
        validateNote(note);
        annotations.append(note);
    } else if (action == "update" || action == "remove") {
        const auto id = stringValue(request["id"], "id", 64);
        int index = -1;
        for (qsizetype i = 0; i < annotations.size(); ++i) {
            if (annotations[i].toObject()["id"] == id) { index = static_cast<int>(i); break; }
        }
        if (index < 0) fail("Unknown annotation ID");
        if (action == "remove") annotations.removeAt(index);
        else {
            auto original = annotations[index].toObject();
            auto patch = objectValue(request["note"], "note");
            for (const auto* key : {"id", "author", "created_at", "created_against", "dependency_revision"}) {
                if (patch.contains(key) && patch[key] != original[key])
                    fail("Cannot change note creation field: " + QString(key));
            }
            patch.remove("updated_at");
            for (auto it = patch.begin(); it != patch.end(); ++it) original[it.key()] = it.value();
            // Do not move updated_at backwards if the host clock was adjusted.
            const auto previousTime = utcTime(original["updated_at"], "note.updated_at");
            const auto now = QDateTime::currentDateTimeUtc();
            original["updated_at"] = (now < previousTime ? previousTime : now).toString(Qt::ISODateWithMs);
            validateNote(original);
            annotations[index] = original;
        }
    } else {
        validateLayers(request["layer_presets"]);
        document["layer_presets"] = request["layer_presets"];
    }
    document["annotations"] = annotations;
    document["document_revision"] = expected + 1;
    validateDocument(document);
    const auto bytes = QJsonDocument(document).toJson(QJsonDocument::Indented);
    if (bytes.size() > MaximumDocumentBytes) fail("Review document exceeds 16 MiB");

    QSaveFile output(path);
    output.setDirectWriteFallback(false);
    if (!output.open(QIODevice::WriteOnly)) fail("Cannot save review: " + output.errorString());
    if (output.write(bytes) != bytes.size()) fail("Cannot write review: " + output.errorString());
    // Cooperating writers use the lock. Detect noncooperating edits made while
    // validating or staging this mutation before the atomic replacement.
    if (existed != QFileInfo::exists(path) || (existed && readBytes(path) != previous))
        fail("Review document changed during save");
    if (!output.commit()) fail("Cannot commit review: " + output.errorString());
    return {document, sha256(bytes)};
}

QJsonObject mutateDocument(const QString& path, const QJsonObject& request) {
    return mutateDocumentSnapshot(path, request).document;
}

QString exportMarkdown(const QJsonObject& document) {
    validateDocument(document);
    if (QJsonDocument(document).toJson(QJsonDocument::Compact).size() > MaximumDocumentBytes)
        fail("Review document exceeds 16 MiB");
    const auto asset = document["asset"].toObject();
    QString report = "# Asset review: " + escapedMarkdown(asset["id"].toString()) + "\n\n";
    report += "Variant: " + escapedMarkdown(asset["variant"].toString()) + "  \n";
    report += "LOD: " + QString::number(asset["lod"].toInt()) + "  \n";
    report += "Document revision: " + QString::number(document["document_revision"].toInteger()) + "  \n";
    report += "Model: " + escapedMarkdown(asset["model"].toString()) + "  \n";
    report += "Original model SHA-256: " + asset["model_sha256"].toString() + "\n\n";
    const auto notes = document["annotations"].toArray();
    if (notes.isEmpty()) report += "No annotations.\n";
    for (const auto& item : notes) {
        const auto note = item.toObject();
        const auto author = note["author"].toObject();
        report += "## " + escapedMarkdown(note["id"].toString()) + " (" +
                  escapedMarkdown(note["status"].toString()) + ")\n\n";
        report += "Author: " + escapedMarkdown(author["name"].toString()) + " (" +
                  escapedMarkdown(author["kind"].toString()) + ")  \n";
        report += "Created: " + note["created_at"].toString() + "  \n";
        report += "Updated: " + note["updated_at"].toString() + "  \n";
        report += "Created against: " + note["created_against"].toString() + "\n\n";
        const auto tags = note["tags"].toArray();
        if (!tags.isEmpty()) {
            report += "Tags: ";
            QStringList labels;
            for (const auto& tag : tags) labels.append(escapedMarkdown(tag.toString()));
            report += labels.join(", ") + "\n\n";
        }
        const auto lines = note["text"].toString().split('\n');
        for (const auto& line : lines) report += "> " + escapedMarkdown(line) + "\n";
        report += "\n";
        const auto targets = note["targets"].toArray();
        if (targets.isEmpty()) report += "Target: whole model.\n\n";
        else {
            report += "Targets:\n\n";
            for (const auto& target : targets) report += jsonBlock(target.toObject());
        }
        if (!note["view"].toObject().isEmpty()) report += "Saved view:\n\n" + jsonBlock(note["view"].toObject());
        if (note["resolution"].isObject()) report += "Resolution:\n\n" + jsonBlock(note["resolution"].toObject());
    }
    return report;
}

} // namespace preview::inspection
