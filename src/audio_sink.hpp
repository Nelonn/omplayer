#pragma once

#include "av_clock.hpp"
#include "ring_buffer.hpp"

#include <SDL3/SDL.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

// ---------------------------------------------------------------------------
// AudioSink
//
// Owns one SDL audio device + SDL_AudioStream.  Pulls PCM from a RingBuffer
// on the audio callback thread and advances the shared AVClock.
//
// Buffering model (hysteresis):
//   fill < LOW_THRESH  → worker told to produce more
//   fill > HIGH_THRESH → worker sleeps
//   fill > START_THRESH on first fill → playback unpaused
// ---------------------------------------------------------------------------
class AudioSink {
public:
  // Thresholds as fractions of the ring buffer capacity.
  static constexpr double kLowThresh = 0.20;
  static constexpr double kHighThresh = 0.80;
  static constexpr double kStartThresh = 0.30;

  ~AudioSink() { close(); }

  // -----------------------------------------------------------------------
  // Open the device for a given PCM format.
  // `clock` must outlive AudioSink.
  // -----------------------------------------------------------------------
  auto open(SDL_AudioFormat sdl_fmt, int channels, int sample_rate,
            size_t bytes_per_sample, AVClock* clock) -> bool {
    close();

    clock_ = clock;
    sample_rate_ = sample_rate;
    channels_ = channels;
    bps_ = bytes_per_sample;
    frame_bytes_ = bps_ * static_cast<size_t>(channels);

    // 2-second ring buffer
    ring_ = std::make_unique<RingBuffer<uint8_t>>(
        static_cast<size_t>(sample_rate) * 2 * frame_bytes_);

    SDL_AudioSpec spec {};
    spec.format = sdl_fmt;
    spec.channels = channels;
    spec.freq = sample_rate;

    device_ = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, nullptr);
    if (!device_) {
      SDL_Log("[AudioSink] OpenAudioDevice: %s", SDL_GetError());
      return false;
    }

    stream_ = SDL_CreateAudioStream(&spec, &spec);
    if (!stream_) {
      SDL_Log("[AudioSink] CreateAudioStream: %s", SDL_GetError());
      close();
      return false;
    }

    SDL_SetAudioStreamGetCallback(stream_, audioCallbackS, this);
    if (!SDL_BindAudioStream(device_, stream_)) {
      SDL_Log("[AudioSink] BindAudioStream: %s", SDL_GetError());
      close();
      return false;
    }

    SDL_SetAudioDeviceGain(device_, gain_);
    // Device starts paused until buffer is primed.
    SDL_PauseAudioDevice(device_);
    open_ = true;
    started_ = false;
    return true;
  }

  void close() {
    if (stream_) {
      SDL_DestroyAudioStream(stream_);
      stream_ = nullptr;
    }
    if (device_) {
      SDL_CloseAudioDevice(device_);
      device_ = 0;
    }
    open_ = started_ = false;
  }

  // Push interleaved PCM bytes into the ring buffer.
  // Returns bytes written (may be partial if full).
  auto pushPcm(const uint8_t* data, size_t len) -> size_t {
    if (!ring_) return 0;
    size_t written = ring_->write(data, len);
    {
      std::lock_guard<std::mutex> lock(sync_mutex_);
      total_bytes_pushed_ += written;
    }
    return written;
  }

  // Poll buffering state; returns true once playback has started.
  // Call this after pushing audio data.
  auto tickBuffering(double current_seconds) -> bool {
    if (!open_ || started_ || !ring_) return false;
    const double ratio = ring_->fillRatio();
    if (ratio >= kStartThresh) {
      {
        std::lock_guard<std::mutex> lock(sync_mutex_);
        if (start_pts_ < 0) start_pts_ = current_seconds; // anchor for updateClock
      }
      SDL_ResumeAudioDevice(device_);
      started_ = true;
      SDL_Log("[AudioSink] Playback started (fill=%.1f%%)", ratio * 100.0);
    }
    return started_;
  }

  void updateClock() {
    if (!started_ || !clock_ || !ring_) return;

    double start;
    uint64_t pushed;
    {
      std::lock_guard<std::mutex> lock(sync_mutex_);
      start = start_pts_;
      pushed = total_bytes_pushed_;
    }

    const size_t ring_queued = ring_->currentSize();
    const int stream_queued = SDL_GetAudioStreamQueued(stream_);
    const uint64_t total_in_pipe = static_cast<uint64_t>(ring_queued + (stream_queued > 0 ? stream_queued : 0));

    if (pushed >= total_in_pipe && sample_rate_ > 0 && frame_bytes_ > 0) {
      const uint64_t bytes_played = pushed - total_in_pipe;
      const double clock_sec = start + static_cast<double>(bytes_played) / (sample_rate_ * frame_bytes_);
      clock_->setAudioSeconds(clock_sec);
    }
  }

  // Pause/resume for seek.
  void pause() {
    if (device_) SDL_PauseAudioDevice(device_);
  }
  void resume() {
    if (device_ && started_) SDL_ResumeAudioDevice(device_);
  }

  void clearBuffer() {
    if (ring_) ring_->clear();
    {
      std::lock_guard<std::mutex> lock(sync_mutex_);
      start_pts_ = -1.0;
      total_bytes_pushed_ = 0;
    }
    started_ = false;
  }

  void setGain(float g) {
    gain_ = std::clamp(g, 0.0f, 1.5f);
    if (device_) SDL_SetAudioDeviceGain(device_, gain_);
  }

  auto gain() const -> float { return gain_; }
  auto isOpen() const -> bool { return open_; }
  auto started() const -> bool { return started_; }

  double start_pts_ = -1.0;
  uint64_t total_bytes_pushed_ = 0;
  mutable std::mutex sync_mutex_;

  std::atomic<double> last_stream_pts_sec_ {0.0};

  // True when worker should produce more audio data.
  auto needsData() const -> bool {
    return !ring_ || ring_->fillRatio() < kHighThresh;
  }

  auto fillRatio() const -> double {
    return ring_ ? ring_->fillRatio() : 0.0;
  }

private:
  static void SDLCALL audioCallbackS(void* userdata, SDL_AudioStream* stream,
                                     int need, int /*total*/) {
    static_cast<AudioSink*>(userdata)->audioCallback(stream, need);
  }

  void audioCallback(SDL_AudioStream* stream, int need) {
    if (!ring_ || !clock_) return;

    const size_t available = ring_->currentSize();
    const size_t to_read = std::min(available, static_cast<size_t>(need));

    if (to_read > 0) {
      tmp_buf_.resize(to_read);
      const size_t n = ring_->read(tmp_buf_.data(), to_read);
      SDL_PutAudioStreamData(stream, tmp_buf_.data(), static_cast<int>(n));
    } else {
      // Underrun – push silence to avoid SDL starvation.
      silence_buf_.assign(static_cast<size_t>(need), 0);
      SDL_PutAudioStreamData(stream, silence_buf_.data(), need);
    }
  }

  AVClock* clock_ = nullptr;
  SDL_AudioDeviceID device_ = 0;
  SDL_AudioStream* stream_ = nullptr;
  std::unique_ptr<RingBuffer<uint8_t>> ring_;

  int sample_rate_ = 0;
  int channels_ = 0;
  size_t bps_ = 0;         // bytes per sample
  size_t frame_bytes_ = 0; // bytes per interleaved PCM frame

  float gain_ = 1.0f;
  bool open_ = false;
  bool started_ = false;

  std::vector<uint8_t> tmp_buf_;
  std::vector<uint8_t> silence_buf_;
};
