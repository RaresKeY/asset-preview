#pragma once
#include <QJsonObject>
#include <QString>
#ifdef ASSET_PREVIEW_GLTF_PIXELS
#include <f3d/reader.h>
#include <memory>
#include <vector>
#endif

namespace preview {
// Import-local scalar evidence; no retained mesh or renderer references.
void prepareTopologyImport(const QString& mode);
QJsonObject topologyImportMetrics();
#ifdef ASSET_PREVIEW_GLTF_PIXELS
std::vector<std::shared_ptr<f3d::reader>> topologyReaders();
#endif
}
