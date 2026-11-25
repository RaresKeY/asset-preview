#include "viewer.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QGridLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMouseEvent>
#include <QOpenGLWidget>
#include <QOpenGLFunctions>
#include <QTemporaryDir>
#include <QThread>
#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <vector>
#include <sys/resource.h>

// Opt-in production-component probe. Callback time includes Qt submission and
// driver waits; it is neither GPU execution time nor physical display latency.
class ImagePanApp final : public QApplication {
public:
    using QApplication::QApplication;
    QList<QObject*> surfaces;
    bool measuring=false;
    std::vector<double> paints,updates,publications;
    bool notify(QObject* target,QEvent* event) override {
        const bool paint=measuring && event->type()==QEvent::Paint && surfaces.contains(target);
        const bool update=measuring && event->type()==QEvent::UpdateRequest;
        const bool publication=measuring && target==this && event->type()==QEvent::MetaCall;
        QElapsedTimer clock; if(paint || update || publication)clock.start();
        const bool result=QApplication::notify(target,event);
        if(paint)paints.push_back(clock.nsecsElapsed()/1e6);
        if(update)updates.push_back(clock.nsecsElapsed()/1e6);
        if(publication)publications.push_back(clock.nsecsElapsed()/1e6);
        return result;
    }
};
static QJsonObject distribution(std::vector<double> values) {
    if(values.empty())return {};
    std::sort(values.begin(),values.end()); double sum=0;for(auto v:values)sum+=v;
    return {{"samples",int(values.size())},{"mean_ms",sum/values.size()},{"p50_ms",values[(values.size()-1)/2]},
        {"p95_ms",values[int((values.size()-1)*.95)]},{"p99_ms",values[int((values.size()-1)*.99)]},{"max_ms",values.back()}};
}
static void require(bool okay,const char* message) { if(!okay)throw std::runtime_error(message); }
int main(int argc,char** argv) {
    ImagePanApp app(argc,argv); app.setQuitOnLastWindowClosed(false);
    try {
        require(argc==6,"Usage: profile_image_pan EDGE VIEWS nearest|smooth ZOOM OUTPUT.json");
        const int edge=QString::fromLocal8Bit(argv[1]).toInt(),count=QString::fromLocal8Bit(argv[2]).toInt();
        require((edge==2048 || edge==4096) && (count==1 || count==2 || count==4),"Use bounded image/view presets");
        const bool nearest=QString::fromLocal8Bit(argv[3])=="nearest";
        const double zoom=QString::fromLocal8Bit(argv[4]).toDouble(); require(zoom>=1 && zoom<=8,"Zoom out of range");
        auto spin=[&](int ms) {QElapsedTimer timer;timer.start();do {app.processEvents();QThread::msleep(1);}while(timer.elapsed()<ms);};
        QTemporaryDir root; QWidget window;window.resize(1400,850);auto* layout=new QGridLayout(&window);layout->setContentsMargins(0,0,0,0);
        QList<ImageView*> views; QString error;QStringList paths;
        for(int i=0;i<count;++i) {
            QImage pixels(edge,edge,QImage::Format_RGBA8888);
            for(int y=0;y<edge;++y)for(int x=0;x<edge;++x)pixels.setPixelColor(x,y,QColor((x*3+i*27)%256,(y*5)%256,(x+y)%256,(x/17+y/13)%3==0?127:255));
            const auto path=root.filePath(QString("large-%1.png").arg(i));require(pixels.save(path),"Fixture save failed");
            paths.append(path);
        }
        QElapsedTimer preparation;preparation.start();app.measuring=true;
        for(int i=0;i<count;++i) {
            auto* view=new ImageView(&window);layout->addWidget(view,count==4?i/2:0,count==4?i%2:i);views.append(view);
            require(view->reload({{"path",paths[i]},{"settings",QJsonObject{{"nearest",nearest}}}},error),"Image load rejected");
        }
        window.show();QElapsedTimer deadline;deadline.start();
        while(std::any_of(views.begin(),views.end(),[](auto* view){return view->loading();})) {require(deadline.elapsed()<30000,"Decode timed out");spin(2);}
        const auto readyMs=preparation.nsecsElapsed()/1e6;
        const auto publication=distribution(app.publications);app.measuring=false;app.paints.clear();app.updates.clear();
        spin(250); QString renderer;
        for(auto* view:views) {
            auto* display=view->findChild<QOpenGLWidget*>();require(display && display->isValid(),"Hardware image display required");
            display->makeCurrent();renderer=QString::fromLatin1(reinterpret_cast<const char*>(display->context()->functions()->glGetString(GL_RENDERER)));display->doneCurrent();
            require(!renderer.isEmpty() && !renderer.contains("llvmpipe",Qt::CaseInsensitive),"Software renderer refused");app.surfaces.append(display);
            view->setImageView({{"zoom",zoom},{"pan",QJsonArray{0,0}}});
        }
        // Two views represent linked comparison. Four views represent a grid
        // with one actively dragged image and three static image siblings.
        if(count==2)views.first()->inspectionChanged=[&](const QJsonObject& state,bool){views[1]->setImageView(state);};
        auto* source=views.first(); QPointF point=source->rect().center();
        auto mouse=[&](QEvent::Type type,Qt::MouseButton button,Qt::MouseButtons buttons) {
            QMouseEvent event(type,point,source->mapToGlobal(point.toPoint()),button,buttons,Qt::NoModifier);QApplication::sendEvent(source,&event);
        };
        spin(250);mouse(QEvent::MouseButtonPress,Qt::LeftButton,Qt::LeftButton);
        for(int i=0;i<12;++i) {point+=QPointF(i%2?2:-2,1);mouse(QEvent::MouseMove,Qt::NoButton,Qt::LeftButton);spin(8);}
        struct rusage usage{};getrusage(RUSAGE_SELF,&usage);
        const double cpuBefore=usage.ru_utime.tv_sec+usage.ru_utime.tv_usec/1e6+usage.ru_stime.tv_sec+usage.ru_stime.tv_usec/1e6;
        app.measuring=true;QElapsedTimer clock;clock.start();std::vector<double> input;
        constexpr int moves=120;
        for(int i=0;i<moves;++i) {
            point+=QPointF(i%2?2:-2,i%4<2?1:-1);QElapsedTimer callback;callback.start();mouse(QEvent::MouseMove,Qt::NoButton,Qt::LeftButton);input.push_back(callback.nsecsElapsed()/1e6);
            const auto target=(i+1)*8;while(clock.elapsed()<target)spin(1);
        }
        mouse(QEvent::MouseButtonRelease,Qt::LeftButton,Qt::NoButton);spin(50);app.measuring=false;
        getrusage(RUSAGE_SELF,&usage);const double cpuAfter=usage.ru_utime.tv_sec+usage.ru_utime.tv_usec/1e6+usage.ru_stime.tv_sec+usage.ru_stime.tv_usec/1e6;
        QJsonArray states;for(auto* view:views)states.append(view->metrics());
        const auto renders=source->metrics()["renders"].toInt();spin(120);require(source->metrics()["renders"].toInt()==renders,"Idle image kept repainting");
        QFile memory("/proc/self/smaps_rollup");require(memory.open(QIODevice::ReadOnly),"Process memory metrics unavailable");
        QJsonObject report{{"edge",edge},{"views",count},{"nearest",nearest},{"zoom",zoom},{"renderer",renderer},{"platform",QGuiApplication::platformName()},
            {"ready_ms",readyMs},{"publication_callbacks",publication},
            {"moves",moves},{"image_paint",distribution(app.paints)},{"window_update",distribution(app.updates)},{"input",distribution(input)},
            {"cpu_seconds",cpuAfter-cpuBefore},{"resident_peak_kib",qint64(usage.ru_maxrss)},{"memory",QString::fromLatin1(memory.readAll())},{"states",states},
            {"scope","Production ImageView input and Qt draw/submit callbacks, after 12 warmups; no scanout or service persistence"}};
        QFile output(QString::fromLocal8Bit(argv[5]));require(output.open(QIODevice::WriteOnly),"Report open failed");output.write(QJsonDocument(report).toJson());
        std::puts(QJsonDocument(report).toJson(QJsonDocument::Compact).constData());
    }catch(const std::exception& error){std::fprintf(stderr,"%s\n",error.what());return 1;}
}
