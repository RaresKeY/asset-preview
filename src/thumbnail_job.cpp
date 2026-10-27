#include "thumbnail_job.h"
#include "backend_loader.h"
#include <QApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QImage>
#include <QImageReader>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <clocale>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace {
bool privateDirectory(const QString& path) {
    struct stat status{};
    return ::lstat(QFile::encodeName(path).constData(),&status)==0 && S_ISDIR(status.st_mode) &&
        status.st_uid==::geteuid() && !(status.st_mode&0077);
}
}
QString preview::prepareThumbnailJobs(const QString& runtimeDirectory) {
    // The server holds runtimeDirectory/server.lock for its complete lifetime.
    // Only that instance can own jobs here, so leftovers are safe to reap after
    // reacquiring the lock following an abrupt server exit.
    if (!privateDirectory(runtimeDirectory)) return {};
    const auto root=QDir(runtimeDirectory).filePath("thumbnail-jobs");
    if (!QFileInfo::exists(root) && ::mkdir(QFile::encodeName(root).constData(),0700)!=0) return {};
    if (!privateDirectory(root)) return {};
    QDirIterator jobs(root,{"job-*"},QDir::Dirs|QDir::NoDotAndDotDot|QDir::NoSymLinks);
    const QRegularExpression name("^job-[A-Za-z0-9]{6}$");
    int scanned=0;
    while (jobs.hasNext() && ++scanned<=256) {
        const auto path=jobs.next();
        if (name.match(jobs.fileName()).hasMatch() && privateDirectory(path)) QDir(path).removeRecursively();
    }
    return root;
}

int preview::thumbnailJob(int argc,char** argv) {
    // A finite task process, never a service or a visible preview window.
    ::setpriority(PRIO_PROCESS,0,19);
#ifdef SYS_ioprio_set
    ::syscall(SYS_ioprio_set,1,0,3<<13); // idle I/O class
#endif
    const rlimit cpu{20,25}, core{0,0};
    ::setrlimit(RLIMIT_CPU,&cpu); ::setrlimit(RLIMIT_CORE,&core);
    qputenv("QT_QPA_PLATFORM","offscreen");
    for (const auto* name:{"OMP_NUM_THREADS","VTK_SMP_MAX_THREADS","OPENBLAS_NUM_THREADS"}) qputenv(name,"1");
    qputenv("QT_SCALE_FACTOR","1");
    QApplication app(argc,argv); std::setlocale(LC_NUMERIC,"C");
    app.setApplicationName("asset-preview");
    QImageReader::setAllocationLimit(64);
    const QDir directory(QString::fromLocal8Bit(argv[2]));
    QFile request(directory.filePath("request.json"));
    if (!request.open(QIODevice::ReadOnly) || request.size()>64*1024) return 2;
    const auto config=QJsonDocument::fromJson(request.readAll()).object();
    if (config["kind"]!="model" && config["kind"]!="material") return 2;
    QString error,renderer;
    QImage image;
    try {
        QLibrary library(QCoreApplication::applicationDirPath()+"/asset-preview-f3d.so");
        library.setLoadHints(QLibrary::PreventUnloadHint);
        using Factory=QImage (*)(QJsonObject,QString&,QString&);
        const auto factory=reinterpret_cast<Factory>(resolveBackend(library,"asset_preview_model_thumbnail"));
        image=factory(config,error,renderer);
    } catch (const std::exception& failure) { error=QString::fromUtf8(failure.what()); }
    bool saved=false;
    if (!image.isNull() && image.width()<=256 && image.height()<=160) {
        QSaveFile output(directory.filePath("thumbnail.png"));
        saved=output.open(QIODevice::WriteOnly) && image.save(&output,"PNG") && output.commit();
    }
    QSaveFile receipt(directory.filePath("result.json"));
    const auto data=QJsonDocument(QJsonObject{{"renderer",renderer.left(256)},{"error",error.left(512)},
        {"nice",::getpriority(PRIO_PROCESS,0)},{"saved",saved}}).toJson(QJsonDocument::Compact);
    if (!receipt.open(QIODevice::WriteOnly) || receipt.write(data)!=data.size() || !receipt.commit()) return 2;
    return saved?0:1;
}
