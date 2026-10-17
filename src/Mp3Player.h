#pragma once

#include <string>
#include <vector>
#include <deque>
#include <mutex>
#include <atomic>
#include <thread>
#include <condition_variable>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
}

#include <miniaudio.h>
#include "AudioRingBuffer.h"

class Mp3Player {
public:
    Mp3Player();
    ~Mp3Player();

    bool load(const std::string& filepath);

    void play();
    void pause();
    void togglePause();
    void stop();
    void seek(double targetSec);

    void setVolume(float vol); // 0.0f ~ 1.0f
    float getVolume() const { return m_volume.load(); }

    bool isLoaded() const { return m_deviceInitialized; }
    bool isPlaying() const { return m_isPlaying && !m_isPaused; }
    bool isFinished() const { return m_eof && m_pcmBuffer.empty(); }

    double getCurrentTimeSec() const { return m_currentPtsSec.load(); }
    double getDurationSec() const { return m_durationSec; }
    std::string getFileName() const { return m_fileName; }

    void getWaveform(std::vector<float>& outSamples, size_t count = 128) const;

private:
    void decodeLoop();
    static void audioCallback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount);
    void onAudioCallback(uint8_t* pOutput, uint32_t frameCount);
    void cleanup();

private:
    AVFormatContext* m_fmtCtx = nullptr;
    AVCodecContext*  m_codecCtx = nullptr;
    SwrContext*      m_swrCtx = nullptr;
    int              m_audioStreamIdx = -1;

    static constexpr int OUT_SAMPLE_RATE = 44100;
    static constexpr int OUT_CHANNELS = 2;
    static constexpr AVSampleFormat OUT_FORMAT = AV_SAMPLE_FMT_S16;

    ma_device m_device{};
    bool m_deviceInitialized = false;

    std::thread m_decodeThread;
    mutable std::mutex m_bufferMutex;
    std::condition_variable m_cv;
    std::deque<uint8_t> m_pcmBuffer;



    // static constexpr size_t MAX_BUFFER_SIZE = OUT_SAMPLE_RATE * OUT_CHANNELS * 2;
    AudioRingBuffer m_ringBuffer{ OUT_SAMPLE_RATE * OUT_CHANNELS * 2};

    mutable std::mutex m_waveMutex;
    std::vector<float> m_waveHistory;

    std::mutex m_decodeWaitMutex;

    std::atomic<bool> m_isPlaying{false};
    std::atomic<bool> m_isPaused{false};
    std::atomic<bool> m_stopRequested{false};
    std::atomic<bool> m_eof{false};

    std::atomic<bool> m_seekRequested{false};
    std::atomic<double> m_seekTargetSec{0.0};

    std::atomic<float> m_volume{0.8f};
    double m_durationSec = 0.0;
    std::atomic<double> m_currentPtsSec{0.0};
    std::string m_fileName = "未加载文件 (支持直接将 MP3 拖入窗口)";
};