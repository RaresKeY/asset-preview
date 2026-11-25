#include "thumbnail_job.h"
#include "backend_loader.h"
#include "file_view.h"
#include "theme.h"
#include <QApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QImage>
#include <QImageReader>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QProcess>
#include <QStandardPaths>
#include <QRegularExpression>
#include <QSaveFile>
#include <clocale>
#include <cmath>
#include <csignal>
#include <sys/prctl.h>
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
QImage videoFrame(const QJsonObject& config,const QDir& directory,QString& error,double& position) {
    const auto ffmpeg=QStandardPaths::findExecutable("ffmpeg"),ffprobe=QStandardPaths::findExecutable("ffprobe");
    if(ffmpeg.isEmpty() || ffprobe.isEmpty()) { error="Video thumbnail tools unavailable"; return {}; }
    const auto path=config["path"].toString();
    const QFileInfo source(path);
    if(!source.isFile() || source.size()>qint64(8)*1024*1024*1024) { error="Video thumbnail source exceeds limits";return {}; }
    // Local containers only: no playlist/network traversal, player, audio or
    // hardware decode context. The owning job group supplies hard cancellation.
    const QStringList input{"-max_alloc","67108864","-probesize","1048576","-analyzeduration","2000000",
        "-protocol_whitelist","file,pipe","-format_whitelist","mov,matroska,webm,avi,mpegts,mpeg,mpegvideo,ogg,gif,asf,flv"};
    auto run=[&](const QString& program,const QStringList& arguments,const QString& output,int timeout) {
        QProcess child;child.setStandardOutputFile(output);child.setStandardErrorFile(QProcess::nullDevice());
        child.setChildProcessModifier([owner=::getpid()] {
            if(::prctl(PR_SET_PDEATHSIG,SIGKILL)<0 || ::getppid()!=owner)_exit(126);
        });
        child.start(program,arguments);
        if(!child.waitForFinished(timeout)) { child.kill();child.waitForFinished(1000);return false; }
        return child.exitStatus()==QProcess::NormalExit && child.exitCode()==0;
    };
    const auto probePath=directory.filePath("probe.json");
    if(!run(ffprobe,QStringList{"-v","error"}+input+QStringList{"-select_streams","v:0","-show_entries","format=duration:stream=width,height",
        "-of","json",path},probePath,4000)) { error="Cannot probe video thumbnail";return {}; }
    QFile probe(probePath);
    if(!probe.open(QIODevice::ReadOnly) || probe.size()>4096) { error="Invalid video thumbnail metadata";return {}; }
    const auto metadata=QJsonDocument::fromJson(probe.readAll()).object();
    const auto streams=metadata["streams"].toArray();
    if(streams.isEmpty()) { error="No video stream for thumbnail";return {}; }
    const auto stream=streams.at(0).toObject();
    const qint64 pixels=qint64(stream["width"].toInt())*stream["height"].toInt();
    if(pixels<=0 || pixels>32*1024*1024) { error="Video thumbnail dimensions exceed limits";return {}; }
    const double duration=metadata["format"].toObject()["duration"].toString().toDouble();
    position=std::isfinite(duration) && duration>0 && duration<1e8?duration/2:0;
    const auto framePath=directory.filePath("frame.png");
    auto decode=[&] {
        QFile::remove(framePath);
        return run(ffmpeg,QStringList{"-nostdin","-v","error","-y","-threads","1","-filter_threads","1"}+input+
            QStringList{"-ss",QString::number(position,'f',6),"-i",path,"-map","0:v:0","-an","-sn","-dn",
                "-frames:v","1","-vf","scale=w='max(1,min(160,100*dar))':h='max(1,min(100,160/dar))',setsar=1",
                "-c:v","png","-threads","1","-fs","262144",framePath},QProcess::nullDevice(),8000) && QFileInfo(framePath).isFile();
    };
    if(!decode()) { position=0;if(!decode()) { error="Cannot decode video thumbnail";return {}; } }
    QImageReader reader(framePath);
    if(QFileInfo(framePath).size()>256*1024 || reader.size().width()>160 || reader.size().height()>100) {
        error="Invalid video thumbnail output";return {};
    }
    return reader.read();
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
    const auto kind=config["kind"].toString();
    if (kind!="model" && kind!="material" && kind!="file" && kind!="video") return 2;
    QString error,renderer;
    double frameTime=0;
    QImage image;
    try {
        if(kind=="file") { Theme::apply("dark");image=FilePreview::thumbnail(config,error); }
        else if(kind=="video")image=videoFrame(config,directory,error,frameTime);
        else {
            QLibrary library(QCoreApplication::applicationDirPath()+"/asset-preview-f3d.so");
            library.setLoadHints(QLibrary::PreventUnloadHint);
            using Factory=QImage (*)(QJsonObject,QString&,QString&);
            const auto factory=reinterpret_cast<Factory>(resolveBackend(library,"asset_preview_model_thumbnail"));
            image=factory(config,error,renderer);
        }
    } catch (const std::exception& failure) { error=QString::fromUtf8(failure.what()); }
    bool saved=false;
    if (!image.isNull() && image.width()<=256 && image.height()<=160) {
        QSaveFile output(directory.filePath("thumbnail.png"));
        saved=output.open(QIODevice::WriteOnly) && image.save(&output,"PNG") && output.commit();
    }
    QSaveFile receipt(directory.filePath("result.json"));
    const auto data=QJsonDocument(QJsonObject{{"renderer",renderer.left(256)},{"error",error.left(512)},
        {"nice",::getpriority(PRIO_PROCESS,0)},{"saved",saved},{"kind",kind},{"frame_time",frameTime}}).toJson(QJsonDocument::Compact);
    if (!receipt.open(QIODevice::WriteOnly) || receipt.write(data)!=data.size() || !receipt.commit()) return 2;
    return saved?0:1;
}
