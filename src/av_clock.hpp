#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <openmedia/format_api.hpp>

using namespace openmedia;
using SteadyClock = std::chrono::steady_clock;
using TimePoint = std::chrono::time_point<SteadyClock>;

// ---------------------------------------------------------------------------
// AVClock
//
// Optimized master clock that always interpolates between updates using
// the wall clock. This ensures the clock is "alive" and moves smoothly
// even if master updates (audio or manual) are infrequent.
// ---------------------------------------------------------------------------
class AVClock {
public:
  enum class Mode {
    AUDIO,
    WALL,
  };

  AVClock() = default;

  void setMode(Mode m) noexcept {
    mode_ = m;
  }

  auto mode() const noexcept -> Mode { return mode_; }

  void reset(double seconds = 0.0) noexcept {
    std::lock_guard<std::mutex> lock(mtx_);
    wall_ref_pts_sec_ = seconds;
    wall_ref_time_ = SteadyClock::now();
    pts_sec_.store(seconds, std::memory_order_release);
    paused_ = false;
  }

  void pause() noexcept {
    std::lock_guard<std::mutex> lock(mtx_);
    if (paused_) return;
    double current = internalMasterSeconds();
    pts_sec_.store(current, std::memory_order_release);
    wall_ref_pts_sec_ = current;
    paused_ = true;
  }

  void resume() noexcept {
    std::lock_guard<std::mutex> lock(mtx_);
    if (!paused_) return;
    wall_ref_time_ = SteadyClock::now();
    paused_ = false;
  }

  void setAudioSeconds(double seconds) noexcept {
    std::lock_guard<std::mutex> lock(mtx_);
    pts_sec_.store(seconds, std::memory_order_release);
    // When we get a master update, we reset the wall-clock reference
    // so interpolation continues from this new exact point.
    if (!paused_) {
      wall_ref_pts_sec_ = seconds;
      wall_ref_time_ = SteadyClock::now();
    }
  }

  void wallTick() noexcept {
    // wallTick is now mostly a no-op as masterSeconds() interpolates,
    // but we update the atomic for consistency with external observers.
    if (paused_) return;
    pts_sec_.store(masterSeconds(), std::memory_order_release);
  }

  auto masterSeconds() const noexcept -> double {
    std::lock_guard<std::mutex> lock(mtx_);
    return internalMasterSeconds();
  }

private:
  auto internalMasterSeconds() const noexcept -> double {
    if (paused_) return pts_sec_.load(std::memory_order_acquire);
    const auto now = SteadyClock::now();
    const double elapsed = std::chrono::duration<double>(now - wall_ref_time_).count();
    return wall_ref_pts_sec_ + elapsed;
  }

  std::atomic<double> pts_sec_ {0.0};
  Mode mode_ {Mode::WALL};

  double wall_ref_pts_sec_ = 0.0;
  TimePoint wall_ref_time_ = SteadyClock::now();
  bool paused_ = false;
  mutable std::mutex mtx_;
};
