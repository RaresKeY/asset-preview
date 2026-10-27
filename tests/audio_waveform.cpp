#include "audio_waveform.h"
#include <QCoreApplication>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QTextStream>
#include <QtEndian>
#include <cmath>
#include <cstdlib>
#include <numbers>

namespace {
void require(bool value,const char* message) {
    if (!value) { QTextStream(stderr)<<message<<'\n'; std::exit(1); }
}
template<class T> void append(QByteArray& data,T value) {
    const auto encoded=qToLittleEndian(value); data.append(reinterpret_cast<const char*>(&encoded),sizeof encoded);
}
void fixture(const QString& path,int channels) {
    constexpr int rate=48000;
    QByteArray pcm;
    for (int i=0;i<rate;++i) for (int ch=0;ch<channels;++ch) {
        const double t=double(i)/rate;
        const bool active=ch==0?(t>=.2 && t<.4):(t>=.6 && t<.8);
        append<int16_t>(pcm,active?int16_t(16384*std::sin(2*std::numbers::pi*400*t)):0);
    }
    QByteArray data("RIFF"); append<uint32_t>(data,36+pcm.size()); data.append("WAVEfmt ");
    append<uint32_t>(data,16); append<uint16_t>(data,1); append<uint16_t>(data,channels);
    append<uint32_t>(data,rate); append<uint32_t>(data,rate*channels*2);
    append<uint16_t>(data,channels*2); append<uint16_t>(data,16); data.append("data"); append<uint32_t>(data,pcm.size()); data+=pcm;
    QFile file(path); require(file.open(QIODevice::WriteOnly),"Cannot create audio fixture"); require(file.write(data)==data.size(),"Cannot write audio fixture");
}
float amplitude(const preview::AudioWaveform& wave,double at,int channel) {
    const auto& bin=wave.bins[int(at*wave.binCount)]; return std::max(-bin[channel*2],bin[channel*2+1]);
}
void checkStereo(const QString& path) {
    const auto wave=preview::decodeAudioWaveform(path,1,{});
    require(bool(wave),"Waveform decode failed"); require(wave->channels==2,"Lost stereo channels");
    require(amplitude(*wave,.1,0)<.02 && amplitude(*wave,.9,1)<.02,"Silence moved on timeline");
    require(amplitude(*wave,.3,0)>.3 && amplitude(*wave,.3,1)<.02,"Left channel is missing or leaks");
    require(amplitude(*wave,.7,1)>.3 && amplitude(*wave,.7,0)<.02,"Right channel is missing or leaks");
}
}
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv); QTemporaryDir directory;
    const auto stereo=directory.filePath("stereo.wav"), mono=directory.filePath("mono.wav");
    fixture(stereo,2); fixture(mono,1); checkStereo(stereo);
    const auto wave=preview::decodeAudioWaveform(mono,1,{});
    require(wave && wave->channels==1,"Mono decode failed");
    for (const auto& bin:wave->bins) require(bin[0]==bin[2] && bin[1]==bin[3],"Mono is not duplicated");
    std::stop_source cancelled; cancelled.request_stop();
    require(!preview::decodeAudioWaveform(stereo,1,cancelled.get_token()),"Cancelled decode returned data");
    require(!preview::decodeAudioWaveform(stereo,0,{}),"Unknown duration is accepted");
    require(!preview::decodeAudioWaveform(stereo,21601,{}),"Duration cap is ignored");
    require(!preview::decodeAudioWaveform(directory.path(),1,{}),"Directory accepted as audio");
    const auto broken=directory.filePath("broken.wav");
    { QFile file(broken); require(file.open(QIODevice::WriteOnly),"Cannot create invalid fixture"); file.write("unfinished"); }
    require(!preview::decodeAudioWaveform(broken,1,{}),"Broken file returned a waveform");
    for (const auto& extension:{"mp3","flac","ogg","opus","m4a"}) {
        const auto target=directory.filePath(QString("encoded.")+extension);
        QProcess encoder; encoder.start("ffmpeg",{"-nostdin","-loglevel","error","-y","-i",stereo,target});
        require(encoder.waitForFinished(15000) && encoder.exitCode()==0,"Cannot encode compressed fixture");
        checkStereo(target);
    }
    const auto floating=directory.filePath("floating.wav");
    QProcess encoder; encoder.start("ffmpeg",{"-nostdin","-loglevel","error","-y","-i",stereo,"-c:a","pcm_f32le",floating});
    require(encoder.waitForFinished(15000) && encoder.exitCode()==0,"Cannot encode float fixture"); checkStereo(floating);
    QTextStream(stdout)<<"Waveform PCM, stereo timing, mono, compressed formats and cancellation checks passed\n";
}
