#include "audio_visualizer.h"
#include <QJsonArray>
#include <QPainter>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace preview::media {
namespace {
double number(const mpv_node& metadata,const char* key,double fallback) {
    if (metadata.format!=MPV_FORMAT_NODE_MAP || !metadata.u.list) return fallback;
    const auto& list=*metadata.u.list;
    for (int i=0;i<std::min(list.num,64);++i) {
        const auto& value=list.values[i];
        if (std::strcmp(list.keys[i],key) || value.format!=MPV_FORMAT_STRING) continue;
        bool ok=false;
        const double result=QString::fromUtf8(value.u.string).toDouble(&ok);
        if (ok && std::isfinite(result)) return result;
        // astats reports digital silence as -inf. A silent stereo channel must
        // stay silent instead of inheriting the non-silent overall/mono level.
        return ok && std::isinf(result) && result<0?-90:fallback;
    }
    return fallback;
}
double amplitude(double db) { return std::clamp((db+60)/54,0.0,1.0); }
QColor alpha(QColor color,double opacity) { color.setAlphaF(std::clamp(opacity,0.0,1.0)); return color; }

}

void AudioVisualizer::metadata(const mpv_node& node) {
    const double overall=number(node,"lavfi.astats.Overall.RMS_level",-90);
    rmsDb={std::clamp(number(node,"lavfi.astats.1.RMS_level",overall),-90.0,0.0),
           std::clamp(number(node,"lavfi.astats.2.RMS_level",overall),-90.0,0.0)};
    peakDb=std::clamp(number(node,"lavfi.astats.Overall.Peak_level",-90),-90.0,0.0);
    const double left=number(node,"lavfi.aspectralstats.1.centroid",0);
    centroid=std::clamp((left+number(node,"lavfi.aspectralstats.2.centroid",left))/2,0.0,96000.0);
    available=node.format==MPV_FORMAT_NODE_MAP && node.u.list && node.u.list->num>0;
}
void AudioVisualizer::reset() {
    history={}; level={}; energy=0; pulse=0; lastPosition=-1;
}
bool AudioVisualizer::advance(double position) {
    if (!available || !std::isfinite(position) || position<0) return false;
    if (lastPosition>=0 && position<lastPosition) reset();
    if (lastPosition>=0 && position-lastPosition<.035) return false;
    const double dt=lastPosition<0?.05:std::clamp(position-lastPosition,.035,.2);
    lastPosition=position;
    const double before=energy;
    for (int ch=0;ch<2;++ch) {
        const double target=amplitude(rmsDb[ch]);
        level[ch]+=(target-level[ch])*(1-std::exp(-dt/(target>level[ch]?.035:.16)));
        if (level[ch]<.001) level[ch]=0;
    }
    energy=(level[0]+level[1])/2;
    // Keep the measured onset metric available independently of the bar display.
    pulse=std::max(pulse*std::exp(-dt/.18),std::clamp((energy-before)*2.4,0.0,1.0));
    if (pulse<.001) pulse=0;
    const auto old=history;
    std::move(history.begin()+1,history.end(),history.begin()); history.back()=level;
    ++updates;
    return old!=history;
}
QJsonObject AudioVisualizer::metrics() const {
    return {{"available",available},{"updates",updates},{"rms_db",QJsonArray{rmsDb[0],rmsDb[1]}},
        {"peak_db",peakDb},{"centroid_hz",centroid},{"energy",energy},{"pulse",pulse}};
}
void AudioVisualizer::paint(QPainter& painter,const QRectF& area,bool light,bool playing,bool motion) const {
    if (area.width()<32 || area.height()<24) return;
    painter.save(); painter.setClipRect(area); painter.setPen(Qt::NoPen);
    const double width=std::min(820.0,area.width());
    const double left=area.center().x()-width/2;
    const double baseline=area.bottom()-2;
    const double height=std::min(120.0,area.height()-4);
    const double pitch=width/historySize, barWidth=std::clamp(pitch*.4,2.0,7.0);
    const QColor green=light?QColor("#16834e"):QColor("#22cf7c");
    painter.setBrush(alpha(green,playing?1:.5));
    for (int i=0;i<historySize;++i) {
        // Oldest on the left, newest on the right. These bars show measured
        // stereo energy over time, so they do not carry frequency labels.
        const auto& sample=motion?history[i]:level;
        const double value=(sample[0]+sample[1])/2;
        const double h=std::max(2.0,value*height);
        const double x=left+(i+.5)*pitch-barWidth/2;
        painter.drawRoundedRect(QRectF(x,baseline-h,barWidth,h),barWidth/2,barWidth/2);
    }
    painter.restore();
}
}
