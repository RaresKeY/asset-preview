#pragma once

#include <QJsonObject>
#include <QString>

namespace preview::inspection {

inline constexpr qint64 MaximumDocumentBytes = 16 * 1024 * 1024;
inline constexpr int MaximumAnnotations = 4096;
inline constexpr int MaximumAnnotationTextBytes = 8 * 1024;
inline constexpr int MaximumAnnotationTargets = 16;

struct DocumentSnapshot {
    QJsonObject document;
    QString digest; // SHA-256 of the exact validated read/committed bytes, never persisted.
};

// Paths must name explicit absolute regular files in existing directories.
// All functions throw std::runtime_error on invalid data or I/O failure.
// Native callers may reuse an unmodified snapshot returned by this module.
// The optional snapshot is trusted validation evidence, never client-supplied
// data: its document and digest must remain the original pair. Path, file type
// and byte limits are still checked, and current bytes are read and hashed.
// Only an exact digest match skips parsing and schema validation.
DocumentSnapshot readDocumentSnapshot(const QString& path,
                                      const DocumentSnapshot* validated = nullptr);
QJsonObject readDocument(const QString& path);

// Requires action and expected_revision. An optional expected_digest checks exact
// on-disk bytes, including external edits that did not advance the revision.
// Missing documents may be created only by add with revision zero and asset.
// add requires note.author, note.text and note.created_against. IDs and UTC times
// are generated here. update applies a note patch, preserving creation fields,
// including the original presence and value of dependency_revision.
// remove requires id. layers replaces layer_presets on an existing document.
// The complete validated document and exact publication digest are returned
// only after an atomic successful save. A later read must check current bytes
// before treating the snapshot as the file's current contents.
DocumentSnapshot mutateDocumentSnapshot(const QString& path, const QJsonObject& request);
// Compatibility wrapper for callers that need only the document.
QJsonObject mutateDocument(const QString& path, const QJsonObject& request);

// Produces a derived report, without I/O, renderers or mutation.
QString exportMarkdown(const QJsonObject& document);

} // namespace preview::inspection
