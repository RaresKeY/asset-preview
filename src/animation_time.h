#pragma once
#include <algorithm>
#include <cmath>
#include <limits>

namespace preview {
// Round only numerical noise at an integer boundary. A fixed epsilon becomes
// smaller than division/multiplication error on long clips at unusual rates.
inline double animationFrame(double position,int fps) {
    const double frame=position*fps;
    const double rounded=std::round(frame);
    const double tolerance=std::min(.25,std::max(1e-8,std::abs(frame)*std::numeric_limits<double>::epsilon()*2));
    return std::abs(frame-rounded)<=tolerance?rounded:frame;
}
inline double animationSample(double position,double duration,bool stepped,int fps) {
    position=std::clamp(position,0.0,duration);
    if (!stepped || position>=duration) return position;
    return std::min(duration,std::floor(animationFrame(position,fps))/fps);
}
inline double animationStep(double position,double duration,int fps,int direction) {
    const double frame=animationFrame(position,fps);
    const double next=direction>0?std::floor(frame)+1:std::ceil(frame)-1;
    return std::clamp(next/fps,0.0,duration);
}
}
