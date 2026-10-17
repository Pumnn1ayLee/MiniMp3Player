#pragma once
#include <vector>
#include <atomic>
#include <algorithm>
#include <cstring>

class AudioRingBuffer {
public:
    explicit AudioRingBuffer(size_t capacity)
        : m_buffer(capacity), m_capacity(capacity) {}

    // 清空缓冲区（用于 Seek 时）
    void clear() {
        m_readPos.store(0, std::memory_order_relaxed);
        m_writePos.store(0, std::memory_order_relaxed);
    }

    // 获取当前可用数据量
    size_t size() const {
        return m_writePos.load(std::memory_order_acquire) - m_readPos.load(std::memory_order_relaxed);
    }

    size_t capacity() const {
        return m_capacity;
    }

    // 解码线程写入（单生产者）
    size_t write(const uint8_t* data, size_t bytes) {
        size_t writePos = m_writePos.load(std::memory_order_relaxed);
        size_t readPos = m_readPos.load(std::memory_order_acquire);
        size_t available = m_capacity - (writePos - readPos);
        if (bytes > available) bytes = available;
        if (bytes == 0) return 0;

        size_t offset = writePos % m_capacity;
        size_t firstPart = std::min(bytes, m_capacity - offset);

        std::memcpy(m_buffer.data() + offset, data, firstPart);
        if (bytes > firstPart) {
            std::memcpy(m_buffer.data(), data + firstPart, bytes - firstPart);
        }

        m_writePos.fetch_add(bytes, std::memory_order_release);
        return bytes;
    }

    // 声卡回调读取（单消费者，全程无锁）
    size_t read(uint8_t* data, size_t bytes) {
        size_t writePos = m_writePos.load(std::memory_order_acquire);
        size_t readPos = m_readPos.load(std::memory_order_relaxed);
        size_t available = writePos - readPos;
        if (bytes > available) bytes = available;
        if (bytes == 0) return 0;

        size_t offset = readPos % m_capacity;
        size_t firstPart = std::min(bytes, m_capacity - offset);

        std::memcpy(data, m_buffer.data() + offset, firstPart);
        if (bytes > firstPart) {
            std::memcpy(data + firstPart, m_buffer.data(), bytes - firstPart);
        }

        m_readPos.fetch_add(bytes, std::memory_order_release);
        return bytes;
    }

private:
    std::vector<uint8_t> m_buffer;
    size_t m_capacity;
    // 强制缓存行对齐，防止多线程伪共享 (False Sharing)
    alignas(64) std::atomic<size_t> m_readPos{0};
    alignas(64) std::atomic<size_t> m_writePos{0};
};