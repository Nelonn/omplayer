#pragma once

#include <algorithm>
#include <cstring>
#include <mutex>
#include <type_traits>
#include <vector>

template<typename T>
class RingBuffer {
private:
  std::vector<T> buf_;
  size_t capacity_;
  size_t read_pos_ = 0;
  size_t write_pos_ = 0;
  size_t size_ = 0;
  mutable std::mutex mutex_;

public:
  explicit RingBuffer(size_t capacity)
      : buf_(capacity), capacity_(capacity) {}

  auto write(const T* data, size_t len) -> size_t {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t free = capacity_ - size_;
    size_t n = std::min(len, free);
    if (n == 0) return 0;

    size_t first_part = std::min(n, capacity_ - write_pos_);
    copyIn(write_pos_, data, first_part);

    if (n > first_part) {
      copyIn(0, data + first_part, n - first_part);
    }

    write_pos_ = (write_pos_ + n) % capacity_;
    size_ += n;
    return n;
  }

  auto read(T* dst, size_t len) -> size_t {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t n = std::min(len, size_);
    if (n == 0) return 0;

    size_t first_part = std::min(n, capacity_ - read_pos_);
    copyOut(dst, read_pos_, first_part);

    if (n > first_part) {
      copyOut(dst + first_part, 0, n - first_part);
    }

    read_pos_ = (read_pos_ + n) % capacity_;
    size_ -= n;
    return n;
  }

  void clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    read_pos_ = 0;
    write_pos_ = 0;
    size_ = 0;
  }

  auto currentSize() const -> size_t {
    std::lock_guard<std::mutex> lock(mutex_);
    return size_;
  }

  auto capacity() const -> size_t { return capacity_; }

  auto fillRatio() const -> double {
    std::lock_guard<std::mutex> lock(mutex_);
    return capacity_ > 0 ? static_cast<double>(size_) / capacity_ : 0.0;
  }

private:
  void copyIn(size_t pos, const T* src, size_t count) {
    if constexpr (std::is_trivially_copyable_v<T>) {
      std::memcpy(buf_.data() + pos, src, count * sizeof(T));
    } else {
      for (size_t i = 0; i < count; ++i) {
        buf_[pos + i] = src[i];
      }
    }
  }

  void copyOut(T* dst, size_t pos, size_t count) {
    if (!dst) return;

    if constexpr (std::is_trivially_copyable_v<T>) {
      std::memcpy(dst, buf_.data() + pos, count * sizeof(T));
    } else {
      for (size_t i = 0; i < count; ++i) {
        dst[i] = buf_[pos + i];
      }
    }
  }
};
