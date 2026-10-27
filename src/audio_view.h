#pragma once
#include "viewer.h"
#include <atomic>
#include <thread>

// Standalone audio shares libmpv's local playback policy, but needs neither a
// video track nor a decoded frame, render context, GPU surface or idle timer.
// Measured audio-filter metadata drives its painted visualization via events.
class AudioView final : public QWidget, public PreviewView {
public:
    AudioView(QJsonObject, QWidget*, LoadedCallback, SettingsCallback);
    ~AudioView() override;
    bool reload(const QJsonObject&, QString&) override;
    void fit() override {}
    void settings(const QJsonObject&) override;
    QJsonObject metrics() const override;
    std::shared_ptr<const preview::AudioWaveform> audioWaveform() const override;
    QImage snapshot() override { return grab().toImage(); }
    bool loading() const override;
    bool seek(double, QString&) override;
protected:
    void paintEvent(QPaintEvent*) override;
    bool eventFilter(QObject*, QEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
private:
    struct Player;
    static void wakeup(void*);
    void drain();
    void apply(Player&);
    void failed(const QString&);
    void change(QJsonObject);
    void startWaveform();
    QJsonObject config;
    LoadedCallback loaded;
    SettingsCallback changed;
    std::unique_ptr<Player> current, candidate;
    std::atomic_bool queued=false, closing=false;
    int loads=0, renders=0, generation=0;
    std::jthread waveformWorker;
};
