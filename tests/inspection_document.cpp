#include "inspection/document.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QProcess>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QUuid>

#include <functional>
#include <iostream>
#include <stdexcept>
#include <vector>

#ifdef Q_OS_UNIX
#include <csignal>
#include <sys/resource.h>
#endif

using namespace preview::inspection;

namespace {

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template<class Callable> void rejects(Callable&& callable, const char* message) {
    try { callable(); }
    catch (const std::runtime_error&) { return; }
    throw std::runtime_error(message);
}

QByteArray bytes(const QString& path) {
    QFile file(path);
    check(file.open(QIODevice::ReadOnly), "Cannot read fixture");
    return file.readAll();
}

void writeBytes(const QString& path, const QByteArray& data) {
    QFile file(path);
    check(file.open(QIODevice::WriteOnly | QIODevice::Truncate), "Cannot write fixture");
    check(file.write(data) == data.size(), "Cannot write complete fixture");
    check(file.flush(), "Cannot flush fixture");
}

void writeDocument(const QString& path, const QJsonObject& document) {
    writeBytes(path, QJsonDocument(document).toJson(QJsonDocument::Compact));
}

QJsonObject asset() {
    return {{"id", "fixture/hauler"}, {"variant", "active"}, {"lod", 0},
            {"model", "assets/hauler.glb"}, {"model_sha256", QString(64, 'a')},
            {"frame", QJsonObject{{"up", "+Y"}, {"front", "-Z"}, {"units", "metres"}}}};
}

QJsonObject target() {
    return {{"id", "scene:0/nodes:0/3"}, {"name", "Clamp <left>"},
            {"exported_name", "Clamp_Left"}, {"path", "/0:Hull/3:Clamp"},
            {"node_index", 3}, {"semantic_id", "clamp_left"},
            {"anchor", QJsonObject{{"kind", "surface_point"},
                                   {"position_local", QJsonArray{1.0, 2.0, 3.0}},
                                   {"normal_local", QJsonArray{0.0, 1.0, 0.0}}}}};
}

QJsonObject view() {
    return {{"camera", QJsonObject{{"position", QJsonArray{12.0, 8.0, 12.0}},
                                   {"focal", QJsonArray{0.0, 0.0, 0.0}},
                                   {"up", QJsonArray{0.0, 1.0, 0.0}},
                                   {"angle", 30.0}, {"parallel_span", 10.0}}},
            {"orthographic", false},
            {"cuts", QJsonArray{QJsonObject{{"axis", "z"}, {"position", 1.5},
                                           {"keep", "negative"}, {"enabled", true}}}},
            {"hidden", QJsonArray{"scene:0/nodes:0/4"}},
            {"selected", "scene:0/nodes:0/3"}};
}

QJsonObject note(const QString& text = "Check clamp contact.") {
    return {{"author", QJsonObject{{"kind", "human"}, {"name", "Local reviewer"}}},
            {"text", text}, {"created_against", QString(64, 'a')},
            {"targets", QJsonArray{target()}}, {"view", view()},
            {"tags", QJsonArray{"attachment"}}};
}

QJsonObject addRequest(qint64 revision = 0, const QString& text = "Check clamp contact.") {
    return {{"action", "add"}, {"expected_revision", revision},
            {"asset", asset()}, {"note", note(text)}};
}

QJsonObject firstNote(const QJsonObject& document) {
    return document["annotations"].toArray()[0].toObject();
}

QJsonObject updateRequest(const QJsonObject& document, const QJsonObject& patch) {
    return {{"action", "update"}, {"expected_revision", document["document_revision"]},
            {"id", firstNote(document)["id"]}, {"note", patch}};
}

QString requestArgument(const QJsonObject& request) {
    return QString::fromLatin1(QJsonDocument(request).toJson(QJsonDocument::Compact).toBase64());
}

void startHelper(QProcess& process, const QString& mode, const QString& path,
                 const QJsonObject& request) {
    process.start(QCoreApplication::applicationFilePath(), {mode, path, requestArgument(request)});
    check(process.waitForStarted(5000), "Cannot start mutation helper");
}

void waitReady(QProcess& process) {
    check(process.waitForReadyRead(5000), "Mutation helper did not become ready");
    check(process.readLine() == "ready\n", "Unexpected helper readiness message");
}

void complete(QProcess& process) {
    check(process.waitForFinished(10000), "Mutation helper did not finish");
    check(process.exitStatus() == QProcess::NormalExit, "Mutation helper crashed");
}

int helper(const QStringList& arguments) {
    if (arguments.size() != 4) return 20;
    const auto request = QJsonDocument::fromJson(QByteArray::fromBase64(arguments[3].toLatin1())).object();
    if (arguments[1] == "--concurrent") {
        std::cout << "ready\n" << std::flush;
        std::string proceed;
        std::getline(std::cin, proceed);
    }
#ifdef Q_OS_UNIX
    if (arguments[1] == "--limited-write") {
        struct rlimit limit;
        if (getrlimit(RLIMIT_FSIZE, &limit) != 0) return 21;
        limit.rlim_cur = 1024;
        if (setrlimit(RLIMIT_FSIZE, &limit) != 0) return 22;
        std::signal(SIGXFSZ, SIG_IGN);
    }
#endif
    try {
        mutateDocument(arguments[2], request);
        std::cout << "saved\n";
        return 0;
    } catch (const std::runtime_error& error) {
        std::cerr << error.what() << '\n';
        return 2;
    }
}

void roundTripAndPatch() {
    QTemporaryDir directory;
    check(directory.isValid(), "Cannot create fixture directory");
    const auto path = directory.filePath("review.json");
    auto document = mutateDocument(path, addRequest());
    check(document["schema"] == "asset-preview-review/v1", "Wrong schema");
    check(document["document_revision"].toInteger() == 1, "Wrong initial revision");
    check(document == readDocument(path), "Document did not round trip exactly");
    const auto original = firstNote(document);
    check(original["id"].toString().startsWith("note-"), "Missing generated UUID");
    check(original["created_at"].toString().endsWith('Z'), "Creation time is not UTC");
    check(original["status"] == "open", "Missing default open status");
    check(original["view"] == view(), "Saved view changed");
    const auto snapshot = readDocumentSnapshot(path);
    check(snapshot.document == document, "Snapshot differs from read");
    check(snapshot.digest == QString::fromLatin1(QCryptographicHash::hash(bytes(path),
        QCryptographicHash::Sha256).toHex()), "Snapshot digest is not exact bytes");
    check(!document.contains("_digest") && !bytes(path).contains("_digest"), "Persisted transient digest");
    auto request = updateRequest(document, {{"text", "Corrected contact note"},
        {"status", "resolved"}, {"resolution", QJsonObject{{"resolved_against", QString(64, 'b')},
        {"evidence", QJsonArray{"evidence/clamp.png", QJsonObject{{"check", "owner mesh validation"}}}}}}});
    request["expected_digest"] = snapshot.digest;
    document = mutateDocument(path, request);
    const auto changed = firstNote(document);
    check(document["document_revision"].toInteger() == 2, "Patch did not advance exactly one revision");
    for (const auto* key : {"id", "author", "created_at", "created_against"})
        check(changed[key] == original[key], "Patch changed a creation field");
    check(changed["text"] == "Corrected contact note", "Patch lost text");
    check(changed["status"] == "resolved", "Patch lost workflow status");
    check(changed["view"] == original["view"], "Patch lost unmodified view");
    document = mutateDocument(path, {{"action", "remove"}, {"expected_revision", 2},
                                    {"id", changed["id"]}});
    check(document["document_revision"].toInteger() == 3 && document["annotations"].toArray().isEmpty(),
          "Removal did not persist");
}

void publicationSnapshots() {
    QTemporaryDir directory;
    const auto path = directory.filePath("review.json");
    const auto published = mutateDocumentSnapshot(path, addRequest());
    const auto committed = bytes(path);
    const auto digest = QString::fromLatin1(QCryptographicHash::hash(committed,
        QCryptographicHash::Sha256).toHex());
    check(published.document == readDocument(path), "Publication snapshot lost committed data");
    check(published.digest == digest, "Publication digest is not the exact committed bytes");
    const auto reused = readDocumentSnapshot(path, &published);
    check(reused.document == published.document && reused.digest == published.digest,
          "Unchanged publication snapshot did not round trip");
    auto callerCopy = reused.document;
    callerCopy["annotations"] = QJsonArray{};
    check(!readDocumentSnapshot(path, &published).document["annotations"].toArray().isEmpty(),
          "Caller copy changed the cached native snapshot");
    check(bytes(path) == committed, "Snapshot reuse changed the publication");

    auto request = updateRequest(published.document, {{"text", "Updated using the publication digest"}});
    request["expected_digest"] = published.digest;
    const auto updated = mutateDocumentSnapshot(path, request);
    check(updated.document["document_revision"].toInteger() == 2,
          "Snapshot mutation did not advance revision");
    check(updated.digest == readDocumentSnapshot(path).digest && updated.digest != published.digest,
          "Updated publication digest does not identify its bytes");
    const auto current = readDocumentSnapshot(path, &published);
    check(current.document == updated.document && current.digest == updated.digest,
          "Old snapshot hid a later native publication");
}

void snapshotMismatchAndReplacement() {
    QTemporaryDir directory;
    const auto path = directory.filePath("review.json");
    const auto published = mutateDocumentSnapshot(path, addRequest());
    const auto committed = bytes(path);

    // Formatting alone changes exact identity without advancing the revision.
    writeBytes(path, committed + " \n");
    const auto reformatted = readDocumentSnapshot(path, &published);
    check(reformatted.document == published.document && reformatted.digest != published.digest,
          "Snapshot reused an obsolete exact-byte digest after an external edit");
    auto request = updateRequest(published.document, {{"status", "resolved"}});
    request["expected_digest"] = published.digest;
    const auto external = bytes(path);
    rejects([&] { mutateDocumentSnapshot(path, request); }, "Publication snapshot bypassed exact-byte CAS");
    check(bytes(path) == external, "Snapshot CAS failure changed external bytes");

    // Same-length content changes remain visible even when the mtime is restored.
    writeBytes(path, committed);
    const auto previousTime = QFileInfo(path).lastModified();
    auto replacement = committed;
    replacement.replace("Check clamp contact.", "Check clamp overlap.");
    check(replacement != committed && replacement.size() == committed.size(),
          "Invalid same-length replacement fixture");
    writeBytes(path, replacement);
    QFile edited(path);
    check(edited.open(QIODevice::ReadWrite), "Cannot open timestamp fixture");
    check(edited.setFileTime(previousTime, QFileDevice::FileModificationTime),
          "Cannot restore fixture modification time");
    edited.close();
    const auto changed = readDocumentSnapshot(path, &published);
    check(firstNote(changed.document)["text"] == "Check clamp overlap." &&
          changed.document["document_revision"] == published.document["document_revision"] &&
          changed.digest != published.digest,
          "Cached snapshot hid a same-revision, same-length external edit");

    // Atomic external replacement must replace the document as well as its hash.
    QSaveFile output(path);
    output.setDirectWriteFallback(false);
    check(output.open(QIODevice::WriteOnly), "Cannot stage external replacement");
    check(output.write(external) == external.size() && output.commit(),
          "Cannot publish external replacement");
    const auto replaced = readDocumentSnapshot(path, &changed);
    check(replaced.document == published.document && replaced.digest == reformatted.digest,
          "Cached snapshot hid an atomic external replacement");
    request["expected_digest"] = replaced.digest;
    check(firstNote(mutateDocumentSnapshot(path, request).document)["status"] == "resolved",
          "Fresh external snapshot could not be used for CAS");
}

void snapshotReuseStillChecksFiles() {
    QTemporaryDir directory;
    const auto path = directory.filePath("review.json");
    const auto published = mutateDocumentSnapshot(path, addRequest());
    const auto committed = bytes(path);
    const auto linked = directory.filePath("linked.json");
    check(QFile::link(path, linked), "Cannot create snapshot symlink fixture");
    rejects([&] { readDocumentSnapshot(linked, &published); }, "Cached snapshot accepted a file symlink");
    rejects([&] { readDocumentSnapshot("relative.json", &published); }, "Cached snapshot accepted a relative path");
    rejects([&] { readDocumentSnapshot(directory.path(), &published); }, "Cached snapshot accepted a directory");
    check(QFile::remove(path), "Cannot remove snapshot fixture");
    rejects([&] { readDocumentSnapshot(path, &published); }, "Cached snapshot hid a missing file");
    writeBytes(path, "{broken JSON");
    rejects([&] { readDocumentSnapshot(path, &published); }, "Cached snapshot hid corrupt JSON");
    auto invalid = published.document;
    auto item = firstNote(invalid);
    item["status"] = "invented";
    invalid["annotations"] = QJsonArray{item};
    writeDocument(path, invalid);
    rejects([&] { readDocumentSnapshot(path, &published); }, "Snapshot mismatch bypassed schema validation");
    writeBytes(path, committed + QByteArray(MaximumDocumentBytes + 1 - committed.size(), ' '));
    rejects([&] { readDocumentSnapshot(path, &published); }, "Cached snapshot bypassed the file byte limit");
}

void optionalMetadataAndMarkdown() {
    QTemporaryDir directory;
    const auto path = directory.filePath("review.json");
    auto request = addRequest();
    auto item = note("<script>hello</script>\n# plain heading\n`literal` [text](file:///tmp/x)");
    item["future_note"] = QJsonObject{{"safe", true}, {"nested", QJsonArray{1, "two", QJsonValue(QJsonValue::Null)}}};
    request["note"] = item;
    auto document = mutateDocument(path, request);
    document["future_document"] = QJsonObject{{"producer", "fixture-v2"}};
    writeDocument(path, document);
    document = mutateDocument(path, updateRequest(document, {{"text", item["text"]}}));
    check(document["future_document"].toObject()["producer"] == "fixture-v2", "Lost optional top metadata");
    check(firstNote(document)["future_note"] == item["future_note"], "Lost optional note metadata");
    const auto markdown = exportMarkdown(document);
    check(markdown.contains("fixture/hauler"), "Report lost asset identity");
    check(markdown.contains("scene:0/nodes:0/3"), "Report lost exact target identity");
    check(markdown.contains("Saved view:"), "Report lost saved view");
    check(markdown.contains("&lt;script&gt;") && !markdown.contains("<script>"), "Report injected raw HTML");
    check(markdown.contains("> \\# plain heading"), "Report promoted text to Markdown heading");
    auto modelNote = note("Whole model note");
    modelNote.remove("targets");
    modelNote.remove("view");
    auto next = addRequest(2);
    next["note"] = modelNote;
    document = mutateDocument(path, next);
    check(document["annotations"].toArray()[1].toObject()["targets"].toArray().isEmpty(), "Model note lacks empty targets");
    check(exportMarkdown(document).contains("Target: whole model."), "Report lost model-level note");
}

void malformedDocumentsStayUntouched() {
    QTemporaryDir directory;
    const auto path = directory.filePath("review.json");
    const std::vector<QByteArray> broken = {"{\"schema\":", "[]", "null", "", "{garbage}"};
    for (const auto& data : broken) {
        writeBytes(path, data);
        rejects([&] { readDocument(path); }, "Accepted corrupt document");
        rejects([&] { mutateDocument(path, addRequest(0)); }, "Overwrote corrupt document");
        check(bytes(path) == data, "Corrupt document bytes changed");
    }
    QFile::remove(path);
    const auto valid = mutateDocument(path, addRequest());
    auto unknown = valid;
    unknown["schema"] = "asset-preview-review/v2";
    writeDocument(path, unknown);
    const auto before = bytes(path);
    rejects([&] { mutateDocument(path, addRequest(1)); }, "Overwrote unknown schema");
    check(bytes(path) == before, "Unknown schema bytes changed");
    auto duplicate = valid;
    duplicate["annotations"] = QJsonArray{firstNote(valid), firstNote(valid)};
    writeDocument(path, duplicate);
    rejects([&] { readDocument(path); }, "Accepted duplicate note IDs");
    auto transient = valid;
    transient["_digest"] = QString(64, 'c');
    writeDocument(path, transient);
    rejects([&] { readDocument(path); }, "Accepted persisted transient digest");
}

void conflictsAndIdentity() {
    QTemporaryDir directory;
    const auto path = directory.filePath("review.json");
    auto document = mutateDocument(path, addRequest());
    const auto initial = bytes(path);
    rejects([&] { mutateDocument(path, addRequest(0)); }, "Accepted stale revision");
    check(bytes(path) == initial, "Stale write changed bytes");
    const auto snapshot = readDocumentSnapshot(path);
    writeBytes(path, initial + " \n");
    auto request = addRequest(1);
    request["expected_digest"] = snapshot.digest;
    const auto external = bytes(path);
    rejects([&] { mutateDocument(path, request); }, "Accepted unversioned external edit");
    check(bytes(path) == external, "Digest conflict overwrote external bytes");
    for (const auto* key : {"id", "variant", "lod", "model"}) {
        auto mismatch = asset();
        mismatch[key] = QString(key) == "lod" ? QJsonValue(1) : QJsonValue("another");
        request = addRequest(1);
        request["asset"] = mismatch;
        rejects([&] { mutateDocument(path, request); }, "Accepted asset identity conflict");
        check(bytes(path) == external, "Asset conflict changed bytes");
    }
    auto newAsset = asset();
    newAsset["model_sha256"] = QString(64, 'b');
    request = addRequest(1);
    request["asset"] = newAsset;
    auto newNote = note("Note on later model revision");
    newNote["created_against"] = QString(64, 'b');
    request["note"] = newNote;
    document = mutateDocument(path, request);
    check(document["asset"].toObject()["model_sha256"] == QString(64, 'a'), "Rewrote original asset digest");
    check(firstNote(document)["created_against"] == QString(64, 'a'), "Retargeted old note");
    check(document["annotations"].toArray()[1].toObject()["created_against"] == QString(64, 'b'),
          "Lost new note creation revision");
    for (const auto* key : {"id", "author", "created_at", "created_against"}) {
        const QJsonValue replacement = QString(key) == "author" ?
            QJsonValue(QJsonObject{{"kind", "agent"}, {"name", "Replacement"}}) : QJsonValue("changed");
        request = updateRequest(document, {{key, replacement}});
        const auto before = bytes(path);
        rejects([&] { mutateDocument(path, request); }, "Changed immutable creation field");
        check(bytes(path) == before, "Rejected patch changed bytes");
    }
    auto invalidRevision = addRequest(2);
    invalidRevision["expected_revision"] = "2";
    rejects([&] { mutateDocument(path, invalidRevision); }, "Coerced string revision");
    invalidRevision["expected_revision"] = 2.5;
    rejects([&] { mutateDocument(path, invalidRevision); }, "Accepted fractional revision");
}

void concurrentProcesses() {
    QTemporaryDir directory;
    const auto path = directory.filePath("review.json");
    mutateDocument(path, addRequest());
    const auto snapshot = readDocumentSnapshot(path);
    auto oneRequest = addRequest(1, "Writer one");
    auto twoRequest = addRequest(1, "Writer two");
    oneRequest["expected_digest"] = snapshot.digest;
    twoRequest["expected_digest"] = snapshot.digest;
    QProcess one, two;
    startHelper(one, "--concurrent", path, oneRequest);
    startHelper(two, "--concurrent", path, twoRequest);
    waitReady(one);
    waitReady(two);
    one.write("go\n");
    two.write("go\n");
    complete(one);
    complete(two);
    check((one.exitCode() == 0 && two.exitCode() == 2) ||
          (one.exitCode() == 2 && two.exitCode() == 0), "Concurrent writers did not yield one save and one conflict");
    const auto after = readDocument(path);
    check(after["document_revision"].toInteger() == 2, "Concurrent writers advanced revision twice");
    check(after["annotations"].toArray().size() == 2, "Concurrent writers lost or duplicated notes");
    const auto messages = one.readAllStandardError() + two.readAllStandardError();
    check(messages.contains("conflict"), "Concurrent rejection did not describe conflict");
}

void creationDependencyBinding() {
    QTemporaryDir directory;
    const auto path = directory.filePath("review.json");
    auto request = addRequest();
    auto item = note();
    item["dependency_revision"] = QString(64, 'b');
    request["note"] = item;
    auto document = mutateDocument(path, request);
    document = mutateDocument(path, updateRequest(document,
        {{"text", "Retained dependency binding"}, {"dependency_revision", QString(64, 'b')}}));
    const auto before = bytes(path);
    rejects([&] { mutateDocument(path, updateRequest(document,
        {{"dependency_revision", QString(64, 'c')}})); }, "Changed note creation dependency");
    check(bytes(path) == before, "Dependency conflict changed bytes");

    const auto unboundPath = directory.filePath("unbound.json");
    const auto unbound = mutateDocument(unboundPath, addRequest());
    const auto unboundBytes = bytes(unboundPath);
    rejects([&] { mutateDocument(unboundPath, updateRequest(unbound,
        {{"dependency_revision", QString(64, 'b')}})); }, "Added creation dependency to an unbound note");
    rejects([&] { mutateDocument(unboundPath, updateRequest(unbound,
        {{"dependency_revision", QJsonValue(QJsonValue::Null)}})); }, "Added null dependency to an unbound note");
    check(bytes(unboundPath) == unboundBytes, "Rejected dependency addition changed bytes");

    const std::vector<QJsonValue> invalid = {QJsonValue(QJsonValue::Null), "", QString(63, 'a'),
        QString(64, 'g'), 123, true};
    const auto invalidPath = directory.filePath("invalid.json");
    for (const auto& value : invalid) {
        auto invalidNote = note();
        invalidNote["dependency_revision"] = value;
        auto invalidRequest = addRequest();
        invalidRequest["note"] = invalidNote;
        rejects([&] { mutateDocument(invalidPath, invalidRequest); }, "Created invalid note dependency");
        check(!QFile::exists(invalidPath), "Invalid dependency creation left a document");
        auto invalidDocument = document;
        invalidNote = firstNote(document);
        invalidNote["dependency_revision"] = value;
        invalidDocument["annotations"] = QJsonArray{invalidNote};
        writeDocument(path, invalidDocument);
        const auto invalidBytes = bytes(path);
        rejects([&] { readDocument(path); }, "Read invalid note dependency");
        check(bytes(path) == invalidBytes, "Invalid dependency read changed bytes");
    }
}

void offlineCliInteroperability() {
    QTemporaryDir directory;
    const auto path = directory.filePath("review.json");
    auto request = addRequest();
    auto item = note();
    auto multiline = target();
    QString controls;
    for (char16_t value = 1; value < 32; ++value) controls += QChar(value);
    controls += QChar(0x7f);
    for (const auto* key : {"name", "exported_name", "semantic_id"})
        multiline[key] = "Clamp" + controls + "<panel>";
    multiline["id"] = "scene:\t0/nodes:0/3";
    item["targets"] = QJsonArray{multiline};
    item["dependency_revision"] = QString(64, 'b');
    item["author"] = QJsonObject{{"kind", "human"}, {"name", "User\nName"}};
    item["tags"] = QJsonArray{"fit\tcheck"};
    item["future_data"] = QJsonObject{{QString("key") + QChar(1), controls}};
    item["text"] = QString("Check") + QChar(0x1b) + "[2Jthe\tclamp\nNext line.";
    request["note"] = item;
    auto source = asset();
    source["id"] = "fixture/\thauler";
    source["model"] = "assets/hauler\tlarge.glb";
    request["asset"] = source;
    const auto document = mutateDocument(path, request);
    const auto report = exportMarkdown(document);
    check(!report.contains(QChar(0x1b)), "Qt Markdown report contains a terminal escape");
    check(!report.contains(QChar(0x7f)), "Qt Markdown report contains a delete control");
    check(report.contains("\n> Next line\\."), "Qt Markdown report lost note lines");
    const auto cli = QDir(QFileInfo(QString::fromUtf8(__FILE__)).absolutePath())
        .absoluteFilePath("../bin/asset-preview");
    QProcess process;
    process.start(cli, {"annotations", "list", path, "--json"});
    check(process.waitForStarted(5000), "Cannot start offline CLI");
    check(process.waitForFinished(5000), "Offline CLI did not finish");
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
        throw std::runtime_error("Offline CLI rejected Qt review: " + process.readAllStandardError().toStdString());
    const auto output = QJsonDocument::fromJson(process.readAllStandardOutput()).object();
    check(output["annotations"] == document["annotations"], "Offline CLI changed Qt review notes");
    check(output["asset"] == document["asset"], "Offline CLI changed Qt asset identity");
}

void saveFailurePreservesBytes() {
    QTemporaryDir directory;
    const auto path = directory.filePath("review.json");
    const auto document = mutateDocument(path, addRequest(0, QString(2048, 's')));
    const auto before = bytes(path);
#ifdef Q_OS_UNIX
    QProcess process;
    startHelper(process, "--limited-write", path,
                updateRequest(document, {{"text", QString(4096, 't')}}));
    complete(process);
    check(process.exitCode() == 2, "Limited filesystem write unexpectedly succeeded");
    const auto error = process.readAllStandardError();
    check(error.contains("write review") || error.contains("commit review"),
          "Save-failure helper did not reach atomic output write or commit");
    check(bytes(path) == before, "Failed save destroyed previous document");
    check(readDocument(path) == document, "Failed save changed persisted data");
#endif
    QLockFile held(path + ".lock");
    check(held.tryLock(), "Cannot hold fixture lock");
    rejects([&] { mutateDocument(path, updateRequest(document, {{"text", "Blocked"}})); }, "Ignored existing document lock");
    check(bytes(path) == before, "Lock conflict changed bytes");
}

void boundedTextAndTargets() {
    QTemporaryDir directory;
    const auto path = directory.filePath("review.json");
    const QString boundary(MaximumAnnotationTextBytes / 2, QChar(0x00e9));
    auto document = mutateDocument(path, addRequest(0, boundary));
    check(firstNote(document)["text"].toString().toUtf8().size() == MaximumAnnotationTextBytes,
          "UTF-8 boundary fixture wrong");
    const auto before = bytes(path);
    rejects([&] { mutateDocument(path, updateRequest(document, {{"text", boundary + QChar(0x00e9)}})); },
            "Accepted oversized multibyte note");
    QJsonArray targets;
    for (int i = 0; i < MaximumAnnotationTargets; ++i) targets.append(target());
    document = mutateDocument(path, updateRequest(document, {{"targets", targets}}));
    targets.append(target());
    rejects([&] { mutateDocument(path, updateRequest(document, {{"targets", targets}})); },
            "Accepted too many targets");
    auto labelsOnly = target();
    labelsOnly.remove("id");
    labelsOnly.remove("path");
    rejects([&] { mutateDocument(path, updateRequest(document, {{"targets", QJsonArray{labelsOnly}}})); },
            "Accepted display label as authoritative identity");
    auto invalidAnchor = target();
    invalidAnchor["anchor"] = QJsonObject{{"kind", "surface_point"},
        {"position_local", QJsonArray{0, 0, 0}}, {"normal_local", QJsonArray{0, 0, 0}}};
    rejects([&] { mutateDocument(path, updateRequest(document, {{"targets", QJsonArray{invalidAnchor}}})); },
            "Accepted degenerate surface normal");
    auto invalidView = view();
    auto camera = invalidView["camera"].toObject();
    camera["position"] = camera["focal"];
    invalidView["camera"] = camera;
    rejects([&] { mutateDocument(path, updateRequest(document, {{"view", invalidView}})); },
            "Accepted degenerate camera");
    check(bytes(path) != before, "Boundary targets update did not save");
}

void noteCountLimit() {
    QTemporaryDir directory;
    const auto path = directory.filePath("review.json");
    auto document = mutateDocument(path, addRequest());
    const auto sample = firstNote(document);
    QJsonArray annotations;
    for (int i = 0; i < MaximumAnnotations; ++i) {
        auto item = sample;
        item["id"] = "note-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
        annotations.append(item);
    }
    document["annotations"] = annotations;
    writeDocument(path, document);
    check(readDocument(path)["annotations"].toArray().size() == MaximumAnnotations,
          "Rejected exact note limit");
    const auto before = bytes(path);
    rejects([&] { mutateDocument(path, addRequest(1)); }, "Accepted annotation beyond limit");
    check(bytes(path) == before, "Note count failure changed bytes");
    annotations.append(sample);
    document["annotations"] = annotations;
    writeDocument(path, document);
    rejects([&] { readDocument(path); }, "Read too many notes");
}

void byteAndDepthLimits() {
    QTemporaryDir directory;
    const auto path = directory.filePath("review.json");
    const auto document = mutateDocument(path, addRequest());
    auto data = QJsonDocument(document).toJson(QJsonDocument::Compact);
    data.append(QByteArray(MaximumDocumentBytes - data.size(), ' '));
    writeBytes(path, data);
    check(readDocument(path) == document, "Rejected exact document byte limit");
    data.append(' ');
    writeBytes(path, data);
    rejects([&] { readDocument(path); }, "Read oversized document");
    check(bytes(path) == data, "Oversized read changed file");
    writeDocument(path, document);
    const auto before = bytes(path);
    QJsonObject nested{{"value", 1}};
    for (int depth = 0; depth < 40; ++depth) nested = {{"nested", nested}};
    rejects([&] { mutateDocument(path, updateRequest(document, {{"future", nested}})); },
            "Accepted deeply nested mutation");
    auto deepDocument = document;
    deepDocument["future"] = nested;
    writeDocument(path, deepDocument);
    const auto deepBytes = bytes(path);
    rejects([&] { readDocument(path); }, "Accepted deeply nested file");
    check(bytes(path) == deepBytes, "Deep JSON changed during rejection");
    writeBytes(path, before);
    rejects([&] { mutateDocument(path, updateRequest(document,
        {{"future", QString(64 * 1024 + 1, 'x')}})); }, "Accepted oversized optional string");
    QJsonObject wide;
    for (int i = 0; i <= 1024; ++i) wide[QString::number(i)] = i;
    rejects([&] { mutateDocument(path, updateRequest(document, {{"future", wide}})); },
            "Accepted oversized optional object");
    check(bytes(path) == before, "Bounded mutation rejection changed file");
}

void finalSerializedSizeLimit() {
    QTemporaryDir directory;
    const auto path = directory.filePath("review.json");
    auto document = mutateDocument(path, addRequest());
    QJsonArray payload;
    for (int i = 0; i < 255; ++i) payload.append(QString(64 * 1024, 'p'));
    document["future_payload"] = payload;
    const auto currentSize = QJsonDocument(document).toJson(QJsonDocument::Compact).size();
    const auto tail = MaximumDocumentBytes - 4096 - currentSize - 3;
    check(tail > 0 && tail <= 64 * 1024, "Invalid near-limit fixture");
    payload.append(QString(tail, 'p'));
    document["future_payload"] = payload;
    writeDocument(path, document);
    check(readDocument(path) == document, "Rejected bounded near-limit metadata");
    const auto before = bytes(path);
    rejects([&] { mutateDocument(path, addRequest(1, QString(MaximumAnnotationTextBytes, 'x'))); },
            "Saved serialized document beyond limit");
    check(bytes(path) == before, "Serialized-size rejection changed previous document");
}

void explicitPathsAndCreationValidation() {
    QTemporaryDir directory;
    const auto path = directory.filePath("review.json");
    rejects([&] { mutateDocument("relative.json", addRequest()); }, "Accepted relative review destination");
    rejects([&] { mutateDocument(directory.filePath("missing/review.json"), addRequest()); },
            "Created missing destination directories implicitly");
    rejects([&] { readDocument(path); }, "Read missing document");
    rejects([&] { mutateDocument(path, addRequest(1)); }, "Created with nonzero revision");
    auto request = addRequest();
    auto invalid = note();
    invalid.remove("author");
    request["note"] = invalid;
    rejects([&] { mutateDocument(path, request); }, "Created note without author");
    check(!QFile::exists(path), "Invalid creation left a document");
    invalid = note();
    invalid["id"] = "note-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    request["note"] = invalid;
    rejects([&] { mutateDocument(path, request); }, "Accepted caller-generated add ID");
    check(!QFile::exists(path), "Rejected ID creation left a document");
    auto initial = addRequest();
    initial["expected_digest"] = "";
    const auto document = mutateDocument(path, initial);
    auto emptyDigestUpdate = addRequest(1);
    emptyDigestUpdate["expected_digest"] = "";
    rejects([&] { mutateDocument(path, emptyDigestUpdate); }, "Accepted empty digest for an existing document");
    const auto linked = directory.filePath("linked.json");
    check(QFile::link(path, linked), "Cannot create fixture symbolic link");
    rejects([&] { mutateDocument(linked, addRequest(1)); }, "Accepted ambiguous symbolic-link destination");
    check(readDocument(path) == document, "Symlink rejection changed target document");
}

void layerPresets() {
    QTemporaryDir directory;
    const auto path = directory.filePath("review.json");
    mutateDocument(path, addRequest());
    const QJsonArray layers{QJsonObject{{"name", "Expose clamp"},
        {"targets", QJsonArray{"scene:0/nodes:0/1", "scene:0/nodes:0/2"}},
        {"created_against", QString(64, 'a')}}};
    const auto document = mutateDocument(path, {{"action", "layers"}, {"expected_revision", 1},
                                               {"layer_presets", layers}});
    check(document["layer_presets"] == layers, "Layer preset did not round trip");
    check(document["document_revision"].toInteger() == 2, "Layer preset did not advance revision");
    const auto before = bytes(path);
    rejects([&] { mutateDocument(path, {{"action", "layers"}, {"expected_revision", 2},
        {"layer_presets", QJsonArray{QJsonObject{{"name", "Inferred hull"}}}}}); },
        "Accepted preset without explicit membership");
    check(bytes(path) == before, "Invalid preset changed bytes");
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    const auto arguments = application.arguments();
    if (arguments.size() > 1) return helper(arguments);
    const std::vector<std::pair<const char*, std::function<void()>>> tests = {
        {"round trip and patch", roundTripAndPatch},
        {"validated publication snapshots", publicationSnapshots},
        {"snapshot mismatch and external replacement", snapshotMismatchAndReplacement},
        {"snapshot file and validation boundaries", snapshotReuseStillChecksFiles},
        {"optional metadata and Markdown", optionalMetadataAndMarkdown},
        {"corrupt and unknown documents", malformedDocumentsStayUntouched},
        {"revision, digest and asset identity", conflictsAndIdentity},
        {"concurrent processes", concurrentProcesses},
        {"creation dependency binding", creationDependencyBinding},
        {"offline CLI interoperability", offlineCliInteroperability},
        {"save failure and held lock", saveFailurePreservesBytes},
        {"text, targets and view bounds", boundedTextAndTargets},
        {"annotation count limit", noteCountLimit},
        {"byte, depth and metadata limits", byteAndDepthLimits},
        {"final serialized size limit", finalSerializedSizeLimit},
        {"explicit paths and creation validation", explicitPathsAndCreationValidation},
        {"explicit layer presets", layerPresets},
    };
    int failures = 0;
    for (const auto& [name, test] : tests) {
        try { test(); std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& error) {
            ++failures;
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
        }
    }
    std::cout << (tests.size() - failures) << '/' << tests.size() << " document tests passed\n";
    return failures ? 1 : 0;
}
