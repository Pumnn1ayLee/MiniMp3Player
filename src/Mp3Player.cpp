#define MINIAUDIO_IMPLEMENTATION
#include "Mp3Player.h"
#include <iostream>
#include <cstring>
#include <algorithm>
#include <filesystem>
#include <cmath>

extern "C" {
#include <libavutil/log.h> // 用于屏蔽 FFmpeg 冗余警告
}

Mp3Player::Mp3Player() : m_ringBuffer(OUT_SAMPLE_RATE * OUT_CHANNELS * 2) {
    // 屏蔽 FFmpeg 的 "skipped samples" 等警告，只保留严重错误
    av_log_set_level(AV_LOG_ERROR);
    m_waveHistory.reserve(256);
}

Mp3Player::~Mp3Player() {
    stop();
}

bool Mp3Player::load(const std::string& filepath) {
    stop();

    // 1. 清洗路径：去掉 Windows "复制为路径" 自带的首尾双引号
    std::string cleanPath = filepath;
    if (!cleanPath.empty() && cleanPath.front() == '"' && cleanPath.back() == '"') {
        cleanPath = cleanPath.substr(1, cleanPath.length() - 2);
    }

    if (avformat_open_input(&m_fmtCtx, cleanPath.c_str(), nullptr, nullptr) < 0) {
        std::cerr << "[Error] 无法打开文件: " << cleanPath << "\n";
        return false;
    }

    if (avformat_find_stream_info(m_fmtCtx, nullptr) < 0) {
        cleanup();
        return false;
    }

    const AVCodec* codec = nullptr;
    m_audioStreamIdx = av_find_best_stream(m_fmtCtx, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
    if (m_audioStreamIdx < 0 || !codec) {
        cleanup();
        return false;
    }

    AVStream* audioStream = m_fmtCtx->streams[m_audioStreamIdx];
    if (audioStream->duration != AV_NOPTS_VALUE) {
        m_durationSec = audioStream->duration * av_q2d(audioStream->time_base);
    } else if (m_fmtCtx->duration != AV_NOPTS_VALUE) {
        m_durationSec = static_cast<double>(m_fmtCtx->duration) / AV_TIME_BASE;
    }

    m_codecCtx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(m_codecCtx, audioStream->codecpar);
    if (avcodec_open2(m_codecCtx, codec, nullptr) < 0) {
        cleanup();
        return false;
    }

    AVChannelLayout outChLayout = AV_CHANNEL_LAYOUT_STEREO;
    if (swr_alloc_set_opts2(&m_swrCtx,
                            &outChLayout, OUT_FORMAT, OUT_SAMPLE_RATE,
                            &m_codecCtx->ch_layout, m_codecCtx->sample_fmt, m_codecCtx->sample_rate,
                            0, nullptr) < 0 || swr_init(m_swrCtx) < 0) {
        cleanup();
        return false;
    }

    ma_device_config config = ma_device_config_init(ma_device_type_playback);
    config.playback.format   = ma_format_s16;
    config.playback.channels = OUT_CHANNELS;
    config.sampleRate        = OUT_SAMPLE_RATE;
    config.dataCallback      = &Mp3Player::audioCallback;
    config.pUserData         = this;

    if (ma_device_init(nullptr, &config, &m_device) != MA_SUCCESS) {
        cleanup();
        return false;
    }
    m_deviceInitialized = true;
    ma_device_set_master_volume(&m_device, m_volume.load());

    m_fileName = std::filesystem::u8path(cleanPath).filename().u8string();
    m_currentPtsSec = 0.0;
    return true;
}

void Mp3Player::play() {
    if (!m_deviceInitialized) return;

    if (!m_isPlaying) {
        m_stopRequested = false;
        m_eof = false;
        m_isPlaying = true;
        m_isPaused = false;
        m_decodeThread = std::thread(&Mp3Player::decodeLoop, this);
        ma_device_start(&m_device);
    } else if (m_isPaused) {
        m_isPaused = false;
        ma_device_start(&m_device);
    }
}

void Mp3Player::pause() {
    if (m_isPlaying && !m_isPaused) {
        m_isPaused = true;
        ma_device_stop(&m_device);
    }
}

void Mp3Player::togglePause() {
    if (!m_isPlaying) return;
    if (m_isPaused) play();
    else pause();
}

void Mp3Player::stop() {
    m_stopRequested = true;
    m_cv.notify_all();

    if (m_decodeThread.joinable()) {
        m_decodeThread.join();
    }

    cleanup();
    m_isPlaying = false;
    m_isPaused = false;
    m_currentPtsSec = 0.0;
}

void Mp3Player::seek(double targetSec) {
    if (!m_isPlaying) return;
    targetSec = std::clamp(targetSec, 0.0, m_durationSec);
    m_seekTargetSec = targetSec;
    m_seekRequested = true;
    m_cv.notify_all();
}

void Mp3Player::setVolume(float vol) {
    vol = std::clamp(vol, 0.0f, 1.0f);
    m_volume.store(vol);
    if (m_deviceInitialized) {
        ma_device_set_master_volume(&m_device, vol);
    }
}

void Mp3Player::getWaveform(std::vector<float>& outSamples, size_t count) const {
    std::lock_guard<std::mutex> lock(m_waveMutex);
    outSamples = m_waveHistory;

    // 如果 UI 需要的点数和当前的不一样，做简单的平滑裁剪/填充
    if (outSamples.size() < count) {
        outSamples.resize(count, 0.0f);
    } else if (outSamples.size() > count) {
        outSamples.erase(outSamples.begin(), outSamples.begin() + (outSamples.size() - count));
    }
}

void Mp3Player::decodeLoop() {
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    AVRational timeBase = m_fmtCtx->streams[m_audioStreamIdx]->time_base;

    // 【优化】热路径零分配：循环外预分配好连续内存，不再释放
    int maxOutSamples = OUT_SAMPLE_RATE;
    std::vector<uint8_t> resampleBuf(maxOutSamples * OUT_CHANNELS * sizeof(int16_t));

    // RMS 波形提取算法参数
    const int RMS_WINDOW = 512;
    float sumSquares = 0.0f;
    int rmsCount = 0;

    while (!m_stopRequested) {
        if (m_seekRequested.exchange(false)) {
            int64_t seekTarget = static_cast<int64_t>(m_seekTargetSec.load() / av_q2d(timeBase));
            if (av_seek_frame(m_fmtCtx, m_audioStreamIdx, seekTarget, AVSEEK_FLAG_BACKWARD) >= 0) {
                avcodec_flush_buffers(m_codecCtx);
                m_ringBuffer.clear();
                m_currentPtsSec = m_seekTargetSec.load();
                m_eof = false;
            }
        }

        // 背压控制：当无锁队列接近满了的时候才挂起解码线程
        {
            std::unique_lock<std::mutex> lock(m_decodeWaitMutex);
            m_cv.wait(lock, [this]() {
                return m_stopRequested || m_seekRequested || m_ringBuffer.size() < (m_ringBuffer.capacity() * 0.9);
            });
            if (m_stopRequested) break;
            if (m_seekRequested) continue;
        }

        int ret = av_read_frame(m_fmtCtx, packet);
        if (ret < 0) {
            avcodec_send_packet(m_codecCtx, nullptr);
            m_eof = true;
        } else {
            if (packet->stream_index == m_audioStreamIdx) {
                avcodec_send_packet(m_codecCtx, packet);
            }
            av_packet_unref(packet);
        }

        while (avcodec_receive_frame(m_codecCtx, frame) >= 0) {
            int64_t delay = swr_get_delay(m_swrCtx, m_codecCtx->sample_rate);
            int dstNbSamples = static_cast<int>(av_rescale_rnd(
                delay + frame->nb_samples, OUT_SAMPLE_RATE, m_codecCtx->sample_rate, AV_ROUND_UP));

            // 仅在数据量超出当前 Vector 容量时扩容
            if (dstNbSamples > maxOutSamples) {
                maxOutSamples = dstNbSamples * 2;
                resampleBuf.resize(maxOutSamples * OUT_CHANNELS * sizeof(int16_t));
            }

            uint8_t* outPtr = resampleBuf.data();
            int convertedSamples = swr_convert(
                m_swrCtx, &outPtr, dstNbSamples,
                (const uint8_t**)frame->data, frame->nb_samples
            );

            if (convertedSamples > 0) {
                size_t dataBytes = convertedSamples * OUT_CHANNELS * sizeof(int16_t);

                // 【优化】计算 RMS 能量包络波形（从声卡回调剥离，不阻塞声卡）
                const auto* pcm16 = reinterpret_cast<const int16_t*>(resampleBuf.data());
                int totalSamples = convertedSamples * OUT_CHANNELS;

                std::vector<float> tempWave;
                for (int i = 0; i < totalSamples; i += OUT_CHANNELS) { // 提取左声道
                    float norm = pcm16[i] / 32768.0f;
                    sumSquares += norm * norm;
                    if (++rmsCount >= RMS_WINDOW) {
                        // 计算均方根，并放大范围映射到 [-1, 1] 使得视觉上更动感
                        tempWave.push_back(std::sqrt(sumSquares / RMS_WINDOW) * 2.0f - 1.0f);
                        sumSquares = 0.0f;
                        rmsCount = 0;
                    }
                }

                // 将波形推入 UI 专用队列
                if (!tempWave.empty()) {
                    std::lock_guard<std::mutex> wlock(m_waveMutex);
                    m_waveHistory.insert(m_waveHistory.end(), tempWave.begin(), tempWave.end());
                    if (m_waveHistory.size() > 256) {
                        m_waveHistory.erase(m_waveHistory.begin(), m_waveHistory.end() - 256);
                    }
                }

                // 【优化】将重采样后的音频数据无锁写入环形队列
                m_ringBuffer.write(resampleBuf.data(), dataBytes);
            }

            // 更新播放时间（考虑到缓冲区里还没播放的时长，保持 UI 进度条精准）
            if (frame->pts != AV_NOPTS_VALUE) {
                double ptsSec = frame->pts * av_q2d(timeBase);
                double bufferedSec = static_cast<double>(m_ringBuffer.size()) / (OUT_SAMPLE_RATE * OUT_CHANNELS * 2);
                m_currentPtsSec = std::max(0.0, ptsSec - bufferedSec);
            }
            av_frame_unref(frame);
        }

        if (m_eof) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

    av_frame_free(&frame);
    av_packet_free(&packet);
}

void Mp3Player::audioCallback(ma_device* pDevice, void* pOutput, const void* /*pInput*/, ma_uint32 frameCount) {
    auto* player = static_cast<Mp3Player*>(pDevice->pUserData);
    if (player) {
        player->onAudioCallback(static_cast<uint8_t*>(pOutput), frameCount);
    }
}

void Mp3Player::onAudioCallback(uint8_t* pOutput, uint32_t frameCount) {
    const size_t bytesNeeded = frameCount * OUT_CHANNELS * sizeof(int16_t);

    // 【优化】声卡底层严格 O(1) 的无锁拉取数据，不再有任何 std::mutex 阻塞
    size_t bytesRead = m_ringBuffer.read(pOutput, bytesNeeded);

    // 如果解码线程没跟上，用静音（0）填充剩下的空间，防止爆破音
    if (bytesRead < bytesNeeded) {
        std::memset(pOutput + bytesRead, 0, bytesNeeded - bytesRead);
    }

    // 唤醒解码线程继续喂数据
    m_cv.notify_one();
}

void Mp3Player::cleanup() {
    if (m_deviceInitialized) {
        ma_device_uninit(&m_device);
        m_deviceInitialized = false;
    }
    if (m_swrCtx) swr_free(&m_swrCtx);
    if (m_codecCtx) avcodec_free_context(&m_codecCtx);
    if (m_fmtCtx) avformat_close_input(&m_fmtCtx);

    m_ringBuffer.clear();
}