#pragma once

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <queue>
#include <vector>

struct VideoFrame {
  std::vector<uint8_t> y_plane;
  std::vector<uint8_t> u_plane;
  std::vector<uint8_t> v_plane;
  std::vector<uint8_t> rgba_plane;
  int y_stride = 0;
  int u_stride = 0;
  int v_stride = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  int64_t pts = 0;
  double pts_sec = 0.0;
};

class FrameQueue {
public:
  explicit FrameQueue(size_t capacity = 8)
      : capacity_(capacity) {}

  auto blockingPush(VideoFrame frame) -> bool {
    std::unique_lock lock(mutex_);
    not_full_cv_.wait(lock, [&] {
      return aborted_ || queue_.size() < capacity_;
    });
    if (aborted_) return false;
    queue_.push(std::move(frame));
    not_empty_cv_.notify_one();
    return true;
  }

  auto blockingPop() -> std::optional<VideoFrame> {
    std::unique_lock lock(mutex_);
    not_empty_cv_.wait(lock, [&] {
      return aborted_ || !queue_.empty();
    });
    if (aborted_ && queue_.empty()) return std::nullopt;
    VideoFrame vf = std::move(queue_.front());
    queue_.pop();
    not_full_cv_.notify_one();
    return vf;
  }

  template<typename Fn>
  auto peekPop(Fn&& decision) -> std::optional<VideoFrame> {
    std::lock_guard lock(mutex_);
    if (aborted_ || queue_.empty()) return std::nullopt;
    if (!decision(queue_.front().pts_sec)) return std::nullopt;
    VideoFrame vf = std::move(queue_.front());
    queue_.pop();
    not_full_cv_.notify_one();
    return vf;
  }

  auto tryPush(VideoFrame frame) -> bool {
    std::lock_guard lock(mutex_);
    if (aborted_ || queue_.size() >= capacity_) return false;
    queue_.push(std::move(frame));
    not_empty_cv_.notify_one();
    return true;
  }

  auto tryPop() -> std::optional<VideoFrame> {
    std::lock_guard lock(mutex_);
    if (aborted_ || queue_.empty()) return std::nullopt;
    VideoFrame vf = std::move(queue_.front());
    queue_.pop();
    not_full_cv_.notify_one();
    return vf;
  }

  auto frontPtsSec() const -> std::optional<double> {
    std::lock_guard lock(mutex_);
    if (queue_.empty()) return std::nullopt;
    return queue_.front().pts_sec;
  }

  void abort() {
    std::lock_guard lock(mutex_);
    aborted_ = true;
    while (!queue_.empty()) queue_.pop();
    not_empty_cv_.notify_all();
    not_full_cv_.notify_all();
  }

  void reset() {
    std::lock_guard lock(mutex_);
    aborted_ = false;
    while (!queue_.empty()) queue_.pop();
  }

  void flush() { abort(); }
  void resetFlush() { reset(); }

  auto isFlushing() const -> bool {
    std::lock_guard lock(mutex_);
    return aborted_;
  }

  auto size() const -> size_t {
    std::lock_guard lock(mutex_);
    return queue_.size();
  }

  auto capacity() const -> size_t { return capacity_; }

  auto empty() const -> bool {
    std::lock_guard lock(mutex_);
    return queue_.empty();
  }

private:
  std::queue<VideoFrame> queue_;
  mutable std::mutex mutex_;
  std::condition_variable not_full_cv_;
  std::condition_variable not_empty_cv_;
  size_t capacity_;
  bool aborted_ = false;
};
