#pragma once
#include <array>
#include <memory>
#include <stop_token>
#include <QString>

namespace preview {
struct AudioWaveform {
    static constexpr int binCount=2048;
    // Minimum/maximum PCM amplitudes for the first two channels, mono duplicated.
    std::array<std::array<float,4>,binCount> bins{};
    double duration=0;
    int channels=0;
    float peak=0;
};
// One finite, local-only decode. No PCM cache or player, cancellable off-page.
std::shared_ptr<const AudioWaveform> decodeAudioWaveform(const QString&,double,std::stop_token);
}
