#pragma once
#include <QString>

namespace preview {
QString prepareThumbnailJobs(const QString& runtimeDirectory);
int thumbnailJob(int argc,char** argv);
}
