#pragma once

#include <chrono>
#include <mutex>
#include <openmedia/format_api.hpp>

using namespace openmedia;
using SteadyClock = std::chrono::steady_clock;
using TimePoint = std::chrono::time_point<SteadyClock>;

// ---------------------------------------------------------------------------
// AVClock
//
// Strictly follows the master updates. Interpolates between updates using
// the wall clock to ensure smooth movement between ticks.
// ---------------------------------------------------------------------------
class AVClock {
public:
  enum class Mode { AUDIO, WALL };

  AVClock() = default;

  void setMode(Mode m) noexcept { mode_ = m; }
  Mode mode() const noexcept { return mode_; }

  void reset(double seconds = 0.0) noexcept {
    std::lock_guard<std::mutex> lock(mtx_);
    base_pts_ = seconds;
    wall_ref_ = SteadyClock::now();
    paused_ = true; // Start paused; wait for resume or audio start
  }

  void pause() noexcept {
    std::lock_guard<std::mutex> lock(mtx_);
    if (paused_) return;
    base_pts_ = internalMasterSeconds();
    paused_ = true;
  }

  void resume() noexcept {
    std::lock_guard<std::mutex> lock(mtx_);
    if (!paused_) return;
    wall_ref_ = SteadyClock::now();
    paused_ = false;
  }

  bool isPaused() const noexcept {
    std::lock_guard<std::mutex> lock(mtx_);
    return paused_;
  }

  void setAudioSeconds(double seconds) noexcept {
    std::lock_guard<std::mutex> lock(mtx_);
    base_pts_ = seconds;
    wall_ref_ = SteadyClock::now();
    // If we are getting audio updates, ensure we are not "paused"
    // so that interpolation works between these updates.
    paused_ = false;
  }

  // Legacy wallTick - no longer strictly needed for movement but kept for API compatibility.
  void wallTick() noexcept {}

  double masterSeconds() const noexcept {
    std::lock_guard<std::mutex> lock(mtx_);
    return internalMasterSeconds();
  }

private:
  double internalMasterSeconds() const noexcept {
    if (paused_) return base_pts_;
    const auto now = SteadyClock::now();
    const double elapsed = std::chrono::duration<double>(now - wall_ref_).count();
    return base_pts_ + elapsed;
  }

  double    base_pts_ = 0.0;
  TimePoint wall_ref_ = SteadyClock::now();
  bool      paused_   = true;
  Mode      mode_     = Mode::WALL;
  mutable std::mutex mtx_;
};
