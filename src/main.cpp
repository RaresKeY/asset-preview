#include "service.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include <QLockFile>
#include <QSocketNotifier>
#include <QSurfaceFormat>
#include <csignal>
#include <fcntl.h>
#include <unistd.h>

static int signalWrite=-1;
static void signalHandler(int) { const char byte=1; if (signalWrite>=0) { const auto ignored=::write(signalWrite,&byte,1); (void)ignored; } }
int main(int argc,char** argv) {
    QSurfaceFormat format;
    format.setVersion(3,3); format.setProfile(QSurfaceFormat::CoreProfile);
    format.setDepthBufferSize(24); format.setStencilBufferSize(8); format.setSamples(0);
    QSurfaceFormat::setDefaultFormat(format);
    QApplication app(argc,argv);
    app.setApplicationName("asset-preview"); app.setApplicationDisplayName("Asset Preview");
    app.setDesktopFileName("asset-preview"); app.setQuitOnLastWindowClosed(false);
    QCommandLineParser parser; parser.addHelpOption();
    parser.addOption({"socket","Private local socket path","path"});
    parser.addOption({"state","Private state directory","path"});
    parser.process(app);
    const auto sock=parser.value("socket"), state=parser.value("state");
    if (sock.isEmpty() || state.isEmpty()) { qCritical("Use bin/asset-preview to start the application"); return 2; }
    QLockFile lock(QFileInfo(sock).absolutePath()+"/server.lock");
    lock.setStaleLockTime(0);
    if (!lock.tryLock(0)) { qCritical("Another Asset Preview server holds the lock"); return 3; }
    Service service(sock,state); QString error;
    if (!service.start(error)) { qCritical("%s",qPrintable(error)); return 1; }
    std::signal(SIGTERM,signalHandler); std::signal(SIGINT,signalHandler);
    int signalPipe[2];
    if (::pipe2(signalPipe,O_NONBLOCK|O_CLOEXEC)) { qCritical("Cannot create signal pipe"); return 1; }
    signalWrite=signalPipe[1];
    QSocketNotifier notifier(signalPipe[0],QSocketNotifier::Read);
    QObject::connect(&notifier,&QSocketNotifier::activated,&app,[&app] { app.quit(); });
    const int result=app.exec();
    signalWrite=-1; ::close(signalPipe[0]); ::close(signalPipe[1]);
    return result;
}
