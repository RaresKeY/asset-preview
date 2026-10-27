#pragma once
#include <QJsonObject>
#include <QRectF>
#include <array>
#include <mpv/client.h>

class QPainter;

namespace preview::media {
// A bounded history of measured stereo energy, not a synthesized spectrum.
// The owning player's property events are its only clock.
class AudioVisualizer {
public:
    static constexpr const char* property="af-metadata/preview-levels";
    static constexpr const char* filter=
        "@preview-levels:lavfi=[astats=metadata=1:reset=1:"
        "measure_perchannel=RMS_level:measure_overall=RMS_level+Peak_level,"
        "aspectralstats=win_size=2048:overlap=0:measure=centroid]";
    void metadata(const mpv_node&);
    bool advance(double position);
    void reset();
    void paint(QPainter&, const QRectF&, bool light, bool playing, bool motion) const;
    QJsonObject metrics() const;
private:
    static constexpr int historySize=40;
    std::array<std::array<double,2>,historySize> history{};
    std::array<double,2> rmsDb{-90,-90}, level{};
    double peakDb=-90, centroid=0, energy=0, pulse=0, lastPosition=-1;
    bool available=false;
    int updates=0;
};
}
