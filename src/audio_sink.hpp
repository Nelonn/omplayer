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
  
  // Audio delay to account for SDL hardware buffer latency.
  // This ensures video syncs to what's actually heard, not what's in the buffer.
  //static constexpr double kAudioDelaySec = 0.080; // 80ms typical hardware latency
  static constexpr double kAudioDelaySec = 0.5;

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
    const int stream_queued_samples = SDL_GetAudioStreamQueued(stream_);
    const size_t stream_queued_bytes = stream_queued_samples > 0
        ? static_cast<size_t>(stream_queued_samples) * frame_bytes_
        : 0;
    const uint64_t total_in_pipe = static_cast<uint64_t>(ring_queued + stream_queued_bytes);

    /*if (pushed >= total_in_pipe && sample_rate_ > 0 && frame_bytes_ > 0) {
      const uint64_t bytes_played = pushed - total_in_pipe;
      double clock_sec = start + static_cast<double>(bytes_played) / (sample_rate_ * frame_bytes_);
      // Subtract audio delay to sync video to what's actually heard,
      // not what's still in SDL's hardware buffer.
      clock_sec -= kAudioDelaySec;
      clock_->setAudioSeconds(clock_sec);
    }*/
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
    bytes_played_.store(0, std::memory_order_relaxed);
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

      bytes_played_.fetch_add(n, std::memory_order_relaxed);

      double start;
      {
        std::lock_guard<std::mutex> lk(sync_mutex_);
        start = start_pts_;
      }

      if (start >= 0.0 && sample_rate_ > 0 && frame_bytes_ > 0) {
        const double clock_sec =
            start + static_cast<double>(bytes_played_.load(std::memory_order_relaxed))
                    / (sample_rate_ * frame_bytes_);
        clock_->setAudioSeconds(clock_sec);
      }
    }

    const size_t gap = static_cast<size_t>(need) - to_read;
    if (gap > 0) {
      silence_buf_.assign(gap, 0);
      SDL_PutAudioStreamData(stream, silence_buf_.data(), static_cast<int>(gap));
      bytes_played_.fetch_add(gap, std::memory_order_relaxed);
    }
  }

  AVClock* clock_ = nullptr;
  SDL_AudioDeviceID device_ = 0;
  SDL_AudioStream* stream_ = nullptr;
  std::unique_ptr<RingBuffer<uint8_t>> ring_;
  std::atomic<uint64_t> bytes_played_ {0};

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

// ---------------------------------------------------------------------------
// AudioSinkWithMixer
//
// Extension of AudioSink that uses AudioMixer for mixing multiple sources.
// The mixer's output is sent to the SDL audio device.
// ---------------------------------------------------------------------------
class AudioSinkWithMixer {
public:
  using SourceId = AudioMixer::SourceId;
  using SourceConfig = AudioMixer::SourceConfig;

  static constexpr double kAudioDelaySec = 0.5;

  ~AudioSinkWithMixer() { close(); }

  // Open the audio device with the specified output format
  auto open(SDL_AudioFormat sdl_fmt, int channels, int sample_rate,
            AVClock* clock) -> bool {
    close();

    clock_ = clock;
    sample_rate_ = sample_rate;
    channels_ = channels;

    // Configure mixer output format
    mixer_.setOutputFormat(sdl_fmt, channels, sample_rate);

    // Calculate frame bytes for clock calculation
    const size_t bps = getBytesPerSample(sdl_fmt);
    frame_bytes_ = bps * static_cast<size_t>(channels);

    // Create output buffer for mixed audio
    output_buffer_.resize(static_cast<size_t>(sample_rate) * 2 *
                          static_cast<size_t>(channels));

    SDL_AudioSpec spec {};
    spec.format = sdl_fmt;
    spec.channels = channels;
    spec.freq = sample_rate;

    device_ = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, nullptr);
    if (!device_) {
      SDL_Log("[AudioSinkWithMixer] OpenAudioDevice: %s", SDL_GetError());
      return false;
    }

    stream_ = SDL_CreateAudioStream(&spec, &spec);
    if (!stream_) {
      SDL_Log("[AudioSinkWithMixer] CreateAudioStream: %s", SDL_GetError());
      close();
      return false;
    }

    SDL_SetAudioStreamGetCallback(stream_, audioCallbackS, this);
    if (!SDL_BindAudioStream(device_, stream_)) {
      SDL_Log("[AudioSinkWithMixer] BindAudioStream: %s", SDL_GetError());
      close();
      return false;
    }

    SDL_SetAudioDeviceGain(device_, gain_);
    SDL_PauseAudioDevice(device_);
    open_ = started_ = false;
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

  // Add a new audio source to the mixer
  auto addSource(const SourceConfig& config) -> std::optional<SourceId> {
    return mixer_.addSource(config);
  }

  // Remove a source from the mixer
  void removeSource(SourceId id) {
    mixer_.removeSource(id);
  }

  // Push interleaved float samples to a source
  auto pushSamples(SourceId id, const float* samples, size_t num_samples) -> size_t {
    return mixer_.pushSamples(id, samples, num_samples);
  }

  // Push interleaved PCM bytes to a source
  auto pushPcmBytes(SourceId id, const uint8_t* data, size_t len) -> size_t {
    return mixer_.pushPcmBytes(id, data, len);
  }

  // Set volume for a specific source
  void setSourceVolume(SourceId id, float volume) {
    mixer_.setSourceVolume(id, volume);
  }

  auto sourceVolume(SourceId id) const -> float {
    return mixer_.sourceVolume(id);
  }

  // Pause/resume a specific source
  void setSourcePaused(SourceId id, bool paused) {
    mixer_.setSourcePaused(id, paused);
  }

  auto isSourcePaused(SourceId id) const -> bool {
    return mixer_.isSourcePaused(id);
  }

  // Set master volume
  void setMasterVolume(float volume) {
    mixer_.setMasterVolume(volume);
    gain_ = volume;
    if (device_) SDL_SetAudioDeviceGain(device_, gain_);
  }

  auto masterVolume() const -> float { return mixer_.masterVolume(); }

  // Clear buffer for a specific source (useful for seeking)
  void clearSourceBuffer(SourceId id) {
    mixer_.clearSourceBuffer(id);
  }

  // Clear all source buffers
  void clearAllBuffers() {
    mixer_.clearAllSources();
    {
      std::lock_guard<std::mutex> lock(sync_mutex_);
      start_pts_ = -1.0;
      total_bytes_pushed_ = 0;
    }
    bytes_played_.store(0, std::memory_order_relaxed);
    started_ = false;
  }

  // Pause/resume for seek
  void pause() {
    if (device_) SDL_PauseAudioDevice(device_);
  }

  void resume() {
    if (device_ && started_) SDL_ResumeAudioDevice(device_);
  }

  auto isOpen() const -> bool { return open_; }
  auto started() const -> bool { return started_; }
  auto sourceCount() const -> size_t { return mixer_.sourceCount(); }

  // Get the mixer for direct access
  auto& mixer() { return mixer_; }
  const auto& mixer() const { return mixer_; }

private:
  static void SDLCALL audioCallbackS(void* userdata, SDL_AudioStream* stream,
                                     int need, int /*total*/) {
    static_cast<AudioSinkWithMixer*>(userdata)->audioCallback(stream, need);
  }

  void audioCallback(SDL_AudioStream* stream, int need) {
    if (!clock_) return;

    const size_t frames_needed = static_cast<size_t>(need) /
                                 (static_cast<size_t>(channels_) * getBytesPerSample(mixer_.outputFormat()));

    // Mix audio from all sources
    mixer_.mix(output_buffer_.data(), frames_needed);

    // Convert to bytes and send to stream
    const size_t bytes_to_write = frames_needed * frame_bytes_;
    SDL_PutAudioStreamData(stream, reinterpret_cast<uint8_t*>(output_buffer_.data()),
                           static_cast<int>(bytes_to_write));

    bytes_played_.fetch_add(bytes_to_write, std::memory_order_relaxed);

    // Update clock
    double start;
    {
      std::lock_guard<std::mutex> lk(sync_mutex_);
      start = start_pts_;
    }

    if (start >= 0.0 && sample_rate_ > 0 && frame_bytes_ > 0) {
      const double clock_sec =
          start + static_cast<double>(bytes_played_.load(std::memory_order_relaxed)) /
                      (static_cast<double>(sample_rate_) * static_cast<double>(frame_bytes_));
      clock_->setAudioSeconds(clock_sec);
    }
  }

  static auto getBytesPerSample(SDL_AudioFormat fmt) -> size_t {
    if (SDL_AUDIO_ISF32(fmt)) return 4;
    if (SDL_AUDIO_ISF64(fmt)) return 8;
    if (SDL_AUDIO_ISS32(fmt)) return 4;
    if (SDL_AUDIO_ISS16(fmt)) return 2;
    if (SDL_AUDIO_ISU8(fmt)) return 1;
    return 4;
  }

  AVClock* clock_ = nullptr;
  SDL_AudioDeviceID device_ = 0;
  SDL_AudioStream* stream_ = nullptr;

  AudioMixer mixer_;
  std::vector<float> output_buffer_;

  std::atomic<uint64_t> bytes_played_ {0};

  int sample_rate_ = 0;
  int channels_ = 0;
  size_t frame_bytes_ = 0;

  float gain_ = 1.0f;
  bool open_ = false;
  bool started_ = false;

  double start_pts_ = -1.0;
  uint64_t total_bytes_pushed_ = 0;
  mutable std::mutex sync_mutex_;
};
