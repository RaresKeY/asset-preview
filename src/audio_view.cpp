#include "audio_view.h"
#include "audio_visualizer.h"
#include "audio_waveform.h"
#include "preview_cache.h"
#include "mpv_player.h"
#include <QAbstractButton>
#include <QFileInfo>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QStyle>
#include <QTimer>
#include <algorithm>
#include <cmath>
using namespace preview::media;

struct AudioView::Player {
    mpv_handle* client=nullptr;
    QJsonObject properties;
    QString path;
    AudioVisualizer visualizer;
    std::shared_ptr<const preview::AudioWaveform> waveform;
    bool waveformPending=false;
    bool waveformStarted=false;
    bool fileLoaded=false;
    QString waveformKey;
    bool waveformCacheHit=false, reuseWaveform=true;
    ~Player() {
        if (client) { mpv_set_wakeup_callback(client,nullptr,nullptr); mpv_terminate_destroy(client); }
    }
};

AudioView::AudioView(QJsonObject cfg,QWidget* parent,LoadedCallback done,SettingsCallback patch)
    : QWidget(parent),config(std::move(cfg)),loaded(std::move(done)),changed(std::move(patch)) {
    setMinimumSize(100,100); setFocusPolicy(Qt::StrongFocus);
    setAccessibleName("Audio track");
}
AudioView::~AudioView() {
    closing=true; ++generation;
    waveformWorker.request_stop();
    if (waveformWorker.joinable()) waveformWorker.join();
    if (cache && current && current->waveform && !current->waveformKey.isEmpty() &&
        current->waveformKey==preview::sourceCacheKey("waveform-v1",current->path))
        cache->put(current->waveformKey,{{},{},std::move(current->waveform)});
    candidate.reset(); current.reset();
}
bool AudioView::loading() const { return bool(candidate); }
void AudioView::wakeup(void* pointer) {
    auto* self=static_cast<AudioView*>(pointer);
    if (self->closing || self->queued.exchange(true)) return;
    QMetaObject::invokeMethod(self,[self] { self->queued=false; if (!self->closing) self->drain(); },Qt::QueuedConnection);
}
bool AudioView::reload(const QJsonObject& cfg,QString& error) {
    config=cfg;
    candidate.reset(); ++generation;
    if (!QFileInfo(config["path"].toString()).isFile()) { error="Waiting for audio file"; return false; }
    try {
        auto next=std::make_unique<Player>(); next->path=config["path"].toString();
        next->reuseWaveform=reuseCachedPayload;
        next->client=mpv_create(); if (!next->client) throw std::runtime_error("Cannot create libmpv audio player");
        configure(next->client,true);
        checked(mpv_set_option_string(next->client,"af",AudioVisualizer::filter));
        checked(mpv_set_option_string(next->client,"pause","yes"));
        checked(mpv_initialize(next->client));
        const std::pair<const char*,mpv_format> observed[]={
            {"duration",MPV_FORMAT_DOUBLE},{"time-pos",MPV_FORMAT_DOUBLE},
            {"pause",MPV_FORMAT_FLAG},{"mute",MPV_FORMAT_FLAG},{"eof-reached",MPV_FORMAT_FLAG},{"seekable",MPV_FORMAT_FLAG},
            {"audio-codec",MPV_FORMAT_STRING},{"audio-params/samplerate",MPV_FORMAT_INT64},
            {"audio-params/channel-count",MPV_FORMAT_INT64},{"current-ao",MPV_FORMAT_STRING}};
        for (const auto& [key,format]:observed) checked(mpv_observe_property(next->client,0,key,format));
        checked(mpv_observe_property(next->client,0,AudioVisualizer::property,MPV_FORMAT_NODE));
        mpv_set_wakeup_callback(next->client,&AudioView::wakeup,this);
        command(next->client,"loadfile",next->path.toUtf8()); candidate=std::move(next);
        const int token=generation;
        QTimer::singleShot(15000,this,[this,token] { if (token==generation && candidate) failed("Audio load timed out"); });
        update(); return true;
    } catch (const std::exception& e) { error=QString::fromUtf8(e.what()); return false; }
}
void AudioView::failed(const QString& error) {
    candidate.reset(); ++generation;
    if (loaded) loaded(false,error);
    update();
}
void AudioView::drain() {
    QString error;
    bool notify=false;
    bool repaint=false;
    for (auto* p:{current.get(),candidate.get()}) {
        if (!p) continue;
        while (auto* event=mpv_wait_event(p->client,0)) {
            if (event->event_id==MPV_EVENT_NONE) break;
            if (event->event_id==MPV_EVENT_PROPERTY_CHANGE) {
                const auto* prop=static_cast<mpv_event_property*>(event->data);
                if (QByteArray(prop->name)==AudioVisualizer::property) {
                    if (prop->data && prop->format==MPV_FORMAT_NODE) p->visualizer.metadata(*static_cast<mpv_node*>(prop->data));
                    continue;
                }
                updateProperty(p->properties,*prop); notify|=p==current.get();
                if (p==current.get() && (QByteArray(prop->name)=="pause" || QByteArray(prop->name)=="mute" || QByteArray(prop->name)=="eof-reached")) repaint=true;
                if (p==current.get() && QByteArray(prop->name)=="eof-reached" && p->properties[prop->name].toBool() &&
                    !config["settings"].toObject()["loop"].toBool(true)) change({{"paused",true}});
            } else if (p==current.get() && event->event_id==MPV_EVENT_SEEK) { p->visualizer.reset(); repaint=true; }
            else if (event->event_id==MPV_EVENT_FILE_LOADED) p->fileLoaded=true;
            else if (p==candidate.get() && event->event_id==MPV_EVENT_END_FILE) {
                const auto* end=static_cast<mpv_event_end_file*>(event->data);
                error=end->error<0?QString::fromUtf8(mpv_error_string(end->error)):"File has no playable audio track";
            }
        }
    }
    if (!error.isEmpty()) failed(error);
    // Publication requires the loaded selected audio decoder's parameters.
    // FILE_LOADED by itself also succeeds for files with no usable audio.
    if (candidate && candidate->fileLoaded && candidate->properties["audio-params/samplerate"].toInt()>0 &&
        !candidate->properties["audio-codec"].toString().isEmpty()) {
        try {
            current=std::move(candidate); ++loads; ++generation;
            apply(*current); notify=true;
            if (loaded) loaded(true,{});
            update();
        } catch (const std::exception& e) { failed(QString::fromUtf8(e.what())); }
    }
    if (current && isVisible() && !config["settings"].toObject()["paused"].toBool(true) &&
        !current->properties["pause"].toBool(true) && !current->properties["eof-reached"].toBool())
        repaint|=current->visualizer.advance(current->properties["time-pos"].toDouble(-1));
    if (current && !current->waveformStarted && current->properties["duration"].toDouble()>0) startWaveform();
    if (repaint) update();
    if (notify && playbackChanged) playbackChanged();
}
void AudioView::startWaveform() {
    waveformWorker.request_stop();
    if (waveformWorker.joinable()) waveformWorker.join();
    const auto path=current->path;
    const auto duration=current->properties["duration"].toDouble();
    const int token=loads;
    current->waveformKey=preview::sourceCacheKey("waveform-v1",path);
    if (cache) cache->identify(current->waveformKey,"waveform",path);
    if (cache && current->reuseWaveform) {
        auto retained=cache->take(current->waveformKey,true);
        if (retained.waveform && std::abs(retained.waveform->duration-duration)<0.05 &&
            current->waveformKey==preview::sourceCacheKey("waveform-v1",path)) {
            current->waveform=std::move(retained.waveform); current->waveformStarted=true; current->waveformCacheHit=true;
            if (playbackChanged) playbackChanged();
            return;
        }
    }
    current->waveformPending=true;
    current->waveformStarted=true;
    waveformWorker=std::jthread([this,path,duration,token](std::stop_token stop) {
        QElapsedTimer timer; timer.start();
        auto data=preview::decodeAudioWaveform(path,duration,stop);
        const auto elapsedUs=timer.nsecsElapsed()/1000;
        if (stop.stop_requested()) return;
        QMetaObject::invokeMethod(this,[this,data=std::move(data),token,elapsedUs] {
            if (closing || !current || loads!=token) return;
            if (cache) cache->observe("waveform","decode",current->waveformKey,data?qint64(sizeof(preview::AudioWaveform)):0,"cache_miss",
                elapsedUs,{{"success",bool(data)}});
            current->waveform=std::move(data); current->waveformPending=false;
            if (playbackChanged) playbackChanged();
        },Qt::QueuedConnection);
    });
}
std::shared_ptr<const preview::AudioWaveform> AudioView::audioWaveform() const {
    return current?current->waveform:nullptr;
}
void AudioView::apply(Player& p) {
    const auto s=config["settings"].toObject();
    flag(p.client,"pause",s["paused"].toBool(true)); flag(p.client,"mute",s["muted"].toBool());
    string(p.client,"loop-file",s["loop"].toBool(true)?"inf":"no");
}
void AudioView::settings(const QJsonObject& cfg) {
    config=cfg;
    try {
        if (current) {
            if (current->properties["eof-reached"].toBool() && !config["settings"].toObject()["paused"].toBool(true))
                command(current->client,"seek","0","absolute+exact");
            apply(*current);
        }
        // Candidates remain paused and muted until successful publication.
    } catch (const std::exception& e) { if (loaded) loaded(false,QString::fromUtf8(e.what())); }
    update();
}
void AudioView::change(QJsonObject patch) { if (changed) changed(patch); }
bool AudioView::seek(double position,QString& error) {
    if (!current) { error="Audio is not loaded"; return false; }
    try { command(current->client,"seek",QByteArray::number(position,'f',3),"absolute+exact"); return true; }
    catch (const std::exception& e) { error=QString::fromUtf8(e.what()); return false; }
}
QJsonObject AudioView::metrics() const {
    QJsonObject result{{"loads",loads},{"renders",renders},{"engine",bool(current)},{"backend","libmpv"},{"loading",bool(candidate)}};
    if (current) {
        const auto& p=current->properties;
        result["duration"]=p["duration"]; result["position"]=p["time-pos"];
        result["paused"]=p["pause"]; result["muted"]=p["mute"]; result["eof"]=p["eof-reached"]; result["seekable"]=p["seekable"];
        result["codec"]=p["audio-codec"]; result["sample_rate"]=p["audio-params/samplerate"];
        result["channels"]=p["audio-params/channel-count"]; result["audio_output"]=p["current-ao"];
        result["track"]=QFileInfo(current->path).fileName();
        result["visualizer"]=current->visualizer.metrics();
        const auto& wave=current->waveform;
        result["waveform"]=QJsonObject{{"ready",bool(wave)},{"loading",current->waveformPending},{"cache_hit",current->waveformCacheHit},
            {"bins",wave?preview::AudioWaveform::binCount:0},{"channels",wave?wave->channels:0},
            {"duration",wave?wave->duration:0}};
    }
    return result;
}
void AudioView::paintEvent(QPaintEvent*) {
    ++renders;
    QPainter p(this); p.setRenderHint(QPainter::Antialiasing);
    const bool light=config["settings"].toObject()["background"].toString()=="light";
    p.fillRect(rect(),light?QColor("#e0e0e0"):QColor("#171c22"));
    const auto area=rect().adjusted(20,70,-20,-130);
    if (area.height()<32 || area.width()<48) return;
    const bool playing=current && !config["settings"].toObject()["paused"].toBool(true) && !current->properties["eof-reached"].toBool();
    const int labels=area.height()>105?64:area.height()>60?32:0;
    const int groupHeight=std::min(area.height(),124+labels);
    const int groupTop=area.bottom()-groupHeight+1;
    const auto visualArea=QRectF(area.x(),groupTop+labels,area.width(),groupHeight-labels);
    const bool motion=style()->styleHint(QStyle::SH_Widget_Animation_Duration,nullptr,this)>0;
    if (current) current->visualizer.paint(p,visualArea,light,playing,motion);
    else AudioVisualizer{}.paint(p,visualArea,light,false,motion);
    if (!labels) return;
    auto font=p.font();
    font.setPixelSize(14); p.setFont(font); p.setPen(light?QColor("#242b32"):QColor("#e8edf3"));
    const auto name=current?QFileInfo(current->path).fileName():QFileInfo(config["path"].toString()).fileName();
    const int textY=groupTop;
    p.drawText(QRect(area.x(),textY,area.width(),24),Qt::AlignCenter,p.fontMetrics().elidedText(name,Qt::ElideMiddle,area.width()));
    if (area.height()>105) {
        font.setPixelSize(12); p.setFont(font); p.setPen(light?QColor("#55616d"):QColor("#9aa7b5"));
        const auto state=!current?QString("Waiting for audio"):config["settings"].toObject()["paused"].toBool(true)?QString("Paused"):QString("Playing");
        const auto mute=config["settings"].toObject()["muted"].toBool()?QString(" · muted"):QString();
        p.drawText(QRect(area.x(),textY+26,area.width(),20),Qt::AlignCenter,state+mute);
    }
}
void AudioView::mousePressEvent(QMouseEvent* e) { if (activated) activated(); setFocus(Qt::MouseFocusReason); e->accept(); }
void AudioView::mouseDoubleClickEvent(QMouseEvent* e) { change({{"paused",!config["settings"].toObject()["paused"].toBool(true)}}); e->accept(); }
bool AudioView::eventFilter(QObject* watched,QEvent* e) {
    if ((e->type()==QEvent::ShortcutOverride || e->type()==QEvent::KeyPress) &&
        static_cast<QKeyEvent*>(e)->key()==Qt::Key_Space && qobject_cast<QAbstractButton*>(watched))
        return QWidget::eventFilter(watched,e);
    if (e->type()==QEvent::ShortcutOverride && playbackKey(static_cast<QKeyEvent*>(e))) { e->accept(); return true; }
    if (e->type()==QEvent::KeyPress && playbackKey(static_cast<QKeyEvent*>(e))) { keyPressEvent(static_cast<QKeyEvent*>(e)); return true; }
    return QWidget::eventFilter(watched,e);
}
void AudioView::keyPressEvent(QKeyEvent* e) {
    if (!playbackKey(e)) { QWidget::keyPressEvent(e); return; }
    const auto s=config["settings"].toObject();
    if (e->key()==Qt::Key_Space) change({{"paused",!s["paused"].toBool(true)}});
    else if (e->key()==Qt::Key_M) change({{"muted",!s["muted"].toBool()}});
    else {
        QString error; const double at=current?current->properties["time-pos"].toDouble():0;
        seek(e->key()==Qt::Key_Home?0:std::max(0.0,at+(e->key()==Qt::Key_Left?-5:5)),error);
    }
    e->accept();
}
extern "C" Q_DECL_EXPORT ModelHandle asset_preview_create_audio(QJsonObject config,QWidget* parent,LoadedCallback loaded,SettingsCallback changed) {
    auto* audio=new AudioView(std::move(config),parent,std::move(loaded),std::move(changed));
    return {audio,audio};
}
